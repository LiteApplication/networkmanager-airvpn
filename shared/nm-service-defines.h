/* nm-airvpn-service - AirVPN integration with NetworkManager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __NM_SERVICE_DEFINES_H__
#define __NM_SERVICE_DEFINES_H__

/* For the NM <-> VPN plugin service */
#define NM_DBUS_SERVICE_AIRVPN    "org.freedesktop.NetworkManager.airvpn"
#define NM_DBUS_INTERFACE_AIRVPN  "org.freedesktop.NetworkManager.airvpn"
#define NM_DBUS_PATH_AIRVPN       "/org/freedesktop/NetworkManager/airvpn"

/* Data items (non-secret) stored in NMSettingVpn */
#define NM_AIRVPN_KEY_SERVER             "server"
#define NM_AIRVPN_KEY_DEVICE             "device"
#define NM_AIRVPN_KEY_PROTOCOL           "protocol"
#define NM_AIRVPN_KEY_PORT               "port"
#define NM_AIRVPN_KEY_CUSTOM_DIRECTIVES  "custom-directives"
#define NM_AIRVPN_KEY_KEEPALIVE          "keepalive"
#define NM_AIRVPN_KEY_PING_INTERVAL      "ping-interval"
#define NM_AIRVPN_KEY_PING_RESTART       "ping-restart"
#define NM_AIRVPN_KEY_KILL_SWITCH        "kill-switch"
#define NM_AIRVPN_KEY_ALLOW_LAN          "allow-lan"

/* Secrets */
#define NM_AIRVPN_KEY_API_KEY            "api-key"

#define NM_AIRVPN_PROTOCOL_UDP           "udp"
#define NM_AIRVPN_PROTOCOL_TCP           "tcp"
#define NM_AIRVPN_DEFAULT_PORT           "443"
#define NM_AIRVPN_DEFAULT_PING_INTERVAL  "10"
#define NM_AIRVPN_DEFAULT_PING_RESTART   "60"

/* Where users obtain their API key */
#define NM_AIRVPN_API_SETTINGS_URL       "https://airvpn.org/apisettings/"

#endif /* __NM_SERVICE_DEFINES_H__ */
