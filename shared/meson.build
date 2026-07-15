shared_sources = files(
  'nm-airvpn-properties.c',
  'nm-utils/nm-shared-utils.c',
)

libnm_airvpn_shared = static_library(
  'nm-airvpn-shared',
  sources: shared_sources,
  include_directories: include_directories('.'),
  c_args: [
    '-DNETWORKMANAGER_COMPILATION=NM_NETWORKMANAGER_COMPILATION_LIB_BASE',
  ],
  dependencies: [glib_dep, libnm_dep, config_dep],
)
