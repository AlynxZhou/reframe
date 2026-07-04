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
rdp_clipboard_sources = files(
  'rdp-clipboard.c',
  'rf-clipboard-text.c',
  'rf-rdp-clipboard-display.c',
  'rf-rdp-clipboard-image.c',
  'rf-rdp-clipboard-provider.c',
  'rf-rdp-clipboard-stream.c',
  'rf-rdp-clipboard-wayland.c'
)
rdp_clipboard_dependencies = dependencies
rdp_clipboard_protocol_sources = []

if have_wayland_clipboard
  wayland_protocols_dir = wayland_protocols.get_variable(pkgconfig: 'pkgdatadir')
  ext_data_control_xml = wayland_protocols_dir / 'staging' / 'ext-data-control' / 'ext-data-control-v1.xml'
  rdp_clipboard_protocol_sources += [
    custom_target(
      'ext-data-control-v1-client-header',
      input: ext_data_control_xml,
      output: 'ext-data-control-v1-client-protocol.h',
      command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@']
    ),
    custom_target(
      'ext-data-control-v1-client-code',
      input: ext_data_control_xml,
      output: 'ext-data-control-v1-protocol.c',
      command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@']
    )
  ]
  rdp_clipboard_dependencies += [wayland_client]
endif

include_directories = []
# For `config.h`.
include_directories += include_directories('..')
include_directories += include_directories('../reframe-common')

executable(
  meson.project_name() + '-session',
  sources: sources,
  dependencies: dependencies,
  include_directories: include_directories,
  install: true
)

executable(
  meson.project_name() + '-rdp-clipboard',
  sources: rdp_clipboard_sources + rdp_clipboard_protocol_sources,
  dependencies: rdp_clipboard_dependencies,
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

test_rdp_clipboard_stream = executable(
  'test-rdp-clipboard-stream',
  '../tests/test-rdp-clipboard-stream.c',
  'rf-rdp-clipboard-stream.c',
  dependencies: [gio],
  include_directories: include_directories
)
test('rdp-clipboard-stream', test_rdp_clipboard_stream)

test_rdp_audio_stream = executable(
  'test-rdp-audio-stream',
  '../tests/test-rdp-audio-stream.c',
  'rf-rdp-audio-stream.c',
  dependencies: [gio, reframe_common_dep],
  include_directories: include_directories
)
test('rdp-audio-stream', test_rdp_audio_stream)

test_rdp_clipboard_provider = executable(
  'test-rdp-clipboard-provider',
  '../tests/test-rdp-clipboard-provider.c',
  'rf-rdp-clipboard-provider.c',
  'rf-rdp-clipboard-image.c',
  '../reframe-common/rf-clipboard-rich.c',
  dependencies: [gtk, glib],
  include_directories: include_directories
)
test('rdp-clipboard-provider', test_rdp_clipboard_provider)

test_rdp_clipboard_display = executable(
  'test-rdp-clipboard-display',
  '../tests/test-rdp-clipboard-display.c',
  'rf-rdp-clipboard-display.c',
  dependencies: [glib],
  include_directories: include_directories
)
test('rdp-clipboard-display', test_rdp_clipboard_display)

test_rdp_clipboard_wayland = executable(
  'test-rdp-clipboard-wayland',
  [
    '../tests/test-rdp-clipboard-wayland.c',
    'rf-rdp-clipboard-wayland.c',
    'rf-rdp-clipboard-image.c',
    'rf-clipboard-text.c',
    'rf-rdp-clipboard-stream.c',
    '../reframe-common/rf-clipboard-rich.c'
  ] + rdp_clipboard_protocol_sources,
  dependencies: [gtk, glib, gio, gio_unix] + (have_wayland_clipboard ? [wayland_client] : []),
  include_directories: include_directories
)
test('rdp-clipboard-wayland', test_rdp_clipboard_wayland)
