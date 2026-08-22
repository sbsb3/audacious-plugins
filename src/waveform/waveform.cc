/*
 * waveform.cc
 * Waveform Seekbar plugin for Audacious
 *
 * Port of the DeaDBeeF plugin ddb_waveform_seekbar: a dockable panel that
 * shows a min/max/RMS waveform of the playing track in place of a plain
 * seek slider, with click/drag-to-seek, scroll-to-seek, a right-click menu
 * to set SKIP trim points (shared with the "jumpin" plugin -- see
 * tagstore.h), right-drag to move the SKIP line, and a right-swipe to
 * either edge to change tracks (or, with a configurable modifier key held,
 * to switch to and start playing the previous/next playlist instead).
 *
 * Differences from the DeaDBeeF original, forced by API differences (see
 * the port's plan document for the full analysis):
 *
 *  - DeaDBeeF exposes DB_decoder_t::open/init/read so any plugin can pull
 *    raw PCM from a file on demand; that's how the original built its
 *    waveform. Audacious's InputPlugin only knows how to decode straight
 *    into the active output device. This port bundles its own minimal
 *    FFmpeg-based decode path instead -- see decode.h/decode.cc.
 *  - DeaDBeeF's design-mode widget system (drag a widget into a layout) has
 *    no Audacious equivalent; this is a normal dockable GeneralPlugin panel
 *    instead (same pattern as the "search-tool" plugin).
 *  - Peak decoding runs on a background std::thread (DeaDBeeF used its own
 *    thread_start()/mutex API); the result is handed back to the main
 *    thread through a small mutex-guarded slot, polled by the same Hz10
 *    timer that redraws the playback cursor.
 *  - SKIP tags are read/written through the same sidecar store as the
 *    "jumpin" plugin (tagstore.h) rather than DeaDBeeF's free-form
 *    pl_find_meta() custom tags.
 *  - Per-channel/per-region RGBA color configuration (fg/bg/played/ruler/
 *    rms/skip, each with its own alpha) is dropped in favor of colors
 *    derived from the current GTK theme (same approach as the built-in
 *    cairo-spectrum visualization), rather than inventing a custom color
 *    picker UI for the preferences pane.
 *  - The original could show a live-updating waveform while a long file was
 *    still being decoded for the first time (periodic partial redraws from
 *    the decode thread). This port decodes to completion in the background
 *    and swaps in the finished result once; scope trim for v1.
 */

#include <math.h>
#include <string.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include <libaudcore/audstrings.h>
#include <libaudcore/drct.h>
#include <libaudcore/hook.h>
#include <libaudcore/i18n.h>
#include <libaudcore/plugin.h>
#include <libaudcore/playlist.h>
#include <libaudcore/preferences.h>
#include <libaudcore/runtime.h>

#include "cache.h"
#include "decode.h"
#include "render.h"
#include "ruler.h"
#include "skiptag.h"
#include "tagstore.h"
#include "wavetypes.h"

#ifdef USE_GTK
#include <libaudgui/gtk-compat.h>
#include <libaudgui/libaudgui-gtk.h>

// --- custom color preferences ---------------------------------------------
//
// preferences.h has no built-in color-picker widget type, so each color row
// is a small hand-built GtkColorButton (with alpha enabled, i.e. opacity)
// wired up through PreferencesWidget's WidgetCustomGTK escape hatch -- same
// idea as blur_scope.cc's single color chooser, just parameterized over
// which of the five colors a given row edits and packed into an hbox with
// its own label (CustomGTK ignores PreferencesWidget::label entirely, so
// the label has to be built into the returned widget).
//
// Each color is stored as four separate 0-255 ints (R, G, B, A) rather than
// one packed 32-bit value: it sidesteps sign-extension headaches with
// aud::numeric_string's int template parameter for alpha values >= 0x80,
// and keeps config.cc-side get/set trivial.

struct ColorKeys
{
    const char *r, *g, *b, *a;
};

static const ColorKeys FG_KEYS = {"color_fg_r", "color_fg_g", "color_fg_b", "color_fg_a"};
static const ColorKeys RMS_KEYS = {"color_rms_r", "color_rms_g", "color_rms_b", "color_rms_a"};
static const ColorKeys BG_KEYS = {"color_bg_r", "color_bg_g", "color_bg_b", "color_bg_a"};
static const ColorKeys PB_KEYS = {"color_pb_r", "color_pb_g", "color_pb_b", "color_pb_a"};
static const ColorKeys SKIP_START_KEYS = {"color_skip_start_r", "color_skip_start_g",
                                           "color_skip_start_b", "color_skip_start_a"};
static const ColorKeys SKIP_END_KEYS = {"color_skip_end_r", "color_skip_end_g",
                                         "color_skip_end_b", "color_skip_end_a"};
static const ColorKeys RULER_KEYS = {"color_rlr_r", "color_rlr_g", "color_rlr_b", "color_rlr_a"};

static void color_row_set_cb(GtkWidget * chooser, void * user_data)
{
    auto keys = (const ColorKeys *)user_data;
    int r, g, b, a;

#ifdef USE_GTK3
    GdkRGBA rgba;
    gtk_color_chooser_get_rgba((GtkColorChooser *)chooser, &rgba);
    r = (int)lround(rgba.red * 255);
    g = (int)lround(rgba.green * 255);
    b = (int)lround(rgba.blue * 255);
    a = (int)lround(rgba.alpha * 255);
#else
    GdkColor c;
    gtk_color_button_get_color((GtkColorButton *)chooser, &c);
    r = c.red / 257;
    g = c.green / 257;
    b = c.blue / 257;
    a = gtk_color_button_get_alpha((GtkColorButton *)chooser) / 257;
#endif

    aud_set_int("waveform", keys->r, r);
    aud_set_int("waveform", keys->g, g);
    aud_set_int("waveform", keys->b, b);
    aud_set_int("waveform", keys->a, a);
    // No explicit redraw request needed here -- the panel's Hz10 timer
    // (on_timer_tick) queues a redraw every tick regardless, so a change
    // shows up within ~100ms whether or not the panel is the focused widget.
}

