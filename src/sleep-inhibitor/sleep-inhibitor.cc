/*
 * sleep-inhibitor.cc
 * Sleep Inhibitor plugin for Audacious
 *
 * Port of the DeaDBeeF plugin sleepinhibitor: stops the system from
 * auto-suspending while music is actually playing, by holding a
 * systemd-logind inhibitor lock (org.freedesktop.login1.Manager.Inhibit)
 * for the duration of playback and releasing it on pause/stop. That way
 * the machine can still sleep normally when nothing is being listened to.
 *
 * Differences from the DeaDBeeF original, forced by API differences:
 *
 *  - DeaDBeeF's DB_EV_SONGSTARTED / DB_EV_PAUSED / DB_EV_STOP map to the
 *    "playback begin" / "playback pause" / "playback unpause" /
 *    "playback stop" hooks. Rather than acquire/release per event, every
 *    hook runs the same update_inhibitor(), which asks libaudcore what the
 *    playback state actually is -- that also covers the plugin being
 *    enabled mid-track, with no separate connect() path.
 *  - The original talked to the bus with sd-bus, adding a libsystemd
 *    dependency. Audacious plugins already link GIO (see mpris2), so GDBus
 *    is used instead; the inhibitor file descriptor comes back over the
 *    wire as a unix fd, hence the GUnixFDList dance in acquire_inhibitor().
 *  - The original took a mutex around the fd, because DeaDBeeF may deliver
 *    plugin messages from a decoder thread. Audacious hooks and preference
 *    callbacks both run on the main loop, so no locking is needed here.
 *  - New: an "Inhibit system sleep while playing" preference. Unticking it
 *    drops any lock currently held without having to disable the plugin.
 */

#include <unistd.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include <libaudcore/drct.h>
#include <libaudcore/hook.h>
#include <libaudcore/i18n.h>
#include <libaudcore/plugin.h>
#include <libaudcore/preferences.h>
#include <libaudcore/runtime.h>

static void update_inhibitor();
static void reacquire_inhibitor();

class SleepInhibitor : public GeneralPlugin
{
public:
    static const char about[];
    static const PreferencesWidget widgets[];
    static const PluginPreferences prefs;

    static constexpr PluginInfo info = {
        N_("Sleep Inhibitor"),
        PACKAGE,
        about,
        &prefs
    };

    constexpr SleepInhibitor() : GeneralPlugin(info, false) {}

    bool init() override;
    void cleanup() override;
};

EXPORT SleepInhibitor aud_plugin_instance;

const char SleepInhibitor::about[] =
 N_("Sleep Inhibitor Plugin for Audacious\n\n"
    "Prevents the system from suspending while music is playing, using a "
    "systemd-logind (org.freedesktop.login1) inhibitor lock. The lock is "
    "held only during playback and is released on pause, stop and quit, so "
    "the machine still sleeps normally when you are not listening.\n\n"
    "To check it is working, run \"systemd-inhibit --list\" while a track "
    "is playing.");

static const char * const sleep_inhibitor_defaults[] = {
    "enabled", "TRUE",
    "what", "sleep:idle",
    nullptr
};

// The logind "what" field. DeaDBeeF exposed this as free text, but the
// only combinations that make sense here are these three, and a text entry
// in Audacious commits on every keystroke -- which would mean re-taking the
// lock against each half-typed target name.
static const ComboItem what_values[] = {
    ComboItem(N_("Idle timeout and suspend requests"), "sleep:idle"),
    ComboItem(N_("Idle timeout only"), "idle"),
    ComboItem(N_("Suspend requests only"), "sleep"),
};

const PreferencesWidget SleepInhibitor::widgets[] = {
    WidgetCheck(N_("Inhibit system sleep while playing"),
                WidgetBool("sleep_inhibitor", "enabled", update_inhibitor)),
    WidgetCombo(N_("Block:"),
                WidgetString("sleep_inhibitor", "what", reacquire_inhibitor),
                {what_values}, WIDGET_CHILD),
    WidgetLabel(N_("\"Idle timeout\" is the one that matters for uninterrupted "
                   "listening: it tells logind the session is not idle, so the "
                   "desktop does not suspend mid-track. \"Suspend requests\" "
                   "additionally delays explicit suspend and hibernate "
                   "requests.")),
};

