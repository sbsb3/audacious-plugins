/*
 * decode.h
 * Waveform Seekbar plugin for Audacious
 *
 * Builds peak data from an audio file, replacing what ddb_waveform_seekbar
 * got almost for free from DeaDBeeF's DB_decoder_t::open/init/read
 * interface (any plugin can ask any decoder for raw PCM on demand).
 * Audacious's InputPlugin has no equivalent -- InputPlugin::play() decodes
 * straight into the active output device, not into a buffer a second
 * plugin can request -- so this bundles its own minimal decode path via
 * FFmpeg (already an optional dependency of this tree, see src/ffaudio).
 *
 * Deliberately independent of libaudcore/GTK: this runs on a background
 * thread (see waveform.cc), and must not touch either.
 */

#ifndef WAVEFORM_DECODE_H
#define WAVEFORM_DECODE_H

#include "wavetypes.h"

// Decodes the local file at `path` (a plain filesystem path, not a URI) and
// builds `num_buckets` peak buckets per channel into `out` (caller owns
// out.data, free with delete[]). Returns false (leaving `out` untouched) on
// failure. May take a while for long files -- always call from a
// background thread.
bool waveform_decode_build(const char * path, int num_buckets, WaveData & out);

#endif // WAVEFORM_DECODE_H
