/*
 * jumpin.cc
 * Jump In plugin for Audacious
 *
 * Port of the DeaDBeeF plugin ddb_jumpin, itself a port of the foobar2000
 * component foo_jumpin: advances to the next track and seeks to a
 * user-configured, optionally randomized time offset (handy so a
 * crossfader doesn't fade in from silence). A track tagged JUMPIN=0 opts
 * out of that randomized seek.
 *
 * Independently of the above, a track can carry a SKIP tag to trim its
 * start and/or end on every playback, not just via the Jump In action. See
 * skiptag.h for the accepted formats.
 *
 * Differences from the DeaDBeeF original, forced by API differences:
 *
 *  - DeaDBeeF's DB_EV_SONGSTARTED/DB_EV_STOP map to the "playback ready" /
 *    "playback stop" hooks.
 *  - DeaDBeeF polled for the SKIP end point from a dedicated background
 *    thread (deadbeef->thread_start()). Audacious's timer_add() calls back
 *    on the main loop at a fixed rate, so no thread/mutex is needed here.
 *  - DeaDBeeF let external processes trigger the action via
 *    deadbeef->plugin.exec_cmdline (`deadbeef --plugin=jumpin`). Audacious
 *    has no equivalent generic plugin-action IPC. Two routes are provided
 *    instead: (1) the "jumpin activate" hook, which the Global Hotkeys /
 *    Qt Global Hotkeys plugins call for their own "Jump In (Next Track)"
 *    binding -- the normal way to assign a system-wide hotkey; and (2) for
 *    triggering from outside Audacious entirely (a desktop-environment
 *    hotkey, a script), the same timer watches the mtime of a small trigger
 *    file ($XDG_DATA_HOME/audacious/jumpin-trigger) -- bind a hotkey to
 *    `touch` it.
 *  - DeaDBeeF read/wrote SKIP and JUMPIN as free-form custom tags via
 *    pl_find_meta(). Audacious's Tuple has no such open key/value store, so
 *    values are kept in a small sidecar file instead -- see tagstore.h.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <glib/gstdio.h>

#include <libaudcore/drct.h>
#include <libaudcore/hook.h>
#include <libaudcore/i18n.h>
#include <libaudcore/interface.h>
#include <libaudcore/audstrings.h>
#include <libaudcore/plugin.h>
#include <libaudcore/preferences.h>
#include <libaudcore/runtime.h>
#include <libaudcore/tuple.h>

#include "skiptag.h"
#include "tagstore.h"

#ifdef USE_GTK
#include <libaudgui/gtk-compat.h>
#include <libaudgui/libaudgui-gtk.h>
#endif

class JumpIn : public GeneralPlugin
{
public:
    static const PreferencesWidget widgets[];
    static const PluginPreferences prefs;

    static constexpr PluginInfo info = {
        N_("Jump In"),
        PACKAGE,
        nullptr,
        &prefs
    };

    constexpr JumpIn() : GeneralPlugin(info, false) {}

    bool init() override;
    void cleanup() override;
};

EXPORT JumpIn aud_plugin_instance;

static const char * const jumpin_defaults[] = {
    "randomize", "TRUE",
    "min_seconds", "8",
    "max_seconds", "20",
    "clamp_to_pct", "TRUE",
    nullptr
};

const PreferencesWidget JumpIn::widgets[] = {
    WidgetLabel(N_("<b>Jump In</b>")),
    WidgetCheck(N_("Randomize offset"), WidgetBool("jumpin", "randomize")),
    WidgetSpin(N_("Minimum offset:"), WidgetFloat("jumpin", "min_seconds"),
               {0, 600, 1, N_("seconds")}, WIDGET_CHILD),
    WidgetSpin(N_("Maximum offset:"), WidgetFloat("jumpin", "max_seconds"),
               {0, 600, 1, N_("seconds")}, WIDGET_CHILD),
    WidgetCheck(N_("Clamp offset to 80% of track length"),
                WidgetBool("jumpin", "clamp_to_pct")),
    WidgetLabel(N_("Tag a track with a JUMPIN=0 comment to opt it out of the "
                   "seek, or SKIP to trim it (e.g. \"1 5:29-\" skips the first "
                   "second and ends the track at 5:29) -- use \"Edit Jump In / "
                   "Skip Tags\" on the Services menu.")),
    WidgetLabel(N_("For best crossfader results, set the minimum offset to at "
                   "least your crossfade duration.")),
};

const PluginPreferences JumpIn::prefs = {{widgets}};

// --- state ---

static bool s_seek_pending = false;
static double s_seek_offset = 0.0;

static bool s_skip_end_active = false;
static double s_skip_end_time = 0.0;

// The SKIP value last acted on for the current track. The tag can change
// while the track is playing -- the waveform seekbar's "Set SKIP End Here"
// writes the shared store (see tagstore.h) mid-playback -- so the timer
// below re-reads it and compares against this instead of latching the value
// once at "playback ready".
static String s_skip_seen;

// (time_t) -1 means "not yet observed"; the trigger file's mtime as of the
// last check otherwise. Never fires on the mtime it has at plugin startup,
// only on a later change, so a stale trigger file left over from a previous
// session doesn't fire immediately.
static time_t s_trigger_seen_mtime = (time_t)-1;

static double compute_jump_offset()
{
    double lo = aud_get_double("jumpin", "min_seconds");
    double hi = aud_get_double("jumpin", "max_seconds");
    if (lo > hi)
    {
        double t = lo;
        lo = hi;
        hi = t;
    }

    double offset = lo;
    if (aud_get_bool("jumpin", "randomize") && hi > lo)
        offset = lo + ((double)rand() / (double)RAND_MAX) * (hi - lo);

    return offset;
}

static void trigger_jump_in()
{
    if (!aud_drct_get_playing())
        return;

    // Arm the deferred seek BEFORE requesting the next track, so the
    // "playback ready" handler catches the very first new-track event.
    s_seek_offset = compute_jump_offset();
    s_seek_pending = true;

    aud_drct_pl_next();
}

static void menu_jump_in() { trigger_jump_in(); }

// Lets the Global Hotkeys plugin (GTK or Qt) invoke Jump In without a direct
// dependency between the two plugins -- same pattern used for AOSD's
// "aosd toggle" hook. Also usable by any other plugin/script via
// hook_call("jumpin activate", nullptr).
static void hook_jump_in(void *, void *) { trigger_jump_in(); }

static void jumpin_playback_ready(void *, void *)
{
    // Every new track re-evaluates both features from scratch: the
    // randomized Jump In seek (only if armed by the action) and the SKIP
    // tag trim (unconditional, applies to every playback of the track).
    bool jumpin_pending = s_seek_pending;
    s_seek_pending = false;
    s_skip_end_active = false;
    s_skip_seen = String();

    String filename = aud_drct_get_filename();
    if (!filename)
        return;

    String jumpin_val = JumpinTagStore::get_jumpin(filename);
    bool jumpin_opt_out = jumpin_val && !strcmp(jumpin_val, "0");

    String skip_val = JumpinTagStore::get_skip(filename);
    s_skip_seen = skip_val;
    SkipSpec spec = parse_skip_tag(skip_val);

    Tuple tuple = aud_drct_get_tuple();
    int length_ms = tuple.get_int(Tuple::Length);
    double length = (length_ms > 0) ? length_ms / 1000.0 : 0.0;

    // Combine the randomized Jump In offset (if armed, and not opted out
    // via JUMPIN=0) with the SKIP tag's start point: whichever asks to
    // start later wins.
    double start_pos = 0.0;
    if (jumpin_pending && !jumpin_opt_out)
        start_pos = s_seek_offset;
    if (spec.has_start && spec.start > start_pos)
        start_pos = spec.start;

    if (start_pos > 0.05)
    {
        if (aud_get_bool("jumpin", "clamp_to_pct") && length > 0)
        {
            double max_pos = length * 0.80;
            if (start_pos > max_pos)
                start_pos = max_pos;
        }
        if (start_pos > 0.05)
            aud_drct_seek((int)(start_pos * 1000.0));
    }

    // Arm the end-of-track monitor for a SKIP end point, which truncates
    // playback before the real end of the file.
    if (spec.has_end && spec.end > 0 && (length <= 0 || spec.end < length))
    {
        s_skip_end_active = true;
        s_skip_end_time = spec.end;
    }
}

static void jumpin_playback_stop(void *, void *)
{
    s_seek_pending = false;
    s_skip_end_active = false;
    s_skip_seen = String();
}

// Picks up a SKIP tag edited during playback (from the waveform seekbar's
// right-click menu, or this plugin's own edit dialog) and re-arms the end
// point accordingly, so the user doesn't have to replay the track to see the
// trim take effect.
//
// A start point can't be applied retroactively, and an end point already
// behind the playhead is deliberately left for the next playback rather than
// yanking the user out of the track they are in the middle of auditioning.
static void refresh_skip_end()
{
    String filename = aud_drct_get_filename();
    if (!filename)
        return;

    String skip_val = JumpinTagStore::get_skip(filename);
    if (skip_val == s_skip_seen)
        return;

    s_skip_seen = skip_val;
    SkipSpec spec = parse_skip_tag(skip_val);

    int length_ms = aud_drct_get_length();
    double length = (length_ms > 0) ? length_ms / 1000.0 : 0.0;
    double now = aud_drct_get_time() / 1000.0;

    s_skip_end_active = spec.has_end && spec.end > now + 0.5 &&
                        (length <= 0 || spec.end < length);
    s_skip_end_time = spec.end;
}

// Polls the mtime of a small trigger file so an external process (e.g. a
// desktop-environment global hotkey bound to `touch` on it) can invoke the
// Jump In action, the way DeaDBeeF's exec_cmdline let `deadbeef
// --plugin=jumpin` do. There's no equivalent generic plugin-action IPC in
// Audacious to hook into instead.
static void check_trigger_file()
{
    StringBuf path = filename_build({aud_get_path(AudPath::UserDir), "jumpin-trigger"});

    GStatBuf st;
    if (g_stat(path, &st) < 0)
        return;

    if (s_trigger_seen_mtime == (time_t)-1)
    {
        s_trigger_seen_mtime = st.st_mtime;
        return;
    }

    if (st.st_mtime != s_trigger_seen_mtime)
    {
        s_trigger_seen_mtime = st.st_mtime;
        trigger_jump_in();
    }
}

// There's no "position changed" hook granular enough to catch a SKIP end
// point, so a lightweight timed poll is the simplest reliable option (same
// approach the DeaDBeeF original used, just on the main loop instead of a
// dedicated thread).
static void jumpin_timer_tick(void *)
{
    check_trigger_file();

    if (!aud_drct_get_playing())
        return;

    refresh_skip_end();

    if (!s_skip_end_active || aud_drct_get_paused())
        return;

    if (aud_drct_get_time() >= (int)(s_skip_end_time * 1000.0))
    {
        s_skip_end_active = false;
        aud_drct_pl_next();
    }
}

#ifdef USE_GTK
struct EditDialogEntries
{
    GtkWidget * skip;
    GtkWidget * jumpin;
};

static void edit_save_cb(void * data)
{
    auto entries = (EditDialogEntries *)data;

    String filename = aud_drct_get_filename();
    if (!filename)
        return;

    JumpinTagStore::set_skip(filename, gtk_entry_get_text((GtkEntry *)entries->skip));
    JumpinTagStore::set_jumpin(filename, gtk_entry_get_text((GtkEntry *)entries->jumpin));
}

static void edit_destroy_cb(GtkWidget *, void * data) { delete (EditDialogEntries *)data; }

static GtkWidget * edit_row_new(const char * label_text, GtkWidget * entry)
{
    GtkWidget * hbox = audgui_hbox_new(6);
    GtkWidget * label = gtk_label_new(label_text);
    gtk_box_pack_start((GtkBox *)hbox, label, false, false, 0);
    gtk_box_pack_start((GtkBox *)hbox, entry, true, true, 0);
    return hbox;
}

static void menu_edit_tags()
{
    String filename = aud_drct_get_filename();
    if (!filename)
    {
        aud_ui_show_error(_("No song is currently playing."));
        return;
    }

    auto entries = new EditDialogEntries;
    entries->skip = gtk_entry_new();
    entries->jumpin = gtk_entry_new();

    gtk_entry_set_text((GtkEntry *)entries->skip, JumpinTagStore::get_skip(filename));
    gtk_entry_set_text((GtkEntry *)entries->jumpin, JumpinTagStore::get_jumpin(filename));
    gtk_entry_set_activates_default((GtkEntry *)entries->skip, true);
    gtk_entry_set_activates_default((GtkEntry *)entries->jumpin, true);

    GtkWidget * box = audgui_vbox_new(6);
    gtk_box_pack_start((GtkBox *)box, edit_row_new(_("SKIP (e.g. \"1 5:29-\"):"), entries->skip),
                       false, false, 0);
    gtk_box_pack_start((GtkBox *)box,
                        edit_row_new(_("JUMPIN (\"0\" to opt out):"), entries->jumpin), false,
                        false, 0);

    GtkWidget * button1 = audgui_button_new(_("_Save"), "document-save", edit_save_cb, entries);
    GtkWidget * button2 = audgui_button_new(_("_Cancel"), "process-stop", nullptr, nullptr);

    GtkWidget * dialog =
        audgui_dialog_new(GTK_MESSAGE_OTHER, _("Edit Jump In / Skip Tags"),
                           _("Tags apply to the currently playing song:"), button1, button2);

    audgui_dialog_add_widget(dialog, box);

    g_signal_connect(dialog, "destroy", (GCallback)edit_destroy_cb, entries);

    gtk_widget_show_all(dialog);
}
#endif // USE_GTK

bool JumpIn::init()
{
    aud_config_set_defaults("jumpin", jumpin_defaults);

    srand((unsigned)time(nullptr));
    s_trigger_seen_mtime = (time_t)-1;

    hook_associate("playback ready", jumpin_playback_ready, nullptr);
    hook_associate("playback stop", jumpin_playback_stop, nullptr);
    hook_associate("jumpin activate", hook_jump_in, nullptr);

    timer_add(TimerRate::Hz10, jumpin_timer_tick);

    aud_plugin_menu_add(AudMenuID::Main, menu_jump_in, N_("Jump In (Next Track)"), "go-jump");
#ifdef USE_GTK
    aud_plugin_menu_add(AudMenuID::Main, menu_edit_tags, N_("Edit Jump In / Skip Tags..."),
                        "document-edit");
#endif

    return true;
}

void JumpIn::cleanup()
{
    aud_plugin_menu_remove(AudMenuID::Main, menu_jump_in);
#ifdef USE_GTK
    aud_plugin_menu_remove(AudMenuID::Main, menu_edit_tags);
#endif

    timer_remove(TimerRate::Hz10, jumpin_timer_tick);

    hook_dissociate("playback ready", jumpin_playback_ready);
    hook_dissociate("playback stop", jumpin_playback_stop);
    hook_dissociate("jumpin activate", hook_jump_in);

    s_seek_pending = false;
    s_skip_end_active = false;
}
