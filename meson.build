project(
  'reframe',
  'c',
  version: '0.0.0',
  license: 'Apache-2.0',
  default_options: [
    'c_std=gnu11',
    # Linker will ignore `RPATH` with `CAP_SYS_ADMIN`, so we need static linking
    # to debug.
    get_option('buildtype') == 'debug' ? 'default_library=static' : 'default_library=shared'
  ]
)

c_args = []
if get_option('debug') == true
  warning_level = 3
  c_args += ['-D__DEBUG__']
endif
c_args += ['-DG_LOG_DOMAIN="ReFrame"']

conf_data = configuration_data()
conf_data.set('project_version', meson.project_version())
# conf_data.set('gettext_package', meson.project_name())
# conf_data.set('package_bindir', get_option('prefix') / get_option('bindir'))
# conf_data.set('package_localedir', get_option('prefix') / get_option('localedir'))

configure_file(
  input: 'reframe-common' / 'config.h.in',
  output: 'config.h',
  configuration: conf_data
)

subdir('reframe-common')
subdir('reframe-streamer')
subdir('reframe-server')
