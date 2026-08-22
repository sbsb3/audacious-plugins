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
 * hash of the track's filename URI.
 *
 * NOTE: this file is a deliberate byte-for-byte copy of jumpin/tagstore.{h,cc}.
 * The waveform plugin's right-click "Set SKIP Start/End Here" writes through
 * this same store, at the same file path (jumpin-tags.conf) with the same
 * hashing scheme, so SKIP tags set from either plugin are visible to the
 * other. Since plugins are built as independent shared_modules there's no
 * cheap way to share a compiled object between them, so the source is
 * duplicated instead; if you change the storage format here, change it in
 * jumpin/tagstore.cc too (and vice versa).
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
