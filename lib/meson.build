project('libunvmed', 'c',
        version: '1.0.0',
        license: ['LGPL-2.1-or-later', 'MIT'],
        default_options: ['werror=true']
        )

static = get_option('static')

vfn_lib = dependency('libvfn', version: '>=5.1.0', static: static)
nvme_lib = dependency('libnvme', version: '>=1.8.0', static: static)
jsonc_lib = dependency('json-c', version: '>=0.15', static: static)

subdir('ccan')

libunvmed_inc = include_directories('.')
libunvmed_src = [
  'libunvmed.c',
  'libunvmed-vcq.c',
  'libunvmed-cmds.c',
  'libunvmed-logs.c',
]
libunvmed_hdr = [
  'libunvmed.h',
  'libunvmed-logs.h',
]

ver = meson.project_version()
libunvmed_so = shared_library(
  'unvmed',
  libunvmed_src,
  version: ver,
  soversion: '1',
  dependencies: [vfn_lib, nvme_lib, jsonc_lib],
  link_with: [ccan_lib],
  include_directories: [ccan_inc, libunvmed_inc],
  install: true,
  install_dir: get_option('libdir'),
)

install_headers(libunvmed_hdr)

pkgconfig = import('pkgconfig')
pkgconfig.generate(
  libunvmed_so,
  version: ver,
  description: 'unvme-cli daemon process library',
)
