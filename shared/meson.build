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
    # nm-utils/nm-shared-utils.c is vendored verbatim from upstream
    # NetworkManager; suppress its g_qsort_with_data deprecation warnings
    # here rather than patching the vendored copy out of sync with upstream.
    '-Wno-deprecated-declarations',
  ],
  dependencies: [glib_dep, libnm_dep, config_dep],
)