static GtkWidget * make_color_row(const char * label_text, const ColorKeys & keys)
{
    int r = aud_get_int("waveform", keys.r);
    int g = aud_get_int("waveform", keys.g);
    int b = aud_get_int("waveform", keys.b);
    int a = aud_get_int("waveform", keys.a);

    GtkWidget * hbox = audgui_hbox_new(6);
    gtk_box_pack_start((GtkBox *)hbox, gtk_label_new(_(label_text)), false, false, 0);

    GtkWidget * chooser;
#ifdef USE_GTK3
    GdkRGBA rgba = {r / 255.0, g / 255.0, b / 255.0, a / 255.0};
    chooser = gtk_color_button_new_with_rgba(&rgba);
    gtk_color_chooser_set_use_alpha((GtkColorChooser *)chooser, true);
#else
    GdkColor gdk_color = {0, (guint16)(r * 257), (guint16)(g * 257), (guint16)(b * 257)};
    chooser = gtk_color_button_new_with_color(&gdk_color);
    gtk_color_button_set_use_alpha((GtkColorButton *)chooser, true);
    gtk_color_button_set_alpha((GtkColorButton *)chooser, (guint16)(a * 257));
#endif
    gtk_box_pack_start((GtkBox *)hbox, chooser, false, false, 0);
    g_signal_connect(chooser, "color-set", (GCallback)color_row_set_cb, (void *)&keys);

    return hbox;
}

static void * make_fg_row() { return make_color_row(N_("Waveform:"), FG_KEYS); }
static void * make_rms_row() { return make_color_row(N_("RMS overlay:"), RMS_KEYS); }
static void * make_bg_row() { return make_color_row(N_("Background:"), BG_KEYS); }
static void * make_pb_row() { return make_color_row(N_("Playhead:"), PB_KEYS); }
static void * make_skip_start_row() { return make_color_row(N_("SKIP start marker:"), SKIP_START_KEYS); }
static void * make_skip_end_row() { return make_color_row(N_("SKIP end marker:"), SKIP_END_KEYS); }
static void * make_rlr_row() { return make_color_row(N_("Ruler text/ticks:"), RULER_KEYS); }

#endif // USE_GTK

class Waveform : public GeneralPlugin
{
public:
    static const PreferencesWidget widgets[];
    static const PluginPreferences prefs;

    static constexpr PluginInfo info = {
        N_("Waveform Seekbar"),
        PACKAGE,
        nullptr,
        &prefs,
        PluginGLibOnly
    };

    constexpr Waveform() : GeneralPlugin(info, false) {}

    bool init() override;
    void cleanup() override;

#ifdef USE_GTK
    void * get_gtk_widget() override;
#endif
};

EXPORT Waveform aud_plugin_instance;

static const char * const waveform_defaults[] = {
    "log_enabled", "FALSE",
    "mix_to_mono", "FALSE",
    "display_rms", "TRUE",
    "display_ruler", "TRUE",
    "dim_played", "TRUE",
    "fill_waveform", "TRUE",
    "soundcloud_style", "FALSE",
    "bars_style", "FALSE",
    "display_skip_markers", "TRUE",
    "cache_enabled", "TRUE",
    "scroll_enabled", "TRUE",
    "swipe_jump_in", "FALSE",
    "swipe_playlist_modifier", "0",
    "num_samples", "3000",
    "max_file_minutes", "60",
    // The original hardcoded this at 8pt, which reads as nearly illegible at
    // typical screen DPI (see the ruler in draw_ruler()). Default bumped up
    // and made configurable.
    "ruler_font_size", "12",

    // Custom colors (all 0-255, including alpha). Unused unless
    // "custom_colors" is on -- otherwise colors are derived from the
    // current GTK theme, see theme_colors(). Defaults below are just a
    // reasonable starting point once the user opts in.
    "custom_colors", "FALSE",
    "color_fg_r", "78", "color_fg_g", "78", "color_fg_b", "78", "color_fg_a", "255",
    "color_rms_r", "19", "color_rms_g", "19", "color_rms_b", "19", "color_rms_a", "255",
    "color_bg_r", "195", "color_bg_g", "195", "color_bg_b", "195", "color_bg_a", "255",
    "color_pb_r", "0", "color_pb_g", "255", "color_pb_b", "0", "color_pb_a", "120",
    "color_skip_start_r", "255", "color_skip_start_g", "156", "color_skip_start_b", "0", "color_skip_start_a", "255",
    "color_skip_end_r", "255", "color_skip_end_g", "50", "color_skip_end_b", "50", "color_skip_end_a", "255",
    "color_rlr_r", "78", "color_rlr_g", "78", "color_rlr_b", "78", "color_rlr_a", "160",
    nullptr
};

// Split across a notebook so the Settings dialog stays short enough that
// the Close button remains on-screen. The plugin-prefs window is not
// resizable and has no scroller (see audgui_show_plugin_prefs()).
static const PreferencesWidget display_page[] = {
    WidgetCheck(N_("Show RMS overlay"), WidgetBool("waveform", "display_rms")),
    WidgetCheck(N_("Dim already-played portion of waveform"), WidgetBool("waveform", "dim_played")),
    WidgetCheck(N_("Show time ruler"), WidgetBool("waveform", "display_ruler")),
    WidgetSpin(N_("Time ruler font size:"), WidgetInt("waveform", "ruler_font_size"),
               {6, 24, 1, N_("pt")}, WIDGET_CHILD),
    WidgetCheck(N_("Show SKIP start/end markers"), WidgetBool("waveform", "display_skip_markers")),
    WidgetCheck(N_("Use logarithmic scale"), WidgetBool("waveform", "log_enabled")),
    WidgetCheck(N_("Fill waveform"), WidgetBool("waveform", "fill_waveform")),
    WidgetCheck(N_("Bars style (instead of filled outline)"), WidgetBool("waveform", "bars_style")),
    WidgetCheck(N_("SoundCloud-style gradient"), WidgetBool("waveform", "soundcloud_style")),
    WidgetCheck(N_("Downmix to mono"), WidgetBool("waveform", "mix_to_mono")),
};

