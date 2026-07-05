sources = files(
	'rf-common.c',
	'rf-clipboard-rich.c',
	'rf-config.c',
	'rf-display-layout.c',
	'rf-rdp-audio-stream.c',
	'rf-resize.c'
)

headers = files(
	'rf-common.h',
	'rf-clipboard-rich.h',
	'rf-config.h',
	'rf-display-layout.h',
	'rf-rdp-audio-stream.h',
	'rf-resize.h'
)

dependencies = []
glib = dependency('glib-2.0', required: true)
gio = dependency('gio-2.0', required: true)
gobject = dependency('gobject-2.0', required: true)
libdrm = dependency('libdrm', required: true)
dependencies += [glib, gio, gobject, libdrm]

include_directories = []
# For `config.h`.
include_directories += include_directories('..')

reframe_common = library(
  meson.project_name() + '-common',
  sources: sources,
  include_directories: include_directories,
  dependencies: dependencies,
  install: true
)
reframe_common_dep = declare_dependency(
	link_with: reframe_common,
	include_directories: include_directories('.')
)

test_clipboard_rich = executable(
  'test-clipboard-rich',
  sources: files('../tests/test-clipboard-rich.c', 'rf-clipboard-rich.c'),
  dependencies: [glib],
  include_directories: include_directories
)
test('clipboard-rich', test_clipboard_rich)

test_display_layout = executable(
  'test-display-layout',
  sources: files('../tests/test-display-layout.c', 'rf-display-layout.c'),
  dependencies: [glib],
  include_directories: include_directories
)
test('display-layout', test_display_layout)
