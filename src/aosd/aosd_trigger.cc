/*
*
* Author: Giacomo Lozito <james@develia.org>, (C) 2005-2007
*
* This program is free software; you can redistribute it and/or modify it
* under the terms of the GNU General Public License as published by the
* Free Software Foundation; either version 2 of the License, or (at your
* option) any later version.
*
* This program is distributed in the hope that it will be useful, but
* WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* General Public License for more details.
*
* You should have received a copy of the GNU General Public License along
* with this program; if not, write to the Free Software Foundation, Inc.,
* 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*
*/

#include <glib.h>
#include <stdlib.h>
#include <string.h>

#include <libaudcore/drct.h>
#include <libaudcore/i18n.h>
#include <libaudcore/audstrings.h>
#include <libaudcore/hook.h>

#include "aosd_trigger.h"
#include "aosd_trigger_private.h"
#include "aosd_cfg.h"
#include "aosd_osd.h"

/* Case-insensitive strstr(), since plain strcasestr() isn't standard. */
static const char * aosd_strcasestr (const char * haystack, const char * needle)
{
  size_t needle_len = strlen (needle);
  for (const char * p = haystack; * p; p ++)
  {
    if (! g_ascii_strncasecmp (p, needle, needle_len))
      return p;
  }
  return nullptr;
}

/* Decoders spell out codec names very differently -- e.g. the built-in FLAC
 * plugin sets "Free Lossless Audio Codec (FLAC)", ffmpeg's long_name is
 * "FLAC (Free Lossless Audio Codec)", and mpg123 gives "MPEG-1 layer 3".
 * This maps such names down to the short form most people actually use
 * (FLAC, MP3, AAC, ...), falling back to the original string unchanged if
 * nothing recognizable is found. */
static StringBuf aosd_trigger_alias_codec (const char * codec)
{
  /* "MPEG-<version> layer <n>" (mpg123) */
  const char * mpeg = aosd_strcasestr (codec, "mpeg");
  const char * layer = mpeg ? aosd_strcasestr (mpeg, "layer") : nullptr;
  if (layer)
  {
    int n = atoi (layer + strlen ("layer"));
    if (n >= 1 && n <= 3)
      return str_printf ("MP%d", n);
  }

  /* "SHORT (long description)", e.g. ffmpeg's "FLAC (Free Lossless ...)" */
  const char * open_paren = strchr (codec, '(');
  if (open_paren && open_paren > codec && open_paren[-1] == ' ')
  {
    StringBuf lead = str_copy (codec, open_paren - 1 - codec);
    if (lead.len () > 0 && lead.len () <= 8 && ! strpbrk (lead, " \t("))
      return lead;
  }

  /* "long description (SHORT)", e.g. the built-in flac plugin */
  int len = strlen (codec);
  if (len > 2 && codec[len - 1] == ')')
  {
    const char * last_open = strrchr (codec, '(');
    if (last_open && last_open > codec && last_open[-1] == ' ')
    {
      StringBuf trail = str_copy (last_open + 1, codec + len - 1 - (last_open + 1));
      if (trail.len () > 0 && trail.len () <= 8 && ! strpbrk (trail, " \t("))
        return trail;
    }
  }

  /* plain names with no parenthesized short form */
  static const struct { const char * needle; const char * alias; } known[] =
  {
    { "opus", "Opus" },
    { "vorbis", "Vorbis" },
    { "flac", "FLAC" },
    { "wavpack", "WavPack" },
    { "monkey", "APE" },
    { "musepack", "MPC" },
    { "true audio", "TTA" },
    { "trueaudio", "TTA" },
    { "windows media", "WMA" },
    { "apple lossless", "ALAC" },
    { "advanced audio coding", "AAC" },
    { "aac", "AAC" },
    { "mpeg audio layer 3", "MP3" },
    { "mpeg audio layer 2", "MP2" },
    { "mpeg audio layer 1", "MP1" },
  };

  for (auto & k : known)
  {
    if (aosd_strcasestr (codec, k.needle))
      return str_copy (k.alias);
  }

  return str_copy (codec);
}

/* One entry of the tiny template language used to render
   aosd_cfg_osd_text_t::format (see aosd_cfg.h for the token syntax). */
