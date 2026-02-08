sources = files(
  'rf-common.c',
  'rf-config.c'
)

headers = files(
  'rf-common.h',
  'rf-config.h'
)

dependencies = []
glib = dependency('glib-2.0', required: true)
gobject = dependency('gobject-2.0', required: true)
gio = dependency('gio-2.0', required: true)
libdrm = dependency('libdrm', required: true)
dependencies += [glib, gobject, gio, libdrm]

include_directories = []
# For `config.h`.
include_directories += include_directories('..')

reframe_common = shared_library(
  meson.project_name() + '-common',
  sources: sources,
  include_directories: include_directories,
  dependencies: dependencies,
  install: true,
  install_dir: moduledir
)
reframe_common_dep = declare_dependency(
  link_with: reframe_common,
  include_directories: include_directories('.')
)
