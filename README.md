# networkmanager-airvpn

A NetworkManager VPN plugin for [AirVPN](https://airvpn.org/), in the
spirit of `networkmanager-openvpn`. Instead of importing generated
`.ovpn` files by hand, you store your AirVPN **API key** and pick a
server; the plugin fetches ready-to-use OpenVPN profiles from the
AirVPN configuration generator and drives the `openvpn` binary.

## Features

- Native AirVPN connection type in GNOME Settings (GTK4),
  nm-connection-editor (GTK3), nm-applet and `nmcli`.
- Server picker fed by the AirVPN status API (continents, countries and
  individual servers with their current load), with a Refresh button.
- Profiles are fetched once and cached under `/var/lib/nm-airvpn/`
  (root-only, `0600`); the cache is refreshed automatically on the
  first connect, when the connection parameters change, when a
  connection attempt fails, or after 90 days.
- API key stored as a NetworkManager secret (GNOME keyring by
  default); GNOME Shell prompts for it via the standard VPN secret
  dialog when it is not saved.
- OpenVPN runs with dropped privileges (dedicated `nm-airvpn`
  system user) once the tunnel is up.

## Getting your API key

1. Sign in at [airvpn.org/apisettings](https://airvpn.org/apisettings/).
2. Enable API access for your account and copy the key.
3. Paste it in the connection editor (or answer the GNOME Shell
   prompt on first connect).

The optional *device name* corresponds to an AirVPN "device"
(Client Area → Devices); each device gets its own keypair. Leave it
empty to use your account's default device. In the editor, press
**Load devices** to pick one of your account's devices from the
dropdown (requires the API key entered above).

The device must exist on your account.

## Usage

GUI: GNOME Settings → Network → VPN → **+** → **AirVPN**, or
`nm-connection-editor`.

CLI:

```sh
nmcli connection add type vpn ifname -- vpn-type airvpn con-name my-airvpn \
    vpn.data "server=ch,protocol=udp,port=443,device=mydevice"
nmcli connection modify my-airvpn vpn.secrets api-key=YOUR_API_KEY
nmcli connection up my-airvpn
```

`server` accepts an AirVPN server name (`Achernar`), a country code
(`ch`), a continent (`europe`) or `earth`.

## Building

```sh
meson setup build --prefix /usr -Dlibexecdir=lib -Dlocalstatedir=/var
meson compile -C build
sudo meson install -C build
sudo systemd-sysusers && sudo systemd-tmpfiles --create
```

Dependencies: `libnm`, `glib2`, `libcurl`, `openvpn`; for the GUI:
`gtk3`, `gtk4`, `libnma`, `libnma-gtk4`, `libsecret`, `json-glib`.
Build with `-Dgnome=false` for a headless (nmcli-only) install.

On Arch, use the provided `PKGBUILD` (`makepkg -si`).

## Meson options

| Option   | Default | Description                                   |
|----------|---------|-----------------------------------------------|
| `gnome`  | `true`  | Build the GTK editors and the auth dialog     |
| `gtk4`   | `true`  | Build the GTK4 editor (GNOME Settings needs it) |

## Cache maintenance

Profiles (including the device private key) live in
`/var/lib/nm-airvpn/<connection-uuid>/`. Deleting a connection does not
remove its cache; clean up manually with
`sudo rm -rf /var/lib/nm-airvpn/<uuid>` if needed.

## License

GPL-2.0-or-later. Portions derived from NetworkManager-openvpn and
NetworkManager-fortisslvpn (Red Hat, Inc. and contributors).
