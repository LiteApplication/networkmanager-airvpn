plugin_c_args = [
  '-DNETWORKMANAGER_COMPILATION=NM_NETWORKMANAGER_COMPILATION_LIB_BASE',
  '-DLOCALEDIR="@0@"'.format(localedir),
]

editor_c_args = [
  '-DNETWORKMANAGER_COMPILATION=NM_NETWORKMANAGER_COMPILATION_LIB_EDITOR',
  '-DLOCALEDIR="@0@"'.format(localedir),
]

plugin_ver = meson.current_source_dir() / 'libnm-vpn-plugin-airvpn.ver'
editor_ver = meson.current_source_dir() / 'libnm-vpn-plugin-airvpn-editor.ver'

# The service-side editor plugin: no GTK linkage, dispatches to the
# GTK3/GTK4 editor at runtime.
shared_library(
  'nm-vpn-plugin-airvpn',
  sources: files(
    'nm-airvpn-editor-plugin.c',
    '../shared/nm-utils/nm-vpn-plugin-utils.c',
  ),
  include_directories: shared_inc,
  c_args: plugin_c_args,
  link_args: ['-Wl,--version-script=' + plugin_ver],
  link_depends: files('libnm-vpn-plugin-airvpn.ver'),
  dependencies: [glib_dep, gmodule_dep, libnm_dep, dl_dep, config_dep],
  install: true,
  install_dir: plugindir,
)

editor_sources = files(
  'nm-airvpn-editor.c',
  'nm-airvpn-server-list.c',
  '../shared/nm-airvpn-properties.c',
)

editor_deps_common = [glib_dep, gio_dep, libnm_dep, jsonglib_dep, libcurl_dep, config_dep]

gtk3_resources = gnome.compile_resources(
  'resources-gtk3',
  'gresource.xml',
  source_dir: meson.current_source_dir(),
  extra_args: ['--internal'],
)

shared_library(
  'nm-vpn-plugin-airvpn-editor',
  sources: editor_sources + [gtk3_resources],
  include_directories: shared_inc,
  c_args: editor_c_args,
  link_args: ['-Wl,--version-script=' + editor_ver],
  link_depends: files('libnm-vpn-plugin-airvpn-editor.ver'),
  dependencies: editor_deps_common + [gtk3_dep, libnma_dep],
  install: true,
  install_dir: plugindir,
)

if with_gtk4
  gtk4_builddir = meson.current_build_dir() / 'gtk4'

  gtk4_ui = custom_target(
    'nm-airvpn-dialog-gtk4.ui',
    input: 'nm-airvpn-dialog.ui',
    output: 'nm-airvpn-dialog.ui',
    command: [
      find_program('../build-aux/gtk4-convert.sh'),
      find_program('gtk4-builder-tool'),
      '@INPUT@',
      '@OUTPUT@',
    ],
  )

  gtk4_gresource = configure_file(
    input: 'gresource.xml',
    output: 'gresource-gtk4.xml',
    copy: true,
  )

  gtk4_resources = gnome.compile_resources(
    'resources-gtk4',
    gtk4_gresource,
    source_dir: [meson.current_build_dir(), meson.current_source_dir()],
    dependencies: gtk4_ui,
    extra_args: ['--internal'],
  )

  shared_library(
    'nm-gtk4-vpn-plugin-airvpn-editor',
    sources: editor_sources + [gtk4_resources],
    include_directories: shared_inc,
    c_args: editor_c_args,
    link_args: ['-Wl,--version-script=' + editor_ver],
    link_depends: files('libnm-vpn-plugin-airvpn-editor.ver'),
    dependencies: editor_deps_common + [gtk4_dep, libnma_gtk4_dep],
    install: true,
    install_dir: plugindir,
  )
endif
