# The inhibitor lock arrives as a unix file descriptor over D-Bus, so this
# needs gio-unix on top of plain gio.
gio_unix_dep = dependency('gio-unix-2.0', version: glib_req, required: true)

have_sleep_inhibitor = true

shared_module('sleep-inhibitor',
  'sleep-inhibitor.cc',
  dependencies: [audacious_dep, glib_dep, gio_dep, gio_unix_dep],
  name_prefix: '',
  install: true,
  install_dir: general_plugin_dir
)
