/*
 * tagstore.h
 * Jump In plugin for Audacious
 *
 * Per-track SKIP / JUMPIN values, keyed by filename URI.
 *
 * DeaDBeeF exposes an open key/value metadata dictionary per track
 * (pl_find_meta()), so ddb_jumpin could read arbitrary custom tags
 * directly. Audacious's Tuple is a fixed-field schema with no such
 * extension point, and libaudtag (the tag reader/writer) only knows how to
 * read/write those fixed fields -- so there is nowhere in core to stash a
 * custom SKIP/JUMPIN tag without patching core.
 *
 * Rather than require that, values are kept in a small sidecar key file
 * (jumpin-tags.conf, in Audacious's per-user data directory), keyed by a
 * hash of the track's filename URI. This is the same design this plugin's
 * plan document flagged as a shared dependency with a possible future port
 * of ddb_waveform_seekbar, which writes the same SKIP grammar.
 */

#ifndef JUMPIN_TAGSTORE_H
#define JUMPIN_TAGSTORE_H

#include <libaudcore/objects.h>

namespace JumpinTagStore
{
// Returns an empty String if no value is set.
String get_skip(const char * filename);
String get_jumpin(const char * filename);

// A null or empty value clears the entry.
void set_skip(const char * filename, const char * value);
void set_jumpin(const char * filename, const char * value);
} // namespace JumpinTagStore

#endif // JUMPIN_TAGSTORE_H
