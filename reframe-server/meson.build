# Main executable sources (no longer includes VNC server implementations)
sources = files(
  'main.c',
  'rf-streamer.c',
  'rf-session.c',
  'rf-converter.c',
  'rf-remote-server.c',
  'rf-vnc-server.c'
)

headers = files(
  'rf-streamer.h',
  'rf-session.h',
  'rf-converter.h',
  'rf-remote-server.h',
  'rf-vnc-server.h'
)

dependencies = []
cc = meson.get_compiler('c')
m = cc.find_library('m')
glib = dependency('glib-2.0', required: true)
gio = dependency('gio-2.0', required: true)
gio_unix = dependency('gio-unix-2.0', required: true)
gmodule = dependency('gmodule-2.0', required: true)
gobject = dependency('gobject-2.0', required: true)
epoxy = dependency('epoxy', required: true)
xkbcommon = dependency('xkbcommon', required: true)
dependencies += [
  m,
  glib,
  gio,
  gio_unix,
  gmodule,
  gobject,
  epoxy,
  xkbcommon,
  mvmath_dep,
  reframe_common_dep
]

include_directories = []
# For `config.h`.
include_directories += include_directories('..')

executable(
  meson.project_name() + '-server',
  sources: sources,
  dependencies: dependencies,
  include_directories: include_directories,
  install: true
)

lvnc_sources = files('rf-lvnc-server.c')

lvnc_headers = files('rf-lvnc-server.h')

lvnc_dependencies = []
libvncserver = dependency('libvncserver', required: true)
lvnc_dependencies += [
  glib,
  gio,
  gmodule,
  gobject,
  libvncserver,
  reframe_common_dep
]

shared_module(
  meson.project_name() + '-libvncserver',
  sources: lvnc_sources,
  dependencies: lvnc_dependencies,
  include_directories: include_directories,
  install: true,
  install_dir: moduledir / 'vnc'
)

