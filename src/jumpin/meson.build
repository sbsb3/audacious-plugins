jumpin_deps = [audacious_dep, glib_dep]

# The core seek/skip logic and the sidecar tag store need only GLib. The
# "Edit Jump In / Skip Tags" dialog needs a toolkit to draw in; for now that
# means GTK only (no Qt variant yet -- see the plan this plugin was ported
# from for why). Building without GTK still gets a working plugin, just
# without a way to set tags short of editing jumpin-tags.conf by hand.
jumpin_sources = ['jumpin.cc', 'skiptag.cc', 'tagstore.cc']

if conf.has('USE_GTK')
  jumpin_deps += [gtk_dep, audgui_dep]
endif

shared_module('jumpin',
  jumpin_sources,
  dependencies: jumpin_deps,
  name_prefix: '',
  install: true,
  install_dir: general_plugin_dir
)
