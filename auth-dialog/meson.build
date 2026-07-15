executable(
  'nm-airvpn-auth-dialog',
  sources: files('main.c'),
  include_directories: shared_inc,
  c_args: [
    '-DNETWORKMANAGER_COMPILATION=NM_NETWORKMANAGER_COMPILATION_LIB_BASE',
  ],
  dependencies: [glib_dep, gio_dep, libnm_dep, gtk3_dep, libnma_dep, libsecret_dep, config_dep],
  install: true,
  install_dir: libexecdir,
)
