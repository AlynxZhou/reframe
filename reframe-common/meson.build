sources = files(
  'rf-common.c',
  'rf-buffer.c',
  'rf-config.c'
)

headers = files(
  'rf-common.h',
  'rf-buffer.h',
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

# Linker ignores `RPATH` with `CAP_SYS_ADMIN`, so debug needs static linking.
if get_option('buildtype') == 'debug'
  reframe_common = static_library(
    meson.project_name() + '-common',
    sources: sources,
    c_args: c_args,
    include_directories: include_directories,
    dependencies: dependencies,
    # Do not install static libray, we already linked against it.
    install: false
  )
else
  reframe_common = shared_library(
    meson.project_name() + '-common',
    sources: sources,
    c_args: c_args,
    include_directories: include_directories,
    dependencies: dependencies,
    install: true
  )
endif
reframe_common_dep = declare_dependency(
  link_with: reframe_common,
  include_directories: include_directories('.')
)
