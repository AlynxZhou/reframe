sources = files(
  'main.c',
  'rf-streamer.c',
  'rf-converter.c',
  'rf-vnc-server.c',
  'rf-lvnc-server.c'
)

headers = files(
  'rf-streamer.h',
  'rf-converter.h',
  'rf-vnc-server.h',
  'rf-lvnc-server.h'
)

dependencies = []
cc = meson.get_compiler('c')
m = cc.find_library('m')
glib = dependency('glib-2.0', required: true)
gio = dependency('gio-2.0', required: true)
gio_unix = dependency('gio-unix-2.0', required: true)
epoxy = dependency('epoxy', required: true)
libvncserver = dependency('libvncserver', required: true)
xkbcommon = dependency('xkbcommon', required: true)
dependencies += [m, glib, gio, gio_unix, epoxy, libvncserver, xkbcommon]

dependencies += [mvmath_dep, reframe_common_dep]

include_directories = []
# For `config.h`.
include_directories += include_directories('..')

executable(
  meson.project_name() + '-server',
  sources: sources,
  c_args: c_args,
  dependencies: dependencies,
  include_directories: include_directories,
  install: true
)
