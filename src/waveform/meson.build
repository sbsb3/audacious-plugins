# This plugin is GTK-only (its whole reason for existing is the dockable
# waveform widget -- see waveform.cc's doc comment on why there's no Qt
# variant yet) and needs FFmpeg (to decode files into peak data -- see
# decode.h) and sqlite3 (to cache the result -- see cache.h). src/meson.build
# only descends into this directory when USE_GTK is set; still check FFmpeg
# and sqlite3 here and skip building (rather than hard-failing the configure)
# if either is unavailable, same as src/ffaudio does for its own FFmpeg
# dependency.

waveform_libavcodec_dep = dependency('libavcodec', required: false)
waveform_libavformat_dep = dependency('libavformat', required: false)
waveform_libavutil_dep = dependency('libavutil', required: false)
waveform_sqlite3_dep = dependency('sqlite3', required: false)

have_waveform_deps = waveform_libavcodec_dep.found() and \
                     waveform_libavformat_dep.found() and \
                     waveform_libavutil_dep.found() and \
                     waveform_sqlite3_dep.found()

if have_waveform_deps
  shared_module('waveform',
    ['waveform.cc', 'render.cc', 'ruler.cc', 'cache.cc', 'decode.cc',
     'skiptag.cc', 'tagstore.cc'],
    dependencies: [audacious_dep, glib_dep, gtk_dep, audgui_dep,
                   waveform_libavcodec_dep, waveform_libavformat_dep,
                   waveform_libavutil_dep, waveform_sqlite3_dep],
    name_prefix: '',
    install: true,
    install_dir: general_plugin_dir
  )
else
  warning('Skipping waveform plugin: FFmpeg and/or sqlite3 not found')
endif
