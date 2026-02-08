sources = files(
  'main.c'
)

dependencies = []
glib = dependency('glib-2.0', required: true)
gio = dependency('gio-2.0', required: true)
gio_unix = dependency('gio-unix-2.0', required: true)
gobject = dependency('gobject-2.0', required: true)
libdrm = dependency('libdrm', required: true)
dependencies += [glib, gio, gio_unix, gobject, libdrm, reframe_common_dep]

if get_option('systemd') and libsystemd.found()
  dependencies += [libsystemd]
endif

include_directories = []
# For `config.h`.
include_directories += include_directories('..')

executable(
  meson.project_name() + '-streamer',
  sources: sources,
  dependencies: dependencies,
  include_directories: include_directories,
  install: true
)
