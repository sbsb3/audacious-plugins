/*
 * tagstore.cc
 * Jump In plugin for Audacious
 */

#include "tagstore.h"

#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>

#include <libaudcore/audstrings.h>
#include <libaudcore/runtime.h>

namespace JumpinTagStore
{
static GKeyFile * s_keyfile = nullptr;
static String s_path;

// Identity of the file as of the last load or save. The "jumpin" and
// "waveform" plugins each link their own copy of this store, with their own
// statics, so neither would ever observe the other's writes if the file were
// read only once per process -- and the whole point of sharing one file is
// that a SKIP tag set from the waveform seekbar takes effect in Jump In
// (and vice versa) without restarting Audacious. Every access therefore
// re-stats the file and reloads it if it changed underneath us.
static time_t s_seen_mtime = 0;
static gint64 s_seen_size = -1;

// SHA-256 of the filename URI, used as the group name. This keeps every
// possible filename valid as a GKeyFile group (which disallows '[', ']' and
// control characters), without needing to escape anything ourselves.
static String group_for(const char * filename)
{
    char * hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, filename, -1);
    String group(hash);
    g_free(hash);
    return group;
}

// Records the file's identity so a later stat can tell whether the other
// plugin has rewritten it. A missing file is recorded as size -1, which no
// existing file can compare equal to.
static void stamp_file()
{
    GStatBuf st;
    if (g_stat(s_path, &st) == 0)
    {
        s_seen_mtime = st.st_mtime;
        s_seen_size = (gint64)st.st_size;
    }
    else
    {
        s_seen_mtime = 0;
        s_seen_size = -1;
    }
}

static bool file_changed()
{
    GStatBuf st;
    if (g_stat(s_path, &st) != 0)
        return s_seen_size != -1;

    return st.st_mtime != s_seen_mtime || (gint64)st.st_size != s_seen_size;
}

static void ensure_loaded()
{
    if (!s_path)
    {
        StringBuf path = filename_build({aud_get_path(AudPath::UserDir), "jumpin-tags.conf"});
        s_path = String(path);
    }
    else if (s_keyfile && !file_changed())
        return;

    if (s_keyfile)
        g_key_file_free(s_keyfile);

    s_keyfile = g_key_file_new();
    // Ignore the error: a missing file just means no tags have been set yet.
    g_key_file_load_from_file(s_keyfile, s_path, G_KEY_FILE_NONE, nullptr);
    stamp_file();
}

static void save()
{
    if (!s_keyfile)
        return;

    GError * error = nullptr;
    if (!g_key_file_save_to_file(s_keyfile, s_path, &error))
    {
        AUDERR("Failed to save %s: %s\n", (const char *)s_path, error->message);
        g_error_free(error);
    }

    stamp_file();
}

static String get_value(const char * filename, const char * key)
{
    if (!filename || !filename[0])
        return String();

    ensure_loaded();

    String group = group_for(filename);
    char * val = g_key_file_get_string(s_keyfile, group, key, nullptr);
    if (!val)
        return String();

    String s(val);
    g_free(val);
    return s;
}

static void set_value(const char * filename, const char * key, const char * value)
{
    if (!filename || !filename[0])
        return;

    ensure_loaded();

    String group = group_for(filename);

    if (value && value[0])
    {
        g_key_file_set_string(s_keyfile, group, key, value);
        // Kept purely so the sidecar file is human-inspectable/greppable;
        // never read back by the plugin itself.
        g_key_file_set_string(s_keyfile, group, "uri", filename);
    }
    else
    {
        g_key_file_remove_key(s_keyfile, group, key, nullptr);

        gsize n = 0;
        char ** keys = g_key_file_get_keys(s_keyfile, group, &n, nullptr);
        bool empty_now = !keys || n == 0 || (n == 1 && !strcmp(keys[0], "uri"));
        if (keys)
            g_strfreev(keys);

        if (empty_now)
            g_key_file_remove_group(s_keyfile, group, nullptr);
    }

    save();
}

String get_skip(const char * filename) { return get_value(filename, "skip"); }
String get_jumpin(const char * filename) { return get_value(filename, "jumpin"); }

void set_skip(const char * filename, const char * value) { set_value(filename, "skip", value); }
void set_jumpin(const char * filename, const char * value) { set_value(filename, "jumpin", value); }

} // namespace JumpinTagStore