typedef StringBuf (* aosd_format_transform_t) (const char * value);

typedef struct
{
  const char * name;
  Tuple::Field field;
  bool is_int; /* true for fields read with get_int() (e.g. bitrate) */
  aosd_format_transform_t transform; /* optional post-processing, or nullptr */
}
aosd_format_field_t;

static const aosd_format_field_t aosd_format_fields[] =
{
  { "title",   Tuple::Title,   false, nullptr },
  { "artist",  Tuple::Artist,  false, nullptr },
  { "album",   Tuple::Album,   false, nullptr },
  { "codec",   Tuple::Codec,   false, aosd_trigger_alias_codec },
  { "quality", Tuple::Quality, false, nullptr },
  { "bitrate", Tuple::Bitrate, true,  nullptr },
};

/* If <s> starts with "<fieldname>%", appends the field's value (if any) to
 * <line> and returns a pointer just past the closing '%'.  Sets *had_value
 * if the field actually had a (non-empty / positive) value.  Returns
 * nullptr, leaving <line> untouched, if <s> does not name a known field. */
static const char * aosd_trigger_format_field (const char * s, const Tuple & tuple,
 GString * line, bool * had_value)
{
  for (const aosd_format_field_t & f : aosd_format_fields)
  {
    size_t len = strlen (f.name);
    if (! strncmp (s, f.name, len) && s[len] == '%')
    {
      if (f.is_int)
      {
        int value = tuple.get_int (f.field);
        if (value > 0)
        {
          g_string_append_printf (line, "%d", value);
          * had_value = true;
        }
      }
      else
      {
        String value = tuple.get_str (f.field);
        if (value && value[0])
        {
          if (f.transform)
          {
            StringBuf alias = f.transform (value);
            g_string_append (line, alias);
          }
          else
            g_string_append (line, value);
          * had_value = true;
        }
      }
      return s + len + 1;
    }
  }
  return nullptr;
}

/* Renders <format> (see aosd_cfg.h for the token syntax) against <tuple>.
 * A line that consists solely of tokens which all turned out empty (e.g.
 * "%bitrate% kbps" when bitrate is unknown) is dropped, so that fields the
 * current song lacks don't leave blank or dangling lines behind.  Returns a
 * newly allocated string; the caller must g_free() it. */
static char * aosd_trigger_format_text (const Tuple & tuple, const char * format)
{
  GString * result = g_string_new (nullptr);
  GString * line = g_string_new (nullptr);
  bool line_has_token = false;
  bool line_has_value = false;

  auto flush_line = [&] ()
  {
    if (! line_has_token || line_has_value)
    {
      if (result->len)
        g_string_append_c (result, '\n');
      g_string_append (result, line->str);
    }
    g_string_truncate (line, 0);
    line_has_token = false;
    line_has_value = false;
  };

  for (const char * s = format; * s; )
  {
    if (s[0] == '\\' && s[1] == 'n')
    {
      flush_line ();
      s += 2;
    }
    else if (s[0] == '%' && s[1] == '%')
    {
      g_string_append_c (line, '%');
      s += 2;
    }
    else if (s[0] == '%')
    {
      bool had_value = false;
      const char * next = aosd_trigger_format_field (s + 1, tuple, line, & had_value);
      if (next)
      {
        line_has_token = true;
        line_has_value = line_has_value || had_value;
        s = next;
      }
      else
      {
        g_string_append_c (line, * s);
        s ++;
      }
    }
    else
    {
      g_string_append_c (line, * s);
      s ++;
    }
  }
  flush_line ();

  g_string_free (line, true);
  return g_string_free (result, false); /* caller owns the returned string */
}

/* prototypes of trigger functions */
static void aosd_trigger_func_pb_start_onoff ( bool );
static void aosd_trigger_func_pb_start_cb ( void * , void * );
static void aosd_trigger_func_pb_titlechange_onoff ( bool );
static void aosd_trigger_func_pb_titlechange_cb ( void * , void * );
static void aosd_trigger_func_pb_pauseon_onoff ( bool );
static void aosd_trigger_func_pb_pauseon_cb ( void * , void * );
static void aosd_trigger_func_pb_pauseoff_onoff ( bool );
static void aosd_trigger_func_pb_pauseoff_cb ( void * , void * );
static void aosd_trigger_func_hook_cb ( void * markup_text , void * unused );

