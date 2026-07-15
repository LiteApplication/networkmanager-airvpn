executable(
  'nm-airvpn-service',
  sources: files(
    'nm-airvpn-service.c',
    'nm-airvpn-api.c',
    'nm-airvpn-cache.c',
  ),
  include_directories: shared_inc,
  link_with: libnm_airvpn_shared,
  dependencies: [glib_dep, gio_dep, gio_unix_dep, libnm_dep, libcurl_dep, jsonglib_dep, config_dep],
  install: true,
  install_dir: libexecdir,
)

executable(
  'nm-airvpn-service-openvpn-helper',
  sources: files(
    'nm-airvpn-service-openvpn-helper.c',
  ),
  include_directories: shared_inc,
  link_with: libnm_airvpn_shared,
  dependencies: [glib_dep, gio_dep, libnm_dep, config_dep],
  install: true,
  install_dir: libexecdir,
)

# Developer test runner for the API/cache modules; not installed.
executable(
  'nm-airvpn-test-fetch',
  sources: files(
    'nm-airvpn-test-fetch.c',
    'nm-airvpn-api.c',
    'nm-airvpn-cache.c',
  ),
  include_directories: shared_inc,
  link_with: libnm_airvpn_shared,
  dependencies: [glib_dep, gio_dep, libnm_dep, libcurl_dep, jsonglib_dep, config_dep],
  install: false,
)
