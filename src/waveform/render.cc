/*
 * render.cc
 * Waveform Seekbar plugin for Audacious
 *
 * Direct port of ddb_waveform_seekbar's render.c. See render.h for what
 * changed and what didn't.
 */

#include "render.h"

#include <math.h>
#include <stdlib.h>

#define VALUES_PER_SAMPLE 3
#define LINE_WIDTH_DEFAULT 1.0
#define LINE_WIDTH_BARS 1.0

static inline float fmax3(float a, float b) { return a > b ? a : b; }
static inline float fmin3(float a, float b) { return a < b ? a : b; }

void waveform_render_data_free(WaveRenderData * ctx)
{
    if (!ctx)
        return;

    if (ctx->samples)
    {
        for (int ch = 0; ch < ctx->num_channels; ch++)
            free(ctx->samples[ch]);
        free(ctx->samples);
    }

    delete ctx;
}

static WaveRenderData * waveform_render_data_new(int channels, int width)
{
    if (channels <= 0)
        return nullptr;

    WaveRenderData * ctx = new WaveRenderData;
    ctx->samples = (WaveSample **)calloc(channels, sizeof(WaveSample *));
    for (int ch = 0; ch < channels; ch++)
        ctx->samples[ch] = new WaveSample[width]();

    ctx->num_channels = channels;
    ctx->num_samples = width;
    return ctx;
}

static int waveform_data_render_build_sample(const WaveData & wave_data, WaveSample * sample,
                                             int sample_size, int channel, double start,
                                             double end)
{
    const int ch_offset = channel * VALUES_PER_SAMPLE;

    float min = 1.0, max = -1.0, rms = 0.0;
    const int s_end = (int)floorf(sample_size * end);

    int counter = 0;
    for (int i = (int)floor(start); i < (int)ceil(end); i++)
    {
        for (int pos = i * sample_size; pos < s_end; pos += sample_size, counter++)
        {
            int index = pos + ch_offset;
            float s_max = (float)wave_data.data[index] / 1000.f;
            float s_min = (float)wave_data.data[index + 1] / 1000.f;
            float s_rms = (float)wave_data.data[index + 2] / 1000.f;
            max = fmax3(max, s_max);
            min = fmin3(min, s_min);
            rms += s_rms * s_rms;
        }
    }

    sample->max = max;
    sample->min = min;
    sample->rms = rms;
    return counter;
}

WaveRenderData * waveform_render_data_build(const WaveData & wave_data, int width,
                                            const RenderOptions & opt)
{
    const int channels_data = wave_data.channels;
    if (channels_data <= 0 || width <= 0)
        return nullptr;

    const int channels_render = opt.mix_to_mono ? 1 : channels_data;
    const int sample_size = VALUES_PER_SAMPLE * channels_data;
    const double num_samples_per_x = wave_data.data_len / (double)(width * sample_size);

    WaveRenderData * ctx = waveform_render_data_new(channels_render, width);
    if (!ctx)
        return nullptr;

    for (int ch = 0; ch < ctx->num_channels; ch++)
    {
        WaveSample * samples = ctx->samples[ch];
        double d_start = 0.;

        for (int x = 0; x < width; x++)
        {
            const double d_end = num_samples_per_x > 0 ?
                (((x + 1) * num_samples_per_x) > 1. ? (x + 1) * num_samples_per_x : 1.) : 1.;
            WaveSample * sample = &samples[x];

            int counter = 0;
            if (opt.mix_to_mono)
            {
                for (int ch_data = 0; ch_data < channels_data; ch_data++)
                    counter += waveform_data_render_build_sample(wave_data, sample, sample_size,
                                                                  ch_data, d_start, d_end);
            }
            else
                counter += waveform_data_render_build_sample(wave_data, sample, sample_size, ch,
                                                              d_start, d_end);

            if (counter > 0)
            {
                sample->rms /= counter;
                sample->rms = sqrtf(sample->rms);
            }

            d_start = d_end;
        }
    }

    return ctx;
}

// --- drawing -----------------------------------------------------------

struct WavePoint
{
    double x, y;
};

struct WaveLine
{
    double x1, y1, x2, y2;
};

typedef void (*RenderSampleFunc)(cairo_t * cr, WaveSample * sample, WavePoint * point,
                                 double y_scale_1, double y_scale_2);

/* copied from ardour3 */
static inline float _log_meter(float power, double lower_db, double upper_db, double non_linearity)
{
    return (power < lower_db ? 0.0 : pow((power - lower_db) / (upper_db - lower_db), non_linearity));
}

static inline float alt_log_meter(float power) { return _log_meter(power, -192.0, 0.0, 8.0); }

static inline float coefficient_to_dB(float coeff) { return 20.0f * log10(coeff); }
/* end of ardour copy */

static inline float sample_log_scale(float sample)
{
    if (sample > 0.0)
        return alt_log_meter(coefficient_to_dB(sample));
    return -alt_log_meter(coefficient_to_dB(-sample));
}

