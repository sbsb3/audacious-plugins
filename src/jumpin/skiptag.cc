/*
 * skiptag.cc
 * Jump In plugin for Audacious
 *
 * Direct C++ port of parse_skip_tag()/parse_time_value() from
 * ddb_jumpin.c / skiptag.c (DeaDBeeF). Logic is unchanged; only the syntax
 * and the return convention (returns a value instead of writing through a
 * pointer) differ from the original.
 */

#include "skiptag.h"

#include <stdlib.h>
#include <string.h>

static double parse_time_value(const char * s)
{
    const char * colon = strchr(s, ':');
    if (colon)
    {
        int mins = atoi(s);
        double secs = atof(colon + 1);
        if (secs < 0)
            secs = 0;
        return mins * 60.0 + secs;
    }
    return atof(s);
}

SkipSpec parse_skip_tag(const char * val)
{
    SkipSpec spec;
    if (!val || !val[0])
        return spec;

    char buf[128];
    strncpy(buf, val, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;

    // Not reentrant, but this is only ever called from the main thread
    // (playback-ready hook / preferences), never nested.
    char * tokens[2] = {nullptr, nullptr};
    int ntok = 0;
    char * tok = strtok(buf, " \t");
    while (tok && ntok < 2)
    {
        tokens[ntok++] = tok;
        tok = strtok(nullptr, " \t");
    }
    if (ntok == 0)
        return spec;

    if (ntok == 1)
    {
        char * t = tokens[0];
        size_t len = strlen(t);
        if (len > 0 && t[len - 1] == '-')
        {
            t[len - 1] = 0;
            spec.has_end = true;
            spec.end = parse_time_value(t);
        }
        else
        {
            spec.has_start = true;
            spec.start = parse_time_value(t);
        }
    }
    else
    {
        spec.has_start = true;
        spec.start = parse_time_value(tokens[0]);

        char * t2 = tokens[1];
        size_t len2 = strlen(t2);
        if (len2 > 0 && t2[len2 - 1] == '-')
            t2[len2 - 1] = 0;
        spec.has_end = true;
        spec.end = parse_time_value(t2);
    }

    // Discard a nonsensical end point (at/before the start point, or after
    // stripping a trailing '-' it turned out empty and parsed as 0).
    if (spec.has_start && spec.has_end && spec.end <= spec.start + 0.5)
        spec.has_end = false;

    return spec;
}
