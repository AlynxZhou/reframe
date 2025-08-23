# gnome = import('gnome')
sources = files(
  'main.c',
  'rf-app.c',
  'rf-win.c',
  'rf-streamer.c',
  'rf-converter.c',
  'rf-vnc-server.c'
)

headers = files(
  'rf-app.h',
  'rf-win.h',
  'rf-streamer.h',
  'rf-converter.h',
  'rf-vnc-server.h'
)

dependencies = []
glib = dependency('glib-2.0', required: true)
gio = dependency('gio-2.0', required: true)
gio_unix = dependency('gio-unix-2.0', required: true)
epoxy = dependency('epoxy', required: true)
libvncserver = dependency('libvncserver', required: true)
xkbcommon = dependency('xkbcommon', required: true)
dependencies += [glib, gio, gio_unix, epoxy, libvncserver, xkbcommon]

cc = meson.get_compiler('c')
m = cc.find_library('m', required : false)
if m.found()
  dependencies += [m]
endif

sources += files('rf-win.c')
gtk = dependency('gtk4', required: true)
cairo = dependency('cairo', required: true)
dependencies += [gtk, cairo]

dependencies += [reframe_common_dep]

include_directories = []
# For `config.h`.
include_directories += include_directories('..')

executable(
  'reframe-server',
  sources: sources,
  c_args: c_args,
  dependencies: dependencies,
  include_directories: include_directories,
  install: true
)