const PluginPreferences SleepInhibitor::prefs = {{widgets}};

// --- state ---

// Only ever touched from the main loop: playback hooks and preferences
// callbacks both run there, so unlike the DeaDBeeF original this needs no
// lock.
static GDBusConnection * s_bus = nullptr;
static int s_fd = -1;

// --- inhibitor lock ---

static bool ensure_bus()
{
    if (s_bus)
        return true;

    GError * error = nullptr;
    s_bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);

    if (! s_bus)
    {
        AUDERR("Failed to connect to the system bus: %s\n", error->message);
        g_error_free(error);
        return false;
    }

    return true;
}

static void acquire_inhibitor()
{
    if (s_fd >= 0) // already holding a lock
        return;
    if (! ensure_bus())
        return;

    String what = aud_get_str("sleep_inhibitor", "what");

    GError * error = nullptr;
    GUnixFDList * fd_list = nullptr;
    GVariant * reply = g_dbus_connection_call_with_unix_fd_list_sync(s_bus,
     "org.freedesktop.login1", "/org/freedesktop/login1",
     "org.freedesktop.login1.Manager", "Inhibit",
     g_variant_new("(ssss)", (const char *) what, "Audacious",
      _("Music playback"), "block"),
     G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, & fd_list,
     nullptr, & error);

    if (! reply)
    {
        AUDERR("Inhibit call failed: %s\n", error->message);
        g_error_free(error);
        return;
    }

    gint32 index = -1;
    g_variant_get(reply, "(h)", & index);

    // The reply carries an index into the fd list, not the descriptor
    // itself. g_unix_fd_list_get() hands back a dup, so the lock outlives
    // the list; it is held until that descriptor is closed.
    if (fd_list && index >= 0)
    {
        s_fd = g_unix_fd_list_get(fd_list, index, & error);

        if (s_fd < 0)
        {
            AUDERR("Failed to claim the inhibitor descriptor: %s\n",
             error->message);
            g_error_free(error);
        }
        else
            AUDINFO("Acquired inhibitor (%s).\n", (const char *) what);
    }
    else
        AUDERR("Inhibit call returned no file descriptor.\n");

    g_variant_unref(reply);

    if (fd_list)
        g_object_unref(fd_list);
}

static void release_inhibitor()
{
    if (s_fd < 0)
        return;

    close(s_fd);
    s_fd = -1;
    AUDINFO("Released inhibitor.\n");
}

// Single point of truth: rather than pair each playback event with an
// acquire or a release, ask what the state is now. Every hook, the
// preference toggle and init() all route through here.
static void update_inhibitor()
{
    bool want = aud_get_bool("sleep_inhibitor", "enabled") &&
                aud_drct_get_playing() && ! aud_drct_get_paused();

    if (want)
        acquire_inhibitor();
    else
        release_inhibitor();
}

static void playback_state_changed(void *, void *) { update_inhibitor(); }

// The target list is baked into the lock when it is taken, so a change to
// it only takes effect on a fresh one.
static void reacquire_inhibitor()
{
    release_inhibitor();
    update_inhibitor();
}

// --- plugin ---

bool SleepInhibitor::init()
{
    aud_config_set_defaults("sleep_inhibitor", sleep_inhibitor_defaults);

    hook_associate("playback begin", playback_state_changed, nullptr);
    hook_associate("playback pause", playback_state_changed, nullptr);
    hook_associate("playback unpause", playback_state_changed, nullptr);
    hook_associate("playback stop", playback_state_changed, nullptr);

    // Catch the case of being enabled part-way through a track.
    update_inhibitor();

    return true;
}

void SleepInhibitor::cleanup()
{
    hook_dissociate("playback begin", playback_state_changed);
    hook_dissociate("playback pause", playback_state_changed);
    hook_dissociate("playback unpause", playback_state_changed);
    hook_dissociate("playback stop", playback_state_changed);

    release_inhibitor();

    if (s_bus)
    {
        g_object_unref(s_bus);
        s_bus = nullptr;
    }
}
