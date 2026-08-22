/*
 * ruler.cc
 * Waveform Seekbar plugin for Audacious
 *
 * Direct port of ddb_waveform_seekbar's ruler.c. Logic unchanged.
 */

#include "ruler.h"

#include <math.h>
#include <stdio.h>

#define TEXT_SPACING 40
#define TEXT_MARKER_SPACING 3
#define RULER_MAX_LABELS 30
#define RULER_LINE_WIDTH 1.0
#define RULER_SUB_MARKER_SPACING_MIN 3.0

enum TimeValueID
{
    TIME_ID_6H,
    TIME_ID_1H,
    TIME_ID_30M,
    TIME_ID_15M,
    TIME_ID_10M,
    TIME_ID_5M,
    TIME_ID_2M,
    TIME_ID_1M,
    TIME_ID_30S,
    TIME_ID_10S,
    TIME_ID_5S,
    TIME_ID_1S,
    TIME_ID_500MS,
    TIME_ID_100MS,
    TIME_ID_50MS,
    TIME_ID_10MS,
    N_TIME_IDS,
};

struct RulerTimeValue
{
    TimeValueID id;
    float value;
    int sub_div;
};

static RulerTimeValue time_scale[] = {
    {TIME_ID_6H, 6 * 3600.f, 3},   {TIME_ID_1H, 3600.f, 2},   {TIME_ID_30M, 1800.f, 2},
    {TIME_ID_15M, 900.f, 3},       {TIME_ID_10M, 600.f, 2},   {TIME_ID_5M, 300.f, 5},
    {TIME_ID_2M, 120.f, 2},        {TIME_ID_1M, 60.f, 2},     {TIME_ID_30S, 30.f, 3},
    {TIME_ID_10S, 10.f, 2},        {TIME_ID_5S, 5.f, 2},      {TIME_ID_1S, 1.f, 5},
    {TIME_ID_500MS, 0.5f, 5},      {TIME_ID_100MS, 0.1f, 5},  {TIME_ID_50MS, 0.05f, 5},
    {TIME_ID_10MS, 0.01f, 5},
};

struct RulerTimeResolution
{
    RulerTimeValue value;
    int n;
};

static double ruler_text_height_get(cairo_t * cr)
{
    cairo_text_extents_t text_dim;
    cairo_text_extents(cr, "Test", &text_dim);
    return text_dim.height;
}

static void ruler_format_time(char * dest, size_t dest_size, RulerTimeValue * time_val, int n)
{
    const double time_in_seconds = n * time_val->value;

    const int hours = (int)time_in_seconds / 3600;
    const int minutes = (int)time_in_seconds / 60;
    const int seconds = (int)time_in_seconds;

    int time_remaining = 0;
    switch (time_val->id)
    {
    case TIME_ID_6H:
    case TIME_ID_1H:
        snprintf(dest, dest_size, "%d:00:00", hours);
        break;
    case TIME_ID_30M:
    case TIME_ID_15M:
    case TIME_ID_10M:
    case TIME_ID_5M:
    case TIME_ID_2M:
    case TIME_ID_1M:
        if (time_in_seconds >= 3600)
        {
            time_remaining = seconds % 3600 / 60;
            snprintf(dest, dest_size, "%d:%02d:00", hours, time_remaining);
        }
        else
            snprintf(dest, dest_size, "%d:00", minutes);
        break;
    case TIME_ID_30S:
    case TIME_ID_10S:
    case TIME_ID_5S:
    case TIME_ID_1S:
        if (time_in_seconds >= 60)
        {
            time_remaining = seconds % 60;
            snprintf(dest, dest_size, "%d:%02d", minutes, time_remaining);
        }
        else
            snprintf(dest, dest_size, "0:%02d", seconds);
        break;
    case TIME_ID_500MS:
    case TIME_ID_100MS:
    case TIME_ID_50MS:
    case TIME_ID_10MS:
        snprintf(dest, dest_size, "%.2f", time_in_seconds);
        break;
    default:
        return;
    }
}

static void ruler_time_resolution_build(RulerTimeResolution * res, float duration)
{
    for (int i = 0; i < N_TIME_IDS; i++)
    {
        RulerTimeResolution * r = &res[i];
        r->value = time_scale[i];
        r->n = (int)floorf(duration / time_scale[i].value);
    }
}