static const ComboItem swipe_modifier_options[] = {
    ComboItem(N_("None (switch track in the current playlist)"), 0),
    ComboItem(N_("Shift"), 1),
    ComboItem(N_("Ctrl"), 2),
    ComboItem(N_("Alt"), 3),
    ComboItem(N_("Super"), 4),
};

static const PreferencesWidget behavior_page[] = {
    WidgetCheck(N_("Seek with scroll wheel"), WidgetBool("waveform", "scroll_enabled")),
    WidgetCheck(N_("Use Jump In when swiping to previous/next track"),
                WidgetBool("waveform", "swipe_jump_in")),
    WidgetLabel(N_("Requires the Jump In plugin. Swiping to either edge then "
                   "lands in the new track at the configured Jump In offset."),
                WIDGET_CHILD),
    WidgetCombo(N_("Held while edge-swiping, switch playlist instead:"),
                WidgetInt("waveform", "swipe_playlist_modifier"),
                {{swipe_modifier_options}}),
    WidgetLabel(N_("Swiping to either edge while holding this key switches to the "
                   "previous/next playlist and starts playing it, instead of "
                   "changing tracks within the current playlist."),
                WIDGET_CHILD),
    WidgetLabel(N_("<b>Peak cache</b>")),
    WidgetCheck(N_("Cache decoded waveforms on disk"), WidgetBool("waveform", "cache_enabled")),
    WidgetSpin(N_("Peak resolution (samples per track):"), WidgetInt("waveform", "num_samples"),
               {200, 20000, 100}, WIDGET_CHILD),
    WidgetSpin(N_("Skip files longer than (0 = no limit):"), WidgetInt("waveform", "max_file_minutes"),
               {0, 600, 1, N_("minutes")}, WIDGET_CHILD),
    WidgetLabel(N_("<b>SKIP / swipe</b>")),
    WidgetLabel(N_("Right-click to set SKIP start/end (shared with Jump In). "
                   "Hold and drag to move the SKIP line; swipe to either edge "
                   "for previous/next track.")),
};

static const PreferencesWidget colors_page[] = {
    WidgetCheck(N_("Use custom colors instead of the GTK theme"),
                WidgetBool("waveform", "custom_colors")),
    WidgetCustomGTK(make_fg_row, WIDGET_CHILD),
    WidgetCustomGTK(make_rms_row, WIDGET_CHILD),
    WidgetCustomGTK(make_bg_row, WIDGET_CHILD),
    WidgetCustomGTK(make_pb_row, WIDGET_CHILD),
    WidgetCustomGTK(make_skip_start_row, WIDGET_CHILD),
    WidgetCustomGTK(make_skip_end_row, WIDGET_CHILD),
    WidgetCustomGTK(make_rlr_row, WIDGET_CHILD),
    WidgetLabel(N_("Each color swatch's opacity (alpha) can be set from within "
                   "the color picker dialog."), WIDGET_CHILD),
};

static const NotebookTab waveform_tabs[] = {
    {N_("Display"), {display_page}},
    {N_("Behavior"), {behavior_page}},
    {N_("Colors"), {colors_page}},
};

const PreferencesWidget Waveform::widgets[] = {
    WidgetNotebook({{waveform_tabs}})
};

const PluginPreferences Waveform::prefs = {{widgets}};

bool Waveform::init()
{
    aud_config_set_defaults("waveform", waveform_defaults);
    WaveCache::open_db(aud_get_path(AudPath::UserDir));
    return true;
}

void Waveform::cleanup() { WaveCache::close_db(); }

#ifdef USE_GTK

// --- state (single panel instance; see class doc comment) ---------------

static GtkWidget * s_area = nullptr;  // waveform drawing area
static GtkWidget * s_ruler = nullptr; // time ruler drawing area
static GtkWidget * s_popup = nullptr; // right-click SKIP menu

static WaveData s_wave; // current track's raw peak data, at a fixed
                        // resolution (see aud_get_int("waveform","num_samples"))

static String s_cur_filename; // main-thread only
static std::string s_cur_key; // plain copy of the above, safe to compare
                              // against from adopt_pending_result()
static double s_cur_duration = 0; // seconds

static bool s_seeking = false;
static double s_seek_x = 0;
static bool s_popup_marker_active = false;
static double s_popup_click_x = 0;

// Right-button gesture: drag moves the SKIP placement line (or an existing
// start/end marker if the press landed on one). Swiping that drag to the
// left/right edge of the widget skips to the previous/next track instead
// of opening the menu.
enum class SkipDragTarget
{
    None,
    Start,
    End
};

static bool s_rbtn_down = false;
static double s_rbtn_start_x = 0;
static bool s_edge_gesture_done = false;
static SkipDragTarget s_skip_drag = SkipDragTarget::None;

// How close a press must be to an existing SKIP marker to grab it, in
// widget pixels. Markers are a couple of pixels wide, so this is the
// actual hit target.
static constexpr double SKIP_GRAB_PX = 10.0;

// Click vs. drag: below this, a right-release is a click (open the menu)
// even if the press landed on a marker.
static constexpr double SKIP_CLICK_PX = 6.0;

// Background build coordination. String/StringBuf are not safe to touch
// off the main thread (libaudcore's string pool takes no locks), so
// everything crossing into build_thread_func() is a plain std::string.
static std::mutex s_build_mutex;
static WaveData s_pending;
static std::string s_pending_key;
static std::atomic<bool> s_build_done{false};

static void reset_wave()
{
    delete[] s_wave.data;
    s_wave = WaveData();
}

static void queue_all_draws()
{
    if (s_area)
        gtk_widget_queue_draw(s_area);
    if (s_ruler)
        gtk_widget_queue_draw(s_ruler);
}

static void build_thread_func(std::string key, std::string path, int num_buckets, bool cache_on,
                               double start_sec, double end_sec)
{
    WaveData wd;
    bool ok = waveform_decode_build(path.c_str(), num_buckets, wd, start_sec, end_sec);
    if (ok && cache_on)
        WaveCache::write(key.c_str(), wd.data, wd.data_len, wd.channels);

    std::lock_guard<std::mutex> lock(s_build_mutex);
    delete[] s_pending.data;
    s_pending = ok ? wd : WaveData();
    s_pending_key = std::move(key);
    s_build_done = true;
}

