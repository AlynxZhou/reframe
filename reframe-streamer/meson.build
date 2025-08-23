sources = files(
  'main.c'
)

dependencies = []
glib = dependency('glib-2.0', required: true)
gio = dependency('gio-2.0', required: true)
gio_unix = dependency('gio-unix-2.0', required: true)
libdrm = dependency('libdrm', required: true)
dependencies += [glib, gio, gio_unix, libdrm]

dependencies += [reframe_common_dep]

include_directories = []
# For `config.h`.
include_directories += include_directories('..')

executable(
  'reframe-streamer',
  sources: sources,
  c_args: c_args,
  dependencies: dependencies,
  include_directories: include_directories,
  install: true
)
