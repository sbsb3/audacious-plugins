/*
 * render.h
 * Waveform Seekbar plugin for Audacious
 *
 * Ported from ddb_waveform_seekbar's render.c/render.h. The peak-bucketing
 * and cairo drawing math is unchanged; only the interface differs, since the
 * original read a handful of global CONFIG_* variables that this port
 * instead passes explicitly as a RenderOptions struct (Audacious plugins
 * read their config live via aud_get_bool()/aud_get_int(), so there's no
 * equivalent persistent global state to hang these off of).
 */

#ifndef WAVEFORM_RENDER_H
#define WAVEFORM_RENDER_H

#include <cairo.h>

#include "wavetypes.h"

struct WaveSample
{
    float max = 0, min = 0, rms = 0;
};

// One re-bucketed set of samples per rendered channel, at the widget's
// current pixel width. Built from a WaveData (which is stored/cached at a
// fixed resolution independent of any widget's width -- see decode.h).
struct WaveRenderData
{
    WaveSample ** samples = nullptr; // [channel][x], x in [0, num_samples)
    int num_channels = 0;
    int num_samples = 0; // == width
};

struct RenderOptions
{
    bool mix_to_mono = false;
    bool log_scale = false;
    bool display_rms = true;
    bool fill_waveform = true;
    bool soundcloud_style = false;
};

void waveform_render_data_free(WaveRenderData * ctx);

// Re-buckets `wave_data` (at whatever resolution it was decoded/cached at)
// down to `width` samples per rendered channel. Returns nullptr if
// wave_data has no channels.
WaveRenderData * waveform_render_data_build(const WaveData & wave_data, int width,
                                            const RenderOptions & opt);

void waveform_draw_wave_default(WaveSample * samples, const WaveColors & colors,
                                cairo_t * cr, const WaveRect & rect,
                                const RenderOptions & opt);

void waveform_draw_wave_bars(WaveSample * samples, const WaveColors & colors,
                             cairo_t * cr, const WaveRect & rect,
                             const RenderOptions & opt);

#endif // WAVEFORM_RENDER_H