static void adopt_pending_result()
{
    if (!s_build_done.exchange(false))
        return;

    std::lock_guard<std::mutex> lock(s_build_mutex);
    if (s_pending_key == s_cur_key && s_pending.channels > 0)
    {
        reset_wave();
        s_wave = s_pending;
        s_pending = WaveData();
        queue_all_draws();
    }
    else
    {
        delete[] s_pending.data;
        s_pending = WaveData();
    }
}

static void load_track(const String & filename)
{
    reset_wave();
    s_cur_filename = filename;
    s_cur_key = filename ? std::string((const char *)filename) : std::string();
    s_cur_duration = aud_drct_get_length() / 1000.0;
    s_popup_marker_active = false;
    s_seeking = false;
    s_skip_drag = SkipDragTarget::None;

    if (!filename || !str_has_prefix_nocase(filename, "file://"))
    {
        queue_all_draws();
        return;
    }

    // Cuesheet tracks (and anything else made of several logical tracks
    // packed into one physical file) report their own virtual URI as the
    // playlist filename -- e.g. "file:///path/album.cue?3" -- but the
    // audio itself lives in a different file, given by Tuple::AudioFile,
    // with this track's [StartTime, StartTime + duration) as its slice of
    // it (see src/cue/cue.cc). Decode that underlying file, trimmed to just
    // this track's segment, instead of trying to open the virtual URI
    // itself (which isn't a real, openable file) or waveform-ing the
    // entire physical file it points to.
    Tuple tuple = aud_drct_get_tuple();
    String audio_file = tuple.get_str(Tuple::AudioFile);
    bool is_segment = audio_file && audio_file[0];
    const String & decode_uri = is_segment ? audio_file : filename;
    double start_sec = is_segment ? tuple.get_int(Tuple::StartTime) / 1000.0 : 0.0;
    double end_sec = is_segment ? start_sec + s_cur_duration : -1.0;

    int max_minutes = aud_get_int("waveform", "max_file_minutes");
    if (max_minutes > 0 && s_cur_duration > max_minutes * 60.0)
    {
        queue_all_draws();
        return;
    }

    bool cache_on = aud_get_bool("waveform", "cache_enabled");
    if (cache_on && WaveCache::read(s_cur_key.c_str(), s_wave))
    {
        queue_all_draws();
        return;
    }

    StringBuf path = uri_to_filename(decode_uri);
    if (!path)
    {
        queue_all_draws();
        return;
    }

    int num_buckets = aud_get_int("waveform", "num_samples");
    std::thread(build_thread_func, s_cur_key, std::string((const char *)path), num_buckets, cache_on,
                start_sec, end_sec)
        .detach();

    queue_all_draws();
}

static void on_playback_ready(void *, void *) { load_track(aud_drct_get_filename()); }

static void on_playback_stop(void *, void *)
{
    reset_wave();
    s_cur_filename = String();
    s_cur_key.clear();
    s_cur_duration = 0;
    queue_all_draws();
}

static void on_timer_tick(void *)
{
    adopt_pending_result();
    if (s_area)
        gtk_widget_queue_draw(s_area);
    if (s_ruler)
        gtk_widget_queue_draw(s_ruler);
}

// --- theming --------------------------------------------------------------

static WaveColors theme_colors(GtkWidget * widget)
{
    WaveColors c;
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    GtkStyle * style = gtk_widget_get_style(widget);
    GdkColor fg = style->text[GTK_STATE_NORMAL];
    GdkColor bg = style->base[GTK_STATE_NORMAL];
    GdkColor sel = style->base[GTK_STATE_SELECTED];
G_GNUC_END_IGNORE_DEPRECATIONS

    c.fg = {fg.red / 65535.0, fg.green / 65535.0, fg.blue / 65535.0, 1.0};
    c.bg = {bg.red / 65535.0, bg.green / 65535.0, bg.blue / 65535.0, 1.0};
    c.pb = {sel.red / 65535.0, sel.green / 65535.0, sel.blue / 65535.0, 0.9};
    c.rlr = {fg.red / 65535.0, fg.green / 65535.0, fg.blue / 65535.0, 0.4};
    c.rms = {fg.red / 65535.0, fg.green / 65535.0, fg.blue / 65535.0, 0.5};
    c.skip_start = {sel.red / 65535.0, sel.green / 65535.0, sel.blue / 65535.0, 0.8};
    c.skip_end = c.skip_start;
    return c;
}

// User-configured colors (see "Colors" prefs section / make_color_row above)
// instead of theme-derived ones. The ruler text/ticks have their own picker
// (RULER_KEYS), independent of the waveform fill color.
static WaveColors custom_colors()
{
    auto get = [](const ColorKeys & k)
    {
        return Color{aud_get_int("waveform", k.r) / 255.0, aud_get_int("waveform", k.g) / 255.0,
                     aud_get_int("waveform", k.b) / 255.0, aud_get_int("waveform", k.a) / 255.0};
    };

    WaveColors c;
    c.fg = get(FG_KEYS);
    c.rms = get(RMS_KEYS);
    c.bg = get(BG_KEYS);
    c.pb = get(PB_KEYS);
    c.rlr = get(RULER_KEYS);
    c.skip_start = get(SKIP_START_KEYS);
    c.skip_end = get(SKIP_END_KEYS);
    return c;
}

static WaveColors resolve_colors(GtkWidget * widget)
{
    return aud_get_bool("waveform", "custom_colors") ? custom_colors() : theme_colors(widget);
}

static RenderOptions current_render_options()
{
    RenderOptions opt;
    opt.mix_to_mono = aud_get_bool("waveform", "mix_to_mono");
    opt.log_scale = aud_get_bool("waveform", "log_enabled");
    opt.display_rms = aud_get_bool("waveform", "display_rms");
    opt.fill_waveform = aud_get_bool("waveform", "fill_waveform");
    opt.soundcloud_style = aud_get_bool("waveform", "soundcloud_style");
    return opt;
}

