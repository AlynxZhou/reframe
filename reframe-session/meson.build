sources = files(
  'main.c',
  'rf-clipboard-text.c'
)

dependencies = []
glib = dependency('glib-2.0', required: true)
gio = dependency('gio-2.0', required: true)
gio_unix = dependency('gio-unix-2.0', required: true)
gobject = dependency('gobject-2.0', required: true)
gtk = dependency('gtk4', required: true)
dependencies += [glib, gio, gio_unix, gobject, gtk, reframe_common_dep]

include_directories = []
# For `config.h`.
include_directories += include_directories('..')

executable(
  meson.project_name() + '-session',
  sources: sources,
  dependencies: dependencies,
  include_directories: include_directories,
  install: true
)

test_clipboard_text = executable(
  'test-clipboard-text',
  '../tests/test-clipboard-text.c',
  'rf-clipboard-text.c',
  dependencies: [glib],
  include_directories: include_directories
)
test('clipboard-text', test_clipboard_text)
