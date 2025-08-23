sources = files(
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
dependencies += [glib, gobject]

include_directories = []
# For `config.h`.
include_directories += include_directories('..')

reframe_common = library(
  'reframe-common',
  sources: sources,
  c_args: c_args,
  include_directories: include_directories,
  dependencies: dependencies,
  install: true
)

reframe_common_dep = declare_dependency(
  link_with: reframe_common,
  include_directories: include_directories('.')
)