// Nominal marker thickness, in logical (pre-scale-factor) pixels.
static constexpr double VLINE_WIDTH = 1.5;

// Rounds a user-space x to the device pixel grid, returning the device-space
// left edge of the snapped marker. See draw_vline() for why.
static double snap_vline_x(cairo_t * cr, double x, double * width_out)
{
    double dx = x, dy = 0;
    cairo_user_to_device(cr, &dx, &dy);

    double w = VLINE_WIDTH, wy = 0;
    cairo_user_to_device_distance(cr, &w, &wy);
    w = floor(w + 0.5);
    if (w < 1)
        w = 1;

    *width_out = w;
    return floor(dx - w / 2 + 0.5);
}

// Cairo strokes a line centred on x with antialiasing, so a 1.5px marker at a
// fractional position lands on two or three pixel columns with whatever
// coverage the position happens to give. That is invisible for a static
// marker, but the playback cursor moves continuously across the widget and is
// redrawn ten times a second, so the coverage pattern churns and the line
// reads as flickering and slightly changing width as it travels.
//
// Snap to whole device pixels and fill a rectangle instead: the marker is then
// byte-for-byte the same shape wherever it is, and only its position changes.
// Device space rather than user space so this still holds on a HiDPI
// (scale-factor 2) surface, where a whole user-space pixel is half a device
// one.
static void draw_vline(cairo_t * cr, int height, const Color & c, double x)
{
    double w;
    double dx = snap_vline_x(cr, x, &w);

    double top = 0, bottom = height, ignored = 0;
    cairo_user_to_device(cr, &ignored, &top);
    ignored = 0;
    cairo_user_to_device(cr, &ignored, &bottom);

    cairo_save(cr);
    cairo_identity_matrix(cr);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
    cairo_rectangle(cr, dx, floor(top + 0.5), w, floor(bottom + 0.5) - floor(top + 0.5));
    cairo_fill(cr);
    cairo_restore(cr);
}

static void draw_skip_markers(cairo_t * cr, int width, int height, const WaveColors & colors)
{
    if (!s_cur_filename || s_cur_duration <= 0)
        return;

    String skip = JumpinTagStore::get_skip(s_cur_filename);
    if (!skip || !skip[0])
        return;

    SkipSpec spec = parse_skip_tag(skip);
    // The marker being dragged is drawn from s_popup_click_x instead, so it
    // tracks the pointer rather than sitting at its last committed position.
    if (spec.has_start && s_skip_drag != SkipDragTarget::Start)
        draw_vline(cr, height, colors.skip_start, (spec.start / s_cur_duration) * width);
    if (spec.has_end && s_skip_drag != SkipDragTarget::End)
        draw_vline(cr, height, colors.skip_end, (spec.end / s_cur_duration) * width);
}

// --- drawing ----------------------------------------------------------

static gboolean draw_waveform(GtkWidget * widget, cairo_t * cr)
{
    GtkAllocation a;
    gtk_widget_get_allocation(widget, &a);
    int width = a.width, height = a.height;

    WaveColors colors = resolve_colors(widget);

    cairo_set_source_rgba(cr, colors.bg.r, colors.bg.g, colors.bg.b, colors.bg.a);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);

    if (s_wave.channels > 0 && width > 0 && height > 0)
    {
        RenderOptions opt = current_render_options();
        WaveRenderData * rd = waveform_render_data_build(s_wave, width, opt);
        if (rd)
        {
            double channel_height = height / (double)rd->num_channels;
            double wave_height = 0.9 * channel_height;
            double y = (channel_height - wave_height) / 2;

            bool bars = aud_get_bool("waveform", "bars_style");
            for (int ch = 0; ch < rd->num_channels; ch++, y += channel_height)
            {
                WaveRect rect{0.0, y, (double)width, wave_height};
                if (bars)
                    waveform_draw_wave_bars(rd->samples[ch], colors, cr, rect, opt);
                else
                    waveform_draw_wave_default(rd->samples[ch], colors, cr, rect, opt);
            }
            waveform_render_data_free(rd);
        }
    }

    double play_frac = -1;
    if (aud_drct_get_playing() && s_cur_duration > 0)
    {
        play_frac = aud_drct_get_time() / 1000.0 / s_cur_duration;
        if (play_frac < 0)
            play_frac = 0;
        if (play_frac > 1)
            play_frac = 1;
    }

    // Dim the already-played portion of the waveform -- ported from the
    // DeaDBeeF original's "Shade waveform" option (waveform.c's surf_shaded
    // pass). The original rendered a whole second offscreen copy of the
    // waveform and alpha-blended it in; since this port already rebuilds
    // render data fresh every draw call (see the comment above the channel
    // loop), it's simpler to just clip to the played region and paint a
    // translucent tint over what's already drawn. The tint is the background
    // color, not the (bright, saturated) playhead color -- blending toward
    // bg is what actually darkens/mutes the waveform under it; blending
    // toward an accent color would instead lighten a dark theme's waveform.
    if (play_frac > 0 && aud_get_bool("waveform", "dim_played"))
    {
        // End the tint exactly where the (pixel-snapped) cursor begins, so its
        // trailing edge doesn't get its own independently-moving antialiased
        // fringe -- which would flicker for the same reason draw_vline() does.
        double w, dx = snap_vline_x(cr, play_frac * width, &w);
        double ux = dx, uy = 0;
        cairo_device_to_user(cr, &ux, &uy);

        cairo_save(cr);
        cairo_rectangle(cr, 0, 0, ux, height);
        cairo_clip(cr);
        cairo_set_source_rgba(cr, colors.bg.r, colors.bg.g, colors.bg.b, 0.45);
        cairo_paint(cr);
        cairo_restore(cr);
    }

    if (play_frac >= 0)
        draw_vline(cr, height, colors.pb, play_frac * width);

    if (aud_get_bool("waveform", "display_skip_markers"))
        draw_skip_markers(cr, width, height, colors);

    if (s_popup_marker_active)
    {
        const Color & marker = (s_skip_drag == SkipDragTarget::End) ? colors.skip_end
                                                                    : colors.skip_start;
        draw_vline(cr, height, marker, s_popup_click_x);
    }

    if (s_seeking)
        draw_vline(cr, height, colors.pb, s_seek_x);

    return TRUE;
}

