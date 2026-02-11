project(
  'reframe',
  'c',
  version: '1.11.0',
  license: 'Apache-2.0',
  default_options: ['c_std=gnu11']
)

if get_option('debug') == true
  warning_level = 3
  add_project_arguments('-D__DEBUG__', language: ['c'])
endif
add_project_arguments('-DG_LOG_DOMAIN="ReFrame"', language: ['c'])

if get_option('systemd')
  systemd = dependency('systemd', required: false)
  libsystemd = dependency('libsystemd', required: false)
endif
if get_option('neatvnc')
  neatvnc = dependency('neatvnc', required: false)
endif

prefix = get_option('prefix')
bindir = prefix / get_option('bindir')
libdir = prefix / get_option('libdir')
moduledir = libdir / meson.project_name()
sysconfdir = get_option('sysconfdir')
confdir = get_option('confdir')
if confdir == ''
  confdir = sysconfdir / meson.project_name()
endif
username = get_option('username')
xdgautostartdir = get_option('xdgautostartdir')
if xdgautostartdir == ''
  xdgautostartdir = sysconfdir / 'xdg' / 'autostart'
endif

systemunitdir = get_option('systemunitdir')
sysusersdir = get_option('sysusersdir')
tmpfilesdir = get_option('tmpfilesdir')
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

  if tmpfilesdir == ''
    tmpfilesdir = systemd.get_variable(pkgconfig: 'tmpfilesdir')
  endif
  if tmpfilesdir == ''
    error('Couldn\'t determine systemd tmpfiles directory.')
  endif
endif

conf_data = configuration_data()
conf_data.set_quoted('PROJECT_NAME', meson.project_name())
conf_data.set_quoted('PROJECT_VERSION', meson.project_version())
conf_data.set_quoted('USERNAME', username)
conf_data.set_quoted('BINDIR', bindir)
conf_data.set_quoted('LIBDIR', libdir)
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
subdir('reframe-session')
subdir('reframe-streamer')
subdir('reframe-server')

summary({
  'buildtype': get_option('buildtype'),
  'prefix': prefix,
  'bindir': bindir,
  'libdir': libdir,
  'moduledir': moduledir,
  'confdir': confdir,
}, section: 'Confiuration')
if systemd.found()
  summary({
    'systemunitdir': systemunitdir,
    'sysusersdir': sysusersdir,
    'tmpfilesdir': tmpfilesdir,
    'username': username
  }, section: 'Confiuration')
endif

meson.add_install_script(
  'meson_post_install.sh',
  skip_if_destdir: true
)