static float sample_value_scale(float value, float scale, bool log_scale)
{
    if (log_scale)
        value = sample_log_scale(value);
    return value * scale;
}

static void waveform_render_samples_loop_reverse(cairo_t * cr, WaveSample * samples,
                                                  RenderSampleFunc render_sample, double y_scale_1,
                                                  double y_scale_2, double x_start, double y_start,
                                                  double width)
{
    if (!render_sample)
        return;

    const int width_i = (int)floor(width) - 1;
    for (int x = width_i; x >= x_start; x--)
    {
        WaveSample * sample = &samples[x];
        WavePoint point = {(double)x, y_start};
        render_sample(cr, sample, &point, y_scale_1, y_scale_2);
    }
}

static void waveform_render_samples_loop(cairo_t * cr, WaveSample * samples,
                                         RenderSampleFunc render_sample, double y_scale_1,
                                         double y_scale_2, double x_start, double y_start,
                                         double width)
{
    if (!render_sample)
        return;

    const int width_i = (int)floor(width);
    for (int x = 0; x < width_i; x++)
    {
        WaveSample * sample = &samples[x];
        WavePoint point = {x_start + x, y_start};
        render_sample(cr, sample, &point, y_scale_1, y_scale_2);
    }
}

static inline void waveform_render_bars_sample_generic(cairo_t * cr, double s1, double s2,
                                                        WavePoint * point, double y_scale_1,
                                                        double y_scale_2, bool log_scale)
{
    const double x = point->x;
    const double y = point->y;
    const double y_1 = y - sample_value_scale(s1, y_scale_1, log_scale);
    const double y_2 = y - sample_value_scale(s2, y_scale_2, log_scale);

    cairo_move_to(cr, x, y_1);
    cairo_line_to(cr, x, y_2);
}

// bound at call sites via a thread-local-ish trick isn't needed: we bind
// log_scale through a couple of small wrapper closures generated per draw
// call (see waveform_render_wave_bar_values / _default_values below).
static bool g_log_scale_bars = false;

static void waveform_render_bars_sample_minmax(cairo_t * cr, WaveSample * sample, WavePoint * point,
                                               double y_scale_1, double y_scale_2)
{
    waveform_render_bars_sample_generic(cr, sample->max, sample->min, point, y_scale_1, y_scale_2,
                                        g_log_scale_bars);
}

static void waveform_render_bars_sample_rms(cairo_t * cr, WaveSample * sample, WavePoint * point,
                                            double y_scale_1, double y_scale_2)
{
    waveform_render_bars_sample_generic(cr, sample->rms, -sample->rms, point, y_scale_1, y_scale_2,
                                        g_log_scale_bars);
}

static cairo_pattern_t * waveform_render_soundcloud_pattern_get(cairo_t * cr, const WaveColors & color,
                                                                 const WaveLine & vec_pat)
{
    cairo_pattern_t * lin_pat =
        cairo_pattern_create_linear(vec_pat.x1, vec_pat.y1, vec_pat.x2, vec_pat.y2);
    cairo_pattern_add_color_stop_rgba(lin_pat, 0.0, color.fg.r, color.fg.g, color.fg.b, 0.7);
    cairo_pattern_add_color_stop_rgba(lin_pat, 0.7, color.fg.r, color.fg.g, color.fg.b, 1.0);
    cairo_pattern_add_color_stop_rgba(lin_pat, 0.7, color.fg.r, color.fg.g, color.fg.b, 0.5);
    cairo_pattern_add_color_stop_rgba(lin_pat, 1.0, color.fg.r, color.fg.g, color.fg.b, 0.5);
    cairo_set_source(cr, lin_pat);
    return lin_pat;
}

enum SampleType
{
    SAMPLE_MAX,
    SAMPLE_RMS_MAX,
};

static void waveform_render_wave_bar_values(cairo_t * cr, WaveSample * samples,
                                            const WaveColors & color, int type,
                                            const WaveRect & rect, const RenderOptions & opt)
{
    double x = rect.x, y = rect.y, width = rect.width, height = rect.height;

    double y_scale_1 = 0.5 * height;
    if (opt.soundcloud_style)
        y_scale_1 = 0.7 * height;
    double y_scale_2 = height - y_scale_1;
    double y_center = y_scale_1 + y;

    cairo_move_to(cr, x, y_center);

    g_log_scale_bars = opt.log_scale;
    RenderSampleFunc render_func =
        (type == SAMPLE_RMS_MAX) ? waveform_render_bars_sample_rms : waveform_render_bars_sample_minmax;

    cairo_pattern_t * lin_pat = nullptr;
    if (opt.soundcloud_style)
    {
        WaveLine vec_pat = {x, y, x, y + height};
        lin_pat = waveform_render_soundcloud_pattern_get(cr, color, vec_pat);
    }

    waveform_render_samples_loop(cr, samples, render_func, y_scale_1, y_scale_2, x, y_center, width);
    cairo_stroke(cr);

    if (lin_pat)
        cairo_pattern_destroy(lin_pat);
}

