project(
  'networkmanager-airvpn', 'c',
  version: '0.1.0',
  license: 'GPL-2.0-or-later',
  meson_version: '>= 0.61',
  default_options: [
    'buildtype=debugoptimized',
    'c_std=gnu11',
    'warning_level=1',
  ],
)

gnome = import('gnome')

add_project_arguments('-D_GNU_SOURCE', language: 'c')

prefix = get_option('prefix')
libdir = prefix / get_option('libdir')
libexecdir = prefix / get_option('libexecdir')
datadir = prefix / get_option('datadir')
localedir = prefix / get_option('localedir')
localstatedir = get_option('localstatedir')

plugindir = libdir / 'NetworkManager'
vpnservicedir = libdir / 'NetworkManager' / 'VPN'
dbus_policy_dir = datadir / 'dbus-1' / 'system.d'
statedir = localstatedir / 'lib' / 'nm-airvpn'

with_gnome = get_option('gnome')
with_gtk4 = get_option('gtk4')

glib_dep = dependency('glib-2.0', version: '>= 2.56')
gio_dep = dependency('gio-2.0')
gio_unix_dep = dependency('gio-unix-2.0')
libnm_dep = dependency('libnm', version: '>= 1.20')
libcurl_dep = dependency('libcurl')
gmodule_dep = dependency('gmodule-2.0')
jsonglib_dep = dependency('json-glib-1.0')
dl_dep = meson.get_compiler('c').find_library('dl')

if with_gnome
  gtk3_dep = dependency('gtk+-3.0', version: '>= 3.24')
  libnma_dep = dependency('libnma', version: '>= 1.8.0')
  libsecret_dep = dependency('libsecret-1')
endif

if with_gtk4
  gtk4_dep = dependency('gtk4', version: '>= 4.0')
  libnma_gtk4_dep = dependency('libnma-gtk4')
endif

config_h = configuration_data()
config_h.set_quoted('VERSION', meson.project_version())
config_h.set_quoted('GETTEXT_PACKAGE', meson.project_name())
config_h.set_quoted('NM_AIRVPN_LOCALEDIR', localedir)
config_h.set_quoted('NM_AIRVPN_STATEDIR', statedir)
config_h.set_quoted('LIBEXECDIR', libexecdir)
config_h.set_quoted('PLUGINDIR', plugindir)
configure_file(output: 'config.h', configuration: config_h)

config_dep = declare_dependency(include_directories: include_directories('.'))

name_conf = configuration_data()
name_conf.set('LIBEXECDIR', libexecdir)
name_conf.set('PLUGINDIR', plugindir)
configure_file(
  input: 'nm-airvpn-service.name.in',
  output: 'nm-airvpn-service.name',
  configuration: name_conf,
  install_dir: vpnservicedir,
)

install_data('nm-airvpn-service.conf', install_dir: dbus_policy_dir)
install_data('nm-airvpn-sysusers.conf',
             install_dir: libdir / 'sysusers.d',
             rename: 'nm-airvpn.conf')
install_data('nm-airvpn-tmpfiles.conf',
             install_dir: libdir / 'tmpfiles.d',
             rename: 'nm-airvpn.conf')

shared_inc = include_directories('shared')

subdir('shared')
subdir('src')
if with_gnome
  subdir('properties')
  subdir('auth-dialog')
  subdir('appdata')
endif