if get_option('rdp')
  avcodec = dependency('libavcodec', required: false)
  avutil = dependency('libavutil', required: false)
  swscale = dependency('libswscale', required: false)
  gdk_pixbuf = dependency('gdk-pixbuf-2.0', required: true)
  libpng = dependency('libpng', required: true)
  libtiff = dependency('libtiff-4', required: true)
  rdp_avc_c_args = []
  rdp_avc_dependencies = []
  if avcodec.found() and avutil.found() and swscale.found()
    rdp_avc_c_args += ['-DRF_HAVE_RDP_AVC=1']
    rdp_avc_dependencies += [avcodec, avutil, swscale]
  endif
  rdp_cliprdr_c_args = []
  rdp_cliprdr_dependencies = []
  rdp_cliprdr_c_args += ['-DRF_HAVE_RDP_CLIPRDR_LIBPNG=1']
  rdp_cliprdr_dependencies += [libpng]
  rdp_cliprdr_c_args += ['-DRF_HAVE_RDP_CLIPRDR_PNG=1']
  rdp_cliprdr_dependencies += [gdk_pixbuf]
  rdp_cliprdr_c_args += ['-DRF_HAVE_RDP_CLIPRDR_TIFF=1']
  rdp_cliprdr_dependencies += [libtiff]

  rdp_sources = files(
    'rf-rdp-server.c',
	    'rf-rdp-proto.c',
    'rf-rdp-mcs.c',
    'rf-rdp-core.c',
    'rf-rdp-input.c',
    'rf-rdp-nsc.c',
    'rf-rdp-dvc.c',
    'rf-rdp-cliprdr.c',
    'rf-rdp-rdpsnd.c',
    'rf-rdp-gfx.c',
    'rf-rdp-rfx.c',
    'rf-rdp-planar.c',
    'rf-rdp-avc.c',
    'rf-rdp-av1.c'
	  )

	  rdp_headers = files(
    'rf-rdp-server.h',
    'rf-rdp-proto.h',
    'rf-rdp-mcs.h',
    'rf-rdp-core.h',
    'rf-rdp-input.h',
    'rf-rdp-nsc.h',
    'rf-rdp-dvc.h',
    'rf-rdp-cliprdr.h',
    'rf-rdp-rdpsnd.h',
    'rf-rdp-gfx.h',
    'rf-rdp-rfx.h',
    'rf-rdp-planar.h',
    'rf-rdp-avc.h',
    'rf-rdp-av1.h'
	  )

  rdp_dependencies = []
  rdp_dependencies += [
    glib,
    gio,
    gio_unix,
    gmodule,
    gobject,
    reframe_common_dep
  ]
  rdp_dependencies += rdp_avc_dependencies
  rdp_dependencies += rdp_cliprdr_dependencies

  shared_module(
    meson.project_name() + '-rdp',
    sources: rdp_sources,
    dependencies: rdp_dependencies,
    include_directories: include_directories,
    c_args: rdp_avc_c_args + rdp_cliprdr_c_args,
    install: true,
    install_dir: moduledir / 'rdp'
  )

  rdp_test_include_directories = []
  rdp_test_include_directories += include_directories
  rdp_test_include_directories += include_directories('.')

  test_rdp_proto = executable(
    'test-rdp-proto',
    sources: files('../tests/test-rdp-proto.c', 'rf-rdp-proto.c'),
    include_directories: rdp_test_include_directories,
    install: false
  )
  test('rdp-proto', test_rdp_proto)

  test_rdp_input = executable(
    'test-rdp-input',
    sources: files('../tests/test-rdp-input.c', 'rf-rdp-input.c'),
    dependencies: [m],
    include_directories: rdp_test_include_directories,
    install: false
  )
  test('rdp-input', test_rdp_input)

  test_rdp_mcs = executable(
    'test-rdp-mcs',
    sources: files(
      '../tests/test-rdp-mcs.c',
      'rf-rdp-mcs.c',
      'rf-rdp-proto.c'
    ),
    include_directories: rdp_test_include_directories,
    install: false
  )
  test('rdp-mcs', test_rdp_mcs)

  test_rdp_core = executable(
    'test-rdp-core',
    sources: files(
      '../tests/test-rdp-core.c',
      'rf-rdp-core.c',
      'rf-rdp-mcs.c',
      'rf-rdp-proto.c'
    ),
    include_directories: rdp_test_include_directories,
    install: false
  )
	  test('rdp-core', test_rdp_core)

  test_rdp_dvc = executable(
    'test-rdp-dvc',
    sources: files(
      '../tests/test-rdp-dvc.c',
      'rf-rdp-dvc.c'
    ),
    include_directories: rdp_test_include_directories,
    install: false
  )
  test('rdp-dvc', test_rdp_dvc)

  test_rdp_cliprdr = executable(
    'test-rdp-cliprdr',
    sources: files(
      '../tests/test-rdp-cliprdr.c',
      'rf-rdp-cliprdr.c'
    ),
    dependencies: [glib] + rdp_cliprdr_dependencies,
    include_directories: rdp_test_include_directories,
    c_args: rdp_cliprdr_c_args,
    install: false
  )
  test('rdp-cliprdr', test_rdp_cliprdr)

  test_rdp_cliprdr_tiff = executable(
    'test-rdp-cliprdr-tiff',
    sources: files(
      '../tests/test-rdp-cliprdr.c',
      'rf-rdp-cliprdr.c'
    ),
    dependencies: [glib, libtiff],
    include_directories: rdp_test_include_directories,
    c_args: ['-DRF_HAVE_RDP_CLIPRDR_TIFF=1'],
    install: false
  )
  test('rdp-cliprdr-tiff', test_rdp_cliprdr_tiff)

  test_rdp_rdpsnd = executable(
    'test-rdp-rdpsnd',
    sources: files(
      '../tests/test-rdp-rdpsnd.c',
      'rf-rdp-rdpsnd.c'
    ),
    dependencies: [glib],
    include_directories: rdp_test_include_directories,
    install: false
  )
  test('rdp-rdpsnd', test_rdp_rdpsnd)

  test_rdp_gfx = executable(
    'test-rdp-gfx',
    sources: files(
      '../tests/test-rdp-gfx.c',
      'rf-rdp-gfx.c',
      'rf-rdp-planar.c'
    ),
    include_directories: rdp_test_include_directories,
    install: false
  )
  test('rdp-gfx', test_rdp_gfx)

  test_rdp_rfx = executable(
    'test-rdp-rfx',
    sources: files(
      '../tests/test-rdp-rfx.c',
      'rf-rdp-rfx.c'
    ),
    dependencies: [glib],
    include_directories: rdp_test_include_directories,
    install: false
  )
  test('rdp-rfx', test_rdp_rfx)

  if rdp_avc_dependencies.length() == 3
    test_rdp_avc = executable(
      'test-rdp-avc',
      sources: files(
        '../tests/test-rdp-avc.c',
        'rf-rdp-avc.c'
      ),
      dependencies: rdp_avc_dependencies,
      include_directories: rdp_test_include_directories,
      c_args: rdp_avc_c_args,
      install: false
    )
    test('rdp-avc', test_rdp_avc)

    test_rdp_av1 = executable(
      'test-rdp-av1',
      sources: files(
        '../tests/test-rdp-av1.c',
        'rf-rdp-av1.c'
      ),
      dependencies: rdp_avc_dependencies,
      include_directories: rdp_test_include_directories,
      c_args: rdp_avc_c_args,
      install: false
    )
    test('rdp-av1', test_rdp_av1)
  endif

	  test_rdp_nsc = executable(
	    'test-rdp-nsc',
	    sources: files(
	      '../tests/test-rdp-nsc.c',
	      'rf-rdp-nsc.c'
	    ),
	    dependencies: [glib],
	    include_directories: rdp_test_include_directories,
	    install: false
	  )
	  test('rdp-nsc', test_rdp_nsc)

	  test_resize = executable(
	    'test-resize',
    sources: files('../tests/test-resize.c', '../reframe-common/rf-resize.c'),
    include_directories: include_directories('../reframe-common'),
    install: false
  )
  test('resize', test_resize)

  test_config = executable(
    'test-config',
    sources: files('../tests/test-config.c'),
    dependencies: [glib, gio, gobject, reframe_common_dep],
    install: false
  )
  test('config', test_config)
endif

if get_option('neatvnc') and neatvnc.found()
  nvnc_sources = files('rf-nvnc-server.c')

  nvnc_headers = files('rf-nvnc-server.h')

  nvnc_dependencies = []
  libdrm = dependency('libdrm', required: true)
  aml = dependency('aml1', required: false)
  if not aml.found()
    aml = dependency('aml', required: true)
  endif
  aml_unstable_api = aml.version().version_compare('<1.0.0')
  pixman = dependency('pixman-1', required: true)
  nvnc_dependencies += [
    glib,
    gio,
    gmodule,
    gobject,
    libdrm,
    neatvnc,
    aml,
    pixman,
    reframe_common_dep
  ]

  nvnc_c_args = []
  if aml_unstable_api
    nvnc_c_args += ['-DAML_UNSTABLE_API=1']
  endif

  shared_module(
    meson.project_name() + '-neatvnc',
    sources: nvnc_sources,
    dependencies: nvnc_dependencies,
    include_directories: include_directories,
    c_args: nvnc_c_args,
    install: true,
    install_dir: moduledir / 'vnc'
  )
endif
