/*
 * wavetypes.h
 * Waveform Seekbar plugin for Audacious
 *
 * Shared plain-data types used by decode.cc (peak building), render.cc
 * (waveform drawing) and ruler.cc (time-ruler drawing). Ported from
 * ddb_waveform_seekbar's waveform.h / cache.h -- same field layout, so the
 * peak-building and rendering math carried over unchanged.
 */

#ifndef WAVEFORM_WAVETYPES_H
#define WAVEFORM_WAVETYPES_H

#include <stddef.h>

// Raw peak data for one track: `channels` interleaved channels, each made up
// of `data_len / channels / 3` (max, min, rms) triplets, stored as the
// original values * 1000 (so they fit in a short while keeping 3 decimal
// digits of precision). This is exactly ddb_waveform_seekbar's wavedata_t
// layout, kept unchanged so cache.cc's sqlite3 blob format needs no
// conversion.
struct WaveData
{
    short * data = nullptr;
    size_t data_len = 0; // in shorts, not bytes
    int channels = 0;
};

struct Color
{
    double r = 0, g = 0, b = 0, a = 1;
};

struct WaveRect
{
    double x = 0, y = 0, width = 0, height = 0;
};

struct WaveColors
{
    Color fg, rms, bg, pb, rlr, skip_start, skip_end;
};

#endif // WAVEFORM_WAVETYPES_H
