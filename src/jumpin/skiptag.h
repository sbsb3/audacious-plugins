/*
 * skiptag.h
 * Jump In plugin for Audacious
 *
 * Parser for the SKIP tag grammar carried over from the DeaDBeeF
 * ddb_jumpin / ddb_waveform_seekbar plugins this was ported from. Kept in
 * its own translation unit, independent of both libaudcore and GTK, so it
 * can be reused as-is if ddb_waveform_seekbar is ported later.
 */

#ifndef JUMPIN_SKIPTAG_H
#define JUMPIN_SKIPTAG_H

// A SKIP tag describes how to trim a track on every playback, independently
// of the Jump In action. Accepted forms (whitespace-separated, 1 or 2
// tokens), each token either plain seconds ("30", "1.5") or "M:SS":
//
//   "5:29-"        end-only:   stop the track at 5:29
//   "0:01" / "1"   start-only: skip the first second
//   "0:01 5:29-"   both:       skip the first second, stop at 5:29
//   "1 30"         both:       the trailing '-' on the end token is
//                              optional once a start token is present
struct SkipSpec
{
    bool has_start = false;
    double start = 0.0;
    bool has_end = false;
    double end = 0.0;
};

// Parses <val> (may be null or empty) into a SkipSpec. Discards a
// nonsensical end point (at or before the start point).
SkipSpec parse_skip_tag(const char * val);

#endif // JUMPIN_SKIPTAG_H
