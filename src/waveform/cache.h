/*
 * cache.h
 * Waveform Seekbar plugin for Audacious
 *
 * sqlite3-backed cache of decoded peak data, keyed by filename URI, so a
 * track's waveform only has to be decoded once. Ported from
 * ddb_waveform_seekbar's cache.c/cache.h; same schema, so the blob format
 * (see wavetypes.h's WaveData) is unchanged. Differences from the original:
 * bound parameters instead of sqlite3_mprintf('%q', ...) string building,
 * and every call is internally serialized with a mutex so it's safe to call
 * from the background decode thread as well as the main thread.
 */

#ifndef WAVEFORM_CACHE_H
#define WAVEFORM_CACHE_H

#include <stddef.h>

#include "wavetypes.h"

namespace WaveCache
{
// Opens (creating if needed) <dir>/waveform-cache.db. Safe to call more
// than once. No-op (and read()/write() silently no-op too) if it fails.
void open_db(const char * dir);
void close_db();

// On a hit, fills `out` (caller owns out.data, free with delete[]) and
// returns true.
bool read(const char * key, WaveData & out);

// `data_len` is in shorts (as in WaveData::data_len), not bytes.
void write(const char * key, const short * data, size_t data_len, int channels);

void remove(const char * key);
} // namespace WaveCache

#endif // WAVEFORM_CACHE_H