void waveform_draw_wave_bars(WaveSample * samples, const WaveColors & colors, cairo_t * cr,
                             const WaveRect & rect, const RenderOptions & opt)
{
    cairo_set_line_width(cr, LINE_WIDTH_BARS);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    cairo_set_source_rgba(cr, colors.fg.r, colors.fg.g, colors.fg.b, colors.fg.a);

    waveform_render_wave_bar_values(cr, samples, colors, SAMPLE_MAX, rect, opt);

    if (opt.display_rms)
    {
        cairo_set_source_rgba(cr, colors.rms.r, colors.rms.g, colors.rms.b, colors.rms.a);
        waveform_render_wave_bar_values(cr, samples, colors, SAMPLE_RMS_MAX, rect, opt);
    }
}

static bool g_log_scale_default = false;

static inline void waveform_render_default_sample_generic(cairo_t * cr, double sample,
                                                           WavePoint * point, double y_scale)
{
    const double x = point->x;
    const double y = point->y - sample_value_scale((float)sample, (float)y_scale, g_log_scale_default);
    cairo_line_to(cr, x, y);
}

static void waveform_render_default_sample_max(cairo_t * cr, WaveSample * sample, WavePoint * point,
                                               double y_scale_1, double)
{
    waveform_render_default_sample_generic(cr, sample->max, point, y_scale_1);
}

static void waveform_render_default_sample_min(cairo_t * cr, WaveSample * sample, WavePoint * point,
                                               double y_scale_1, double)
{
    waveform_render_default_sample_generic(cr, sample->min, point, y_scale_1);
}

static void waveform_render_default_sample_rms1(cairo_t * cr, WaveSample * sample, WavePoint * point,
                                                double y_scale_1, double)
{
    waveform_render_default_sample_generic(cr, sample->rms, point, y_scale_1);
}

static void waveform_render_default_sample_rms2(cairo_t * cr, WaveSample * sample, WavePoint * point,
                                                double y_scale_1, double)
{
    waveform_render_default_sample_generic(cr, -sample->rms, point, y_scale_1);
}

enum SampleGroup
{
    SAMPLE_MIN_MAX,
    SAMPLE_RMS_MIN_MAX,
};

static void waveform_render_wave_default_values(cairo_t * cr, WaveSample * samples,
                                                 const WaveColors & color, int type,
                                                 const WaveRect & rect, const RenderOptions & opt)
{
    double x = rect.x, y = rect.y, width = rect.width, height = rect.height;

    RenderSampleFunc render_func_1, render_func_2;
    switch (type)
    {
    case SAMPLE_MIN_MAX:
        render_func_1 = waveform_render_default_sample_max;
        render_func_2 = waveform_render_default_sample_min;
        break;
    case SAMPLE_RMS_MIN_MAX:
        render_func_1 = waveform_render_default_sample_rms1;
        render_func_2 = waveform_render_default_sample_rms2;
        break;
    default:
        return;
    }

    g_log_scale_default = opt.log_scale;

    double y_scale = 0.5 * height;
    double y_center = y_scale + y;
    if (opt.soundcloud_style)
    {
        y_scale = 0.7 * height;
        y_center = y_scale + y;
    }

    cairo_pattern_t * lin_pat = nullptr;
    if (opt.soundcloud_style)
    {
        WaveLine vec_pat = {x, y, x, y + height};
        lin_pat = waveform_render_soundcloud_pattern_get(cr, color, vec_pat);
    }

    cairo_move_to(cr, x, y_center);
    waveform_render_samples_loop(cr, samples, render_func_1, y_scale, y_scale, x, y_center, width);

    y_scale = height - y_scale;
    waveform_render_samples_loop_reverse(cr, samples, render_func_2, y_scale, y_scale, x, y_center,
                                         width);

    if (!opt.fill_waveform)
        cairo_stroke(cr);
    else
    {
        cairo_line_to(cr, x, y);
        cairo_close_path(cr);
        cairo_fill(cr);
    }

    if (lin_pat)
        cairo_pattern_destroy(lin_pat);
}

void waveform_draw_wave_default(WaveSample * samples, const WaveColors & colors, cairo_t * cr,
                                const WaveRect & rect, const RenderOptions & opt)
{
    cairo_set_line_width(cr, LINE_WIDTH_DEFAULT);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
    cairo_set_source_rgba(cr, colors.fg.r, colors.fg.g, colors.fg.b, colors.fg.a);

    waveform_render_wave_default_values(cr, samples, colors, SAMPLE_MIN_MAX, rect, opt);

    if (opt.display_rms)
    {
        cairo_set_source_rgba(cr, colors.rms.r, colors.rms.g, colors.rms.b, colors.rms.a);
        waveform_render_wave_default_values(cr, samples, colors, SAMPLE_RMS_MIN_MAX, rect, opt);
    }
}
