if get_option('systemd') and systemd.found()
  configure_file(
    input: 'reframe-server@.service.in',
    output: 'reframe-server@.service',
    configuration: {
      'bindir': bindir,
      'confdir': confdir,
      'username': username
    },
    install_dir: systemunitdir
  )
  configure_file(
    input: 'reframe-streamer@.service.in',
    output: 'reframe-streamer@.service',
    configuration: {
      'bindir': bindir,
      'confdir': confdir,
      'username': username
    },
    install_dir: systemunitdir
  )
  configure_file(
    input: 'reframe@.socket.in',
    output: 'reframe@.socket',
    configuration: {
      'username': username
    },
    install_dir: systemunitdir
  )

  configure_file(
    input: 'reframe-sysusers.conf.in',
    output: 'reframe-sysusers.conf',
    configuration: {
      'username': username
    },
    install_dir: sysusersdir
  )
endif

configure_file(
  input: 'reframe-session.desktop.in',
  output: 'reframe-session.desktop',
  configuration: {
    'bindir': bindir
  },
  install_dir: xdgautostartdir
)

install_data(
  'example.conf',
  install_dir: confdir
)
