/*
 * ruler.h
 * Waveform Seekbar plugin for Audacious
 *
 * Direct port of ddb_waveform_seekbar's ruler.c/ruler.h (time-ruler drawing
 * below the waveform). No config globals were involved here, so this ported
 * essentially unchanged, apart from `font_size` which the original hardcoded
 * (RULER_FONT_SIZE == 8.0, tiny at typical screen DPI) and which this port
 * takes as a parameter so it can be set from the prefs pane.
 */

#ifndef WAVEFORM_RULER_H
#define WAVEFORM_RULER_H

#include <cairo.h>

#include "wavetypes.h"

void waveform_render_ruler(cairo_t * cr, const WaveColors & color, float duration,
                           const WaveRect & rect, double font_size);

#endif // WAVEFORM_RULER_H
