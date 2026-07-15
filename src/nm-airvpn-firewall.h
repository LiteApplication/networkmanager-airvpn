/* nm-airvpn-service - AirVPN integration with NetworkManager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __NM_AIRVPN_FIREWALL_H__
#define __NM_AIRVPN_FIREWALL_H__

#include <glib.h>

/* Kill-switch: a dedicated nftables table that drops all outbound
 * traffic except loopback, established/related connections, and the
 * VPN server(s) parsed out of the OpenVPN config, so a dropped tunnel
 * can't silently fall back to the regular route. */

/* TRUE if the "nft" binary is available on this system. */
gboolean nm_airvpn_firewall_available (void);

/* Parses "remote"/"proto" directives from the OpenVPN config at
 * @config_path, resolves each remote host, and loads the kill-switch
 * table. If @allow_lan is TRUE, private/link-local destinations are
 * also allowed through. */
gboolean nm_airvpn_firewall_arm (const char *config_path, gboolean allow_lan, GError **error);

/* Removes the kill-switch table if present. Safe to call even when it
 * was never armed. */
void nm_airvpn_firewall_disarm (void);

#endif /* __NM_AIRVPN_FIREWALL_H__ */