static gboolean draw_ruler(GtkWidget * widget, cairo_t * cr)
{
    // The ruler's height was fixed at creation time (see get_gtk_widget()),
    // but the font size is a live prefs setting -- re-request a tall enough
    // allocation here so a size bump doesn't get clipped, and so the panel
    // shrinks back down if the user lowers it again.
    double font_size = aud_get_int("waveform", "ruler_font_size");
    int wanted_height = (int)font_size + 8;
    if (wanted_height < 16)
        wanted_height = 16;
    gtk_widget_set_size_request(widget, -1, wanted_height);

    GtkAllocation a;
    gtk_widget_get_allocation(widget, &a);
    WaveColors colors = resolve_colors(widget);
    WaveRect rect{0.0, 0.0, (double)a.width, (double)a.height};
    waveform_render_ruler(cr, colors, (float)s_cur_duration, rect, font_size);
    return TRUE;
}

#ifdef USE_GTK3
static gboolean draw_waveform_cb(GtkWidget * widget, cairo_t * cr) { return draw_waveform(widget, cr); }
static gboolean draw_ruler_cb(GtkWidget * widget, cairo_t * cr) { return draw_ruler(widget, cr); }
#else
static gboolean draw_waveform_cb(GtkWidget * widget)
{
    cairo_t * cr = gdk_cairo_create(gtk_widget_get_window(widget));
    gboolean ret = draw_waveform(widget, cr);
    cairo_destroy(cr);
    return ret;
}
static gboolean draw_ruler_cb(GtkWidget * widget)
{
    cairo_t * cr = gdk_cairo_create(gtk_widget_get_window(widget));
    gboolean ret = draw_ruler(widget, cr);
    cairo_destroy(cr);
    return ret;
}
#endif

// --- SKIP menu ----------------------------------------------------------

static void format_seconds(char * out, size_t outsz, double v)
{
    if (v < 0)
        v = 0;
    snprintf(out, outsz, "%.3f", v);
    char * dot = strchr(out, '.');
    if (dot)
    {
        char * end = out + strlen(out) - 1;
        while (end > dot && *end == '0')
            *end-- = 0;
        if (end == dot)
            *end = 0;
    }
}

static StringBuf format_skip_spec(const SkipSpec & spec)
{
    char start_buf[32] = "", end_buf[32] = "";
    if (spec.has_start)
        format_seconds(start_buf, sizeof start_buf, spec.start);
    if (spec.has_end)
        format_seconds(end_buf, sizeof end_buf, spec.end);

    if (spec.has_start && spec.has_end)
        return str_printf("%s %s-", start_buf, end_buf);
    if (spec.has_end)
        return str_printf("%s-", end_buf);
    if (spec.has_start)
        return str_printf("%s", start_buf);
    return StringBuf(0);
}

static void skip_set_point(bool is_start)
{
    if (!s_cur_filename || s_cur_duration <= 0 || !s_area)
        return;

    GtkAllocation a;
    gtk_widget_get_allocation(s_area, &a);
    if (a.width <= 0)
        return;

    double time = s_popup_click_x / (double)a.width * s_cur_duration;
    if (time < 0)
        time = 0;
    if (time > s_cur_duration)
        time = s_cur_duration;

    String cur = JumpinTagStore::get_skip(s_cur_filename);
    SkipSpec spec = parse_skip_tag(cur);
    if (is_start)
    {
        spec.has_start = true;
        spec.start = time;
    }
    else
    {
        spec.has_end = true;
        spec.end = time;
    }

    JumpinTagStore::set_skip(s_cur_filename, format_skip_spec(spec));
    gtk_widget_queue_draw(s_area);
}

static void on_skip_set_start() { skip_set_point(true); }
static void on_skip_set_end() { skip_set_point(false); }

static void on_popup_selection_done(GtkMenuShell *, gpointer)
{
    s_popup_marker_active = false;
    queue_all_draws();
}

static void build_popup_menu()
{
    s_popup = gtk_menu_new();
    gtk_menu_attach_to_widget((GtkMenu *)s_popup, s_area, nullptr);

    GtkWidget * start_item = gtk_menu_item_new_with_mnemonic(_("Set SKIP Start Here"));
    GtkWidget * end_item = gtk_menu_item_new_with_mnemonic(_("Set SKIP End Here"));
    gtk_container_add((GtkContainer *)s_popup, start_item);
    gtk_container_add((GtkContainer *)s_popup, end_item);

    g_signal_connect(start_item, "activate", (GCallback)on_skip_set_start, nullptr);
    g_signal_connect(end_item, "activate", (GCallback)on_skip_set_end, nullptr);
    g_signal_connect(s_popup, "selection-done", (GCallback)on_popup_selection_done, nullptr);

    gtk_widget_show_all(s_popup);
}

// --- mouse ----------------------------------------------------------------

static double clamp_widget_x(double x, int width)
{
    if (x < 0)
        return 0;
    if (width > 0 && x > width)
        return width;
    return x;
}

// Edge zones and the minimum travel needed to treat a right-drag as a
// track-skip rather than a SKIP-line placement. Scaled with the widget so
// a very narrow panel still has a usable interior.
static double edge_zone_px(int width)
{
    double z = width * 0.04;
    if (z < 8)
        z = 8;
    if (z > 24)
        z = 24;
    return z;
}

static double min_swipe_px(int width)
{
    double s = width * 0.08;
    if (s < 24)
        s = 24;
    return s;
}