/* map trigger codes to trigger objects */
aosd_trigger_t aosd_triggers[] =
{
  // AOSD_TRIGGER_PB_START
  { N_("Playback Start") ,
    N_("Triggers OSD when a playlist entry is played.") ,
    aosd_trigger_func_pb_start_onoff ,
    aosd_trigger_func_pb_start_cb },

  // AOSD_TRIGGER_PB_TITLECHANGE
  { N_("Title Change") ,
    N_("Triggers OSD when the song title changes (for internet streams).") ,
    aosd_trigger_func_pb_titlechange_onoff ,
    aosd_trigger_func_pb_titlechange_cb },

  // AOSD_TRIGGER_PB_PAUSEON
  { N_("Pause On") ,
    N_("Triggers OSD when playback is paused.") ,
    aosd_trigger_func_pb_pauseon_onoff ,
    aosd_trigger_func_pb_pauseon_cb },

  // AOSD_TRIGGER_PB_PAUSEOFF
  { N_("Pause Off") ,
    N_("Triggers OSD when playback is unpaused.") ,
    aosd_trigger_func_pb_pauseoff_onoff ,
    aosd_trigger_func_pb_pauseoff_cb }
};

static_assert (aud::n_elems (aosd_triggers) == AOSD_NUM_TRIGGERS, "update aosd_triggers");

/* TRIGGER API */

const char *
aosd_trigger_get_name ( int trig_code )
{
  if (trig_code >= 0 && trig_code < aud::n_elems (aosd_triggers))
    return aosd_triggers[trig_code].name;
  return nullptr;
}


const char *
aosd_trigger_get_desc ( int trig_code )
{
  if (trig_code >= 0 && trig_code < aud::n_elems (aosd_triggers))
    return aosd_triggers[trig_code].desc;
  return nullptr;
}


void aosd_trigger_start (const aosd_cfg_osd_trigger_t & cfg_trigger)
{
  for (int i = 0; i < AOSD_NUM_TRIGGERS; i ++)
  {
    if (cfg_trigger.enabled[i])
      aosd_triggers[i].onoff_func (true);
  }

  /* When called, this hook will display the text of the user pointer
     or the current playing song, if nullptr */
  hook_associate( "aosd toggle" , aosd_trigger_func_hook_cb , nullptr );
}


void aosd_trigger_stop (const aosd_cfg_osd_trigger_t & cfg_trigger)
{
  hook_dissociate( "aosd toggle" , aosd_trigger_func_hook_cb );

  for (int i = 0; i < AOSD_NUM_TRIGGERS; i ++)
  {
    if (cfg_trigger.enabled[i])
      aosd_triggers[i].onoff_func (false);
  }
}


/* TRIGGER FUNCTIONS */

static void
aosd_trigger_func_pb_start_onoff ( bool turn_on )
{
  if (turn_on == true)
    hook_associate("playback ready", aosd_trigger_func_pb_start_cb, nullptr);
  else
    hook_dissociate("playback ready", aosd_trigger_func_pb_start_cb);
}

static void
aosd_trigger_func_pb_start_cb(void * hook_data, void * user_data)
{
  Tuple tuple = aud_drct_get_tuple ();
  char * text = aosd_trigger_format_text (tuple, global_config.text.format);
  char * markup = g_markup_printf_escaped ("<span font_desc='%s'>%s</span>",
   (const char *) global_config.text.fonts_name[0], text);
  g_free (text);

  aosd_osd_display (markup, & global_config, false);
  g_free (markup);
}

typedef struct
{
  String title;
  String filename;
}
aosd_pb_titlechange_prevs_t;

static void
aosd_trigger_func_pb_titlechange_onoff ( bool turn_on )
{
  static aosd_pb_titlechange_prevs_t *prevs = nullptr;

  if ( turn_on == true )
  {
    prevs = new aosd_pb_titlechange_prevs_t;
    hook_associate( "title change" , aosd_trigger_func_pb_titlechange_cb , prevs );
  }
  else
  {
    hook_dissociate( "title change" , aosd_trigger_func_pb_titlechange_cb );
    if ( prevs != nullptr )
    {
      delete prevs;
      prevs = nullptr;
    }
  }
}

