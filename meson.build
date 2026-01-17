project(
  'reframe',
  'c',
  version: '1.7.0',
  license: 'Apache-2.0',
)

c_args = []
if get_option('debug') == true
  warning_level = 3
  c_args += ['-D__DEBUG__']
endif
c_args += ['-std=gnu11', '-DG_LOG_DOMAIN="ReFrame"']

if get_option('systemd')
  systemd = dependency('systemd', required: false)
  libsystemd = dependency('libsystemd', required: false)
endif
if get_option('neatvnc')
  neatvnc = dependency('neatvnc', required: false)
endif

prefix = get_option('prefix')
bindir = prefix / get_option('bindir')
sysconfdir = get_option('sysconfdir')
confdir = get_option('confdir')
if confdir == ''
  confdir = sysconfdir / 'reframe'
endif
username = get_option('username')

systemunitdir = get_option('systemunitdir')
sysusersdir = get_option('sysusersdir')
if get_option('systemd') and systemd.found()
  if systemunitdir == ''
    systemunitdir = systemd.get_variable(pkgconfig: 'systemdsystemunitdir')
  endif
  if systemunitdir == ''
    error('Couldn\'t determine systemd system unit directory.')
  endif

  if sysusersdir == ''
    sysusersdir = systemd.get_variable(pkgconfig: 'sysusersdir')
  endif
  if sysusersdir == ''
    error('Couldn\'t determine systemd sysusers directory.')
  endif
endif

conf_data = configuration_data()
conf_data.set_quoted('PROJECT_VERSION', meson.project_version())
conf_data.set('HAVE_LIBSYSTEMD', get_option('systemd') and libsystemd.found())
conf_data.set('HAVE_NEATVNC', get_option('neatvnc') and neatvnc.found())

configure_file(
  input: 'reframe-common' / 'config.h.in',
  output: 'config.h',
  configuration: conf_data
)

subdir('deps')
subdir('dists')
subdir('reframe-common')
subdir('reframe-streamer')
subdir('reframe-server')

summary({
  'buildtype': get_option('buildtype'),
  'prefix': prefix,
  'bindir': bindir,
  'confdir': confdir,
}, section: 'Confiuration')
if systemd.found()
  summary({
    'systemunitdir': systemunitdir,
    'sysusersdir': sysusersdir,
    'username': username
  }, section: 'Confiuration')
endif

meson.add_install_script(
  'meson_post_install.sh',
  skip_if_destdir: true
)