static SkipDragTarget hit_test_skip_marker(double x, int width)
{
    if (width <= 0 || s_cur_duration <= 0 || !s_cur_filename)
        return SkipDragTarget::None;

    String skip = JumpinTagStore::get_skip(s_cur_filename);
    if (!skip || !skip[0])
        return SkipDragTarget::None;

    SkipSpec spec = parse_skip_tag(skip);
    double best = SKIP_GRAB_PX + 1;
    SkipDragTarget hit = SkipDragTarget::None;

    if (spec.has_start)
    {
        double mx = (spec.start / s_cur_duration) * width;
        double d = fabs(x - mx);
        if (d <= SKIP_GRAB_PX && d < best)
        {
            best = d;
            hit = SkipDragTarget::Start;
        }
    }
    if (spec.has_end)
    {
        double mx = (spec.end / s_cur_duration) * width;
        double d = fabs(x - mx);
        if (d <= SKIP_GRAB_PX && d < best)
        {
            best = d;
            hit = SkipDragTarget::End;
        }
    }
    return hit;
}

// Once the pointer has moved enough to count as a drag (not a click), pick
// which existing SKIP marker to move: the one under the press if any,
// otherwise the only marker, otherwise the nearer of the two.
static SkipDragTarget pick_skip_drag_target(double x, int width)
{
    SkipDragTarget hit = hit_test_skip_marker(x, width);
    if (hit != SkipDragTarget::None)
        return hit;

    if (width <= 0 || s_cur_duration <= 0 || !s_cur_filename)
        return SkipDragTarget::None;

    String skip = JumpinTagStore::get_skip(s_cur_filename);
    if (!skip || !skip[0])
        return SkipDragTarget::None;

    SkipSpec spec = parse_skip_tag(skip);
    if (spec.has_start && !spec.has_end)
        return SkipDragTarget::Start;
    if (spec.has_end && !spec.has_start)
        return SkipDragTarget::End;
    if (spec.has_start && spec.has_end)
    {
        double sx = (spec.start / s_cur_duration) * width;
        double ex = (spec.end / s_cur_duration) * width;
        return (fabs(x - sx) <= fabs(x - ex)) ? SkipDragTarget::Start : SkipDragTarget::End;
    }
    return SkipDragTarget::None;
}

static void finish_right_gesture()
{
    s_rbtn_down = false;
    s_edge_gesture_done = false;
    s_skip_drag = SkipDragTarget::None;
    s_popup_marker_active = false;
}

static bool jumpin_plugin_enabled()
{
    PluginHandle * plugin = aud_plugin_lookup_basename("jumpin");
    return plugin && aud_plugin_get_enabled(plugin);
}

// Maps the "swipe_playlist_modifier" pref (see behavior_page) to the actual
// GDK modifier bit it stands for. 0 means the feature is off.
static GdkModifierType swipe_playlist_mask()
{
    switch (aud_get_int("waveform", "swipe_playlist_modifier"))
    {
    case 1: return GDK_SHIFT_MASK;
    case 2: return GDK_CONTROL_MASK;
    case 3: return GDK_MOD1_MASK;
    case 4: return GDK_SUPER_MASK;
    default: return (GdkModifierType)0;
    }
}

// Switches to the previous/next playlist (wrapping around) and starts it
// playing, rather than just changing tracks within the current playlist.
static void switch_playlist_and_play(int dir)
{
    int n = Playlist::n_playlists();
    if (n <= 0)
        return;

    int idx = Playlist::active_playlist().index();
    if (idx < 0)
        idx = 0;

    Playlist next = Playlist::by_index(((idx + dir) % n + n) % n);
    next.activate();
    next.start_playback();
}

static void apply_edge_swipe(int dir, guint event_state)
{
    s_edge_gesture_done = true;
    s_popup_marker_active = false;
    s_skip_drag = SkipDragTarget::None;
    queue_all_draws();

    GdkModifierType mod = swipe_playlist_mask();
    if (mod && (event_state & mod) == (guint)mod)
    {
        switch_playlist_and_play(dir);
        return;
    }

    // Jump In owns the randomized-offset seek; the waveform plugin just
    // asks it to fire (next or previous) when the pref is on and the
    // plugin is actually loaded. Otherwise this is a plain track skip.
    if (aud_get_bool("waveform", "swipe_jump_in") && jumpin_plugin_enabled())
    {
        hook_call(dir < 0 ? "jumpin activate prev" : "jumpin activate", nullptr);
        return;
    }

    if (dir < 0)
        aud_drct_pl_prev();
    else
        aud_drct_pl_next();
}

static gboolean on_button_press(GtkWidget * widget, GdkEventButton * event, gpointer)
{
    if (event->button == 3)
    {
        GtkAllocation a;
        gtk_widget_get_allocation(widget, &a);

        // Right-click takes over from an in-progress left-drag seek so we
        // don't also seek when the left button is later released.
        s_seeking = false;
        s_rbtn_down = true;
        s_rbtn_start_x = event->x;
        s_edge_gesture_done = false;
        s_popup_click_x = clamp_widget_x(event->x, a.width);
        s_popup_marker_active = true;
        s_skip_drag = hit_test_skip_marker(event->x, a.width);
        queue_all_draws();
        return TRUE;
    }
    if (event->button == 2)
        return TRUE;

    s_seeking = true;
    s_seek_x = event->x;
    queue_all_draws();
    return TRUE;
}

static gboolean on_motion(GtkWidget * widget, GdkEventMotion * event, gpointer)
{
    GtkAllocation a;
    gtk_widget_get_allocation(widget, &a);

    if (s_rbtn_down)
    {
        if (!s_edge_gesture_done)
        {
            s_popup_click_x = clamp_widget_x(event->x, a.width);

            // Sliding (not a click) attaches the nearest existing SKIP
            // marker to the pointer so the set line is movable.
            if (s_skip_drag == SkipDragTarget::None &&
                fabs(event->x - s_rbtn_start_x) >= SKIP_CLICK_PX)
            {
                s_skip_drag = pick_skip_drag_target(s_rbtn_start_x, a.width);
            }

            // Swipe to either edge (from far enough away that a click near
            // the edge is still a click) changes track. Fires once per
            // press so holding the button at the edge after a skip does
            // not chain through the playlist.
            double edge = edge_zone_px(a.width);
            double min_swipe = min_swipe_px(a.width);
            if (event->x <= edge && (s_rbtn_start_x - event->x) >= min_swipe)
                apply_edge_swipe(-1, event->state);
            else if (event->x >= a.width - edge && (event->x - s_rbtn_start_x) >= min_swipe)
                apply_edge_swipe(1, event->state);
            else
                queue_all_draws();
        }
        return TRUE;
    }

    if (!s_seeking)
        return TRUE;

    if (event->x < -100 || event->x > a.width + 100 || event->y < -100 || event->y > a.height + 100)
        s_seeking = false;
    else
        s_seek_x = event->x;

    queue_all_draws();
    return TRUE;
}

