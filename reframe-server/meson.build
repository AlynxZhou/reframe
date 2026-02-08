# Main executable sources (no longer includes VNC server implementations)
sources = files(
  'main.c',
  'rf-streamer.c',
  'rf-session.c',
  'rf-converter.c',
  'rf-vnc-server.c'
)

headers = files(
  'rf-streamer.h',
  'rf-session.h',
  'rf-converter.h',
  'rf-vnc-server.h'
)

dependencies = []
cc = meson.get_compiler('c')
m = cc.find_library('m')
glib = dependency('glib-2.0', required: true)
gio = dependency('gio-2.0', required: true)
gio_unix = dependency('gio-unix-2.0', required: true)
gmodule = dependency('gmodule-2.0', required: true)
epoxy = dependency('epoxy', required: true)
xkbcommon = dependency('xkbcommon', required: true)
dependencies += [m, glib, gio, gio_unix, gmodule, epoxy, xkbcommon]

dependencies += [mvmath_dep, reframe_common_dep]

include_directories = []
# For `config.h`.
include_directories += include_directories('..')

executable(
  meson.project_name() + '-server',
  sources: sources,
  dependencies: dependencies,
  include_directories: include_directories,
  install: true
)

vnc_plugin_dir = get_option('libdir') / 'reframe' / 'vnc'
libvncserver = dependency('libvncserver', required: true)

shared_module(
  'rf-lvnc-server',
  sources: files('rf-lvnc-server.c'),
  dependencies: [glib, gio, libvncserver, reframe_common_dep],
  include_directories: include_directories,
  name_prefix: '',
  install: true,
  install_dir: vnc_plugin_dir
)

if get_option('neatvnc') and neatvnc.found()
  libdrm = dependency('libdrm', required: true)
  aml = dependency('aml', required: true)
  pixman = dependency('pixman-1', required: true)

  shared_module(
    'rf-nvnc-server',
    sources: files('rf-nvnc-server.c'),
    dependencies: [glib, gio, libdrm, aml, pixman, neatvnc, reframe_common_dep],
    include_directories: include_directories,
    name_prefix: '',
    install: true,
    install_dir: vnc_plugin_dir
  )
endif
