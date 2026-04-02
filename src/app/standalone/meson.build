executable(
  'unvme-standalone',
  'standalone.c',
  dependencies: [vfn_lib, nvme_lib, unvmed_dep],
  link_with: [ccan_lib],
  include_directories: [libunvmed_inc, ccan_inc],
  install: true,
  install_dir: get_option('bindir'),
)