static bool ruler_time_fits_width(cairo_t * cr, char * dest, size_t dest_size,
                                  RulerTimeResolution * res, float duration, double width)
{
    if (res->n > RULER_MAX_LABELS)
        return false;

    RulerTimeValue * time_val = &res->value;
    double text_width = 0;
    const double x_start = time_val->value / duration * width;

    for (int i = 1; i <= res->n; i++)
    {
        ruler_format_time(dest, dest_size, time_val, i);
        cairo_text_extents_t text_dim;
        cairo_text_extents(cr, dest, &text_dim);
        text_width += text_dim.width + TEXT_SPACING;
    }
    return floor((width - x_start) / text_width) >= 1;
}

static RulerTimeResolution * ruler_time_find_resolution(cairo_t * cr, RulerTimeResolution * resolutions,
                                                        float duration, double width)
{
    RulerTimeResolution * res = nullptr;
    int n_max = 0;

    for (int i = 0; i < N_TIME_IDS; i++)
    {
        RulerTimeResolution * res_tmp = &resolutions[i];
        if (res_tmp->n <= 0)
            continue;

        char time_text[100] = "";
        bool fits = ruler_time_fits_width(cr, time_text, sizeof time_text, res_tmp, duration, width);
        if (fits && res_tmp->n > n_max)
        {
            res = res_tmp;
            n_max = res_tmp->n;
        }
    }
    return res;
}

static void ruler_sub_marker_draw(cairo_t * cr, RulerTimeResolution * res, double x, double y,
                                  double width, double height)
{
    const int sub_div = res->value.sub_div;
    const double marker_dist = width / sub_div;

    if (marker_dist >= RULER_SUB_MARKER_SPACING_MIN)
    {
        const double m_height = floor(height / 3);
        double x_start = x + marker_dist;
        for (int i = 1; i < sub_div; i++)
        {
            cairo_move_to(cr, x_start, y);
            cairo_line_to(cr, x_start, y - m_height);
            cairo_stroke(cr);
            x_start += marker_dist;
        }
    }
}

void waveform_render_ruler(cairo_t * cr, const WaveColors & color, float duration, const WaveRect & rect,
                           double font_size)
{
    cairo_set_source_rgba(cr, color.bg.r, color.bg.g, color.bg.b, color.bg.a);
    cairo_rectangle(cr, rect.x, rect.y, rect.width, rect.height);
    cairo_fill(cr);

    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    cairo_set_line_width(cr, RULER_LINE_WIDTH);
    cairo_set_font_size(cr, font_size);
    cairo_set_source_rgba(cr, color.rlr.r, color.rlr.g, color.rlr.b, color.rlr.a);

    cairo_move_to(cr, rect.x, rect.height);
    cairo_line_to(cr, rect.width, rect.height);
    cairo_stroke(cr);

    if (duration <= 0.f)
        return;

    RulerTimeResolution resolutions[N_TIME_IDS];
    ruler_time_resolution_build(resolutions, duration);

    RulerTimeResolution * res = ruler_time_find_resolution(cr, resolutions, duration, rect.width);
    if (!res)
        return;

    const double x_start = res->value.value / duration * rect.width;
    const double center_abs = rect.height / 2.0;
    const double center = (rect.height - RULER_LINE_WIDTH) / 2.0;
    const double y = center + ruler_text_height_get(cr) / 2.0;

    double x = rect.x;
    for (int i = 1; i <= res->n; i++)
    {
        ruler_sub_marker_draw(cr, res, x, rect.height, x_start, center_abs);

        x += x_start;

        cairo_move_to(cr, x, center_abs);
        cairo_line_to(cr, x, rect.height);
        cairo_stroke(cr);

        cairo_move_to(cr, x + TEXT_MARKER_SPACING, y);
        char time_text[100] = "";
        ruler_format_time(time_text, sizeof time_text, &res->value, i);
        cairo_show_text(cr, time_text);
    }

    ruler_sub_marker_draw(cr, res, x, rect.height, x_start, center_abs);
}