static gboolean on_button_release(GtkWidget * widget, GdkEventButton * event, gpointer)
{
    if (event->button == 3)
    {
        bool fired_edge = s_edge_gesture_done;
        SkipDragTarget dragged = s_skip_drag;
        double start_x = s_rbtn_start_x;
        finish_right_gesture();

        if (fired_edge)
        {
            queue_all_draws();
            return TRUE;
        }

        GtkAllocation a;
        gtk_widget_get_allocation(widget, &a);
        s_popup_click_x = clamp_widget_x(event->x, a.width);

        // Dragging far enough with an existing SKIP line commits the new
        // position. A click (or tiny movement) still opens the menu, so a
        // second point can be set from the same spot.
        double moved = fabs(event->x - start_x);
        if (moved >= SKIP_CLICK_PX && dragged == SkipDragTarget::None)
            dragged = pick_skip_drag_target(start_x, a.width);
        if (moved >= SKIP_CLICK_PX &&
            (dragged == SkipDragTarget::Start || dragged == SkipDragTarget::End))
        {
            s_popup_marker_active = false;
            skip_set_point(dragged == SkipDragTarget::Start);
            queue_all_draws();
            return TRUE;
        }

        s_popup_marker_active = true;
        queue_all_draws();
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        gtk_menu_popup((GtkMenu *)s_popup, nullptr, nullptr, nullptr, nullptr, 0,
                       gtk_get_current_event_time());
G_GNUC_END_IGNORE_DEPRECATIONS
        return TRUE;
    }
    if (event->button == 2)
    {
        if (aud_drct_get_playing())
            aud_drct_pause();
        return TRUE;
    }

    if (s_seeking)
    {
        GtkAllocation a;
        gtk_widget_get_allocation(widget, &a);
        if (s_cur_duration > 0 && a.width > 0)
        {
            double frac = event->x / (double)a.width;
            if (frac < 0)
                frac = 0;
            if (frac > 1)
                frac = 1;
            aud_drct_seek((int)(frac * s_cur_duration * 1000.0));
        }
        s_seeking = false;
        queue_all_draws();
    }
    return TRUE;
}

static gboolean on_scroll(GtkWidget *, GdkEventScroll * event, gpointer)
{
    if (!aud_get_bool("waveform", "scroll_enabled") || !aud_drct_get_playing())
        return TRUE;

    int duration_ms = (int)(s_cur_duration * 1000.0);
    int time_ms = aud_drct_get_time();
    int step = duration_ms / 30;
    if (step < 1000)
        step = 1000;
    if (step > 3600000)
        step = 3600000;

    if (event->direction == GDK_SCROLL_UP)
    {
        int t = time_ms + step;
        aud_drct_seek(t < duration_ms ? t : duration_ms);
    }
    else if (event->direction == GDK_SCROLL_DOWN)
    {
        int t = time_ms - step;
        aud_drct_seek(t > 0 ? t : 0);
    }
    return TRUE;
}

// --- widget lifecycle -------------------------------------------------

static void widget_destroy_cb(GtkWidget *)
{
    timer_remove(TimerRate::Hz10, on_timer_tick);
    hook_dissociate("playback ready", on_playback_ready);
    hook_dissociate("playback stop", on_playback_stop);

    if (s_popup)
    {
        gtk_widget_destroy(s_popup);
        s_popup = nullptr;
    }

    s_area = nullptr;
    s_ruler = nullptr;

    s_rbtn_down = false;
    s_edge_gesture_done = false;
    s_skip_drag = SkipDragTarget::None;
    s_popup_marker_active = false;
    s_seeking = false;

    reset_wave();
    s_cur_filename = String();
    s_cur_key.clear();
}

void * Waveform::get_gtk_widget()
{
    GtkWidget * vbox = audgui_vbox_new(0);

    GtkWidget * area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, -1, 64);
    gtk_widget_add_events(area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                     GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK);
    gtk_box_pack_start((GtkBox *)vbox, area, true, true, 0);

    GtkWidget * ruler = gtk_drawing_area_new();
    gtk_widget_set_size_request(ruler, -1, 16);
    gtk_box_pack_start((GtkBox *)vbox, ruler, false, false, 0);

    s_area = area;
    s_ruler = ruler;

    build_popup_menu();

    g_signal_connect(area, AUDGUI_DRAW_SIGNAL, (GCallback)draw_waveform_cb, nullptr);
    g_signal_connect(ruler, AUDGUI_DRAW_SIGNAL, (GCallback)draw_ruler_cb, nullptr);
    g_signal_connect(area, "button-press-event", (GCallback)on_button_press, nullptr);
    g_signal_connect(area, "button-release-event", (GCallback)on_button_release, nullptr);
    g_signal_connect(area, "motion-notify-event", (GCallback)on_motion, nullptr);
    g_signal_connect(area, "scroll-event", (GCallback)on_scroll, nullptr);
    g_signal_connect(vbox, "destroy", (GCallback)widget_destroy_cb, nullptr);

    hook_associate("playback ready", on_playback_ready, nullptr);
    hook_associate("playback stop", on_playback_stop, nullptr);
    timer_add(TimerRate::Hz10, on_timer_tick);

    if (aud_drct_get_ready())
        load_track(aud_drct_get_filename());

    gtk_widget_show_all(vbox);
    if (!aud_get_bool("waveform", "display_ruler"))
        gtk_widget_hide(ruler);

    GtkWidget * frame = gtk_frame_new(nullptr);
    gtk_frame_set_shadow_type((GtkFrame *)frame, GTK_SHADOW_IN);
    gtk_container_add((GtkContainer *)frame, vbox);
    return frame;
}

#endif // USE_GTK