static void
aosd_trigger_func_pb_titlechange_cb ( void * plentry_gp , void * prevs_gp )
{
  if (aud_drct_get_playing ())
  {
    aosd_pb_titlechange_prevs_t *prevs = (aosd_pb_titlechange_prevs_t *) prevs_gp;
    String pl_entry_filename = aud_drct_get_filename ();
    Tuple pl_entry_tuple = aud_drct_get_tuple ();
    String pl_entry_title = pl_entry_tuple.get_str (Tuple::FormattedTitle);

    /* same filename but title changed, useful to detect http stream song changes */

    if (prevs->title && prevs->filename)
    {
      if (pl_entry_filename && ! strcmp (pl_entry_filename, prevs->filename))
      {
        if (pl_entry_title && strcmp (pl_entry_title, prevs->title))
        {
          char * text = aosd_trigger_format_text (pl_entry_tuple, global_config.text.format);
          char * markup = g_markup_printf_escaped
           ("<span font_desc='%s'>%s</span>",
           (const char *) global_config.text.fonts_name[0], text);
          g_free (text);

          aosd_osd_display (markup, & global_config, false);
          g_free (markup);

          prevs->title = pl_entry_title;
        }
      }
      else
      {
        prevs->filename = pl_entry_filename;
        /* if filename changes, reset title as well */
        prevs->title = pl_entry_title;
      }
    }
    else
    {
      prevs->title = pl_entry_title;
      prevs->filename = pl_entry_filename;
    }
  }
}

static void
aosd_trigger_func_pb_pauseon_onoff ( bool turn_on )
{
  if ( turn_on == true )
    hook_associate( "playback pause" , aosd_trigger_func_pb_pauseon_cb , nullptr );
  else
    hook_dissociate( "playback pause" , aosd_trigger_func_pb_pauseon_cb );
}

static void
aosd_trigger_func_pb_pauseon_cb ( void * unused1 , void * unused2 )
{
  char * markup = g_markup_printf_escaped ("<span font_desc='%s'>Paused</span>",
   (const char *) global_config.text.fonts_name[0]);
  aosd_osd_display (markup, & global_config, false);
  g_free (markup);
}


static void
aosd_trigger_func_pb_pauseoff_onoff ( bool turn_on )
{
  if ( turn_on == true )
    hook_associate( "playback unpause" , aosd_trigger_func_pb_pauseoff_cb , nullptr );
  else
    hook_dissociate( "playback unpause" , aosd_trigger_func_pb_pauseoff_cb );
}

static void
aosd_trigger_func_pb_pauseoff_cb ( void * unused1 , void * unused2 )
{
  Tuple tuple = aud_drct_get_tuple ();
  int time_cur, time_tot;
  int time_cur_m, time_cur_s, time_tot_m, time_tot_s;

  time_tot = tuple.get_int (Tuple::Length) / 1000;
  time_cur = aud_drct_get_time() / 1000;
  time_cur_s = time_cur % 60;
  time_cur_m = (time_cur - time_cur_s) / 60;
  time_tot_s = time_tot % 60;
  time_tot_m = (time_tot - time_tot_s) / 60;

  char * text = aosd_trigger_format_text (tuple, global_config.text.format);
  char * markup = g_markup_printf_escaped
   ("<span font_desc='%s'>%s (%i:%02i/%i:%02i)</span>",
   (const char *) global_config.text.fonts_name[0], text,
   time_cur_m, time_cur_s, time_tot_m, time_tot_s);
  g_free (text);

  aosd_osd_display (markup, & global_config, false);
  g_free (markup);
}


/* Call with hook_call("aosd toggle", param);
   If param != nullptr, display the supplied text in the OSD
   If param == nullptr, display the current playing song */
static void
aosd_trigger_func_hook_cb ( void * markup_text , void * unused )
{
  if ( markup_text != nullptr )
  {
    /* Display text from caller */
    aosd_osd_display ((char *) markup_text, & global_config, false);
  } else {
    /* Display currently playing song */
    aosd_trigger_func_pb_start_cb (nullptr, nullptr);
  }
}
