/* nm-airvpn-service - AirVPN integration with NetworkManager
 *
 * Shared connection-property validation, linked into both the service
 * daemon and the editor plugin so they agree on required keys.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "nm-default.h"

#include "nm-airvpn-properties.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
	const char *name;
	gboolean required;
} ValidProperty;

static const ValidProperty valid_properties[] = {
	{ NM_AIRVPN_KEY_SERVER,            TRUE  },
	{ NM_AIRVPN_KEY_DEVICE,            FALSE },
	{ NM_AIRVPN_KEY_PROTOCOL,          FALSE },
	{ NM_AIRVPN_KEY_PORT,              FALSE },
	{ NM_AIRVPN_KEY_CUSTOM_DIRECTIVES, FALSE },
	{ NM_AIRVPN_KEY_KEEPALIVE,         FALSE },
	{ NM_AIRVPN_KEY_PING_INTERVAL,     FALSE },
	{ NM_AIRVPN_KEY_PING_RESTART,      FALSE },
	{ NM_AIRVPN_KEY_KILL_SWITCH,       FALSE },
	{ NM_AIRVPN_KEY_ALLOW_LAN,         FALSE },
	{ NULL,                            FALSE }
};

static const ValidProperty valid_secrets[] = {
	{ NM_AIRVPN_KEY_API_KEY,           TRUE  },
	{ NULL,                            FALSE }
};

static gboolean
validate_one_property (const ValidProperty *table,
                       const char *key,
                       const char *value,
                       GError **error)
{
	int i;

	for (i = 0; table[i].name; i++) {
		if (!strcmp (table[i].name, key))
			return TRUE;
	}

	g_set_error (error,
	             NM_VPN_PLUGIN_ERROR,
	             NM_VPN_PLUGIN_ERROR_INVALID_CONNECTION,
	             _("invalid or unknown property “%s”"),
	             key);
	return FALSE;
}

typedef struct {
	const ValidProperty *table;
	GError **error;
	gboolean have[16];
} ValidateInfo;

static void
validate_helper (const char *key, const char *value, gpointer user_data)
{
	ValidateInfo *info = user_data;
	int i;

	if (*(info->error))
		return;

	/* Secret flags are stored as "<secret-name>-flags" data items. */
	if (g_str_has_suffix (key, "-flags"))
		return;

	for (i = 0; info->table[i].name; i++) {
		if (!strcmp (info->table[i].name, key))
			info->have[i] = TRUE;
	}

	validate_one_property (info->table, key, value, info->error);
}

static gboolean
validate_properties (const ValidProperty *table,
                     NMSettingVpn *s_vpn,
                     gboolean secrets,
                     GError **error)
{
	GError *validate_error = NULL;
	ValidateInfo info = { table, &validate_error, { FALSE } };
	int i;

	if (secrets)
		nm_setting_vpn_foreach_secret (s_vpn, validate_helper, &info);
	else
		nm_setting_vpn_foreach_data_item (s_vpn, validate_helper, &info);

	if (validate_error) {
		g_propagate_error (error, validate_error);
		return FALSE;
	}

	for (i = 0; table[i].name; i++) {
		if (table[i].required && !info.have[i]) {
			g_set_error (error,
			             NM_VPN_PLUGIN_ERROR,
			             NM_VPN_PLUGIN_ERROR_INVALID_CONNECTION,
			             _("missing required property “%s”"),
			             table[i].name);
			return FALSE;
		}
	}

	return TRUE;
}

gboolean
nm_airvpn_properties_validate (NMSettingVpn *s_vpn, GError **error)
{
	const char *value;

	if (!validate_properties (valid_properties, s_vpn, FALSE, error))
		return FALSE;

	value = nm_setting_vpn_get_data_item (s_vpn, NM_AIRVPN_KEY_PROTOCOL);
	if (   value
	    && strcmp (value, NM_AIRVPN_PROTOCOL_UDP)
	    && strcmp (value, NM_AIRVPN_PROTOCOL_TCP)) {
		g_set_error (error,
		             NM_VPN_PLUGIN_ERROR,
		             NM_VPN_PLUGIN_ERROR_INVALID_CONNECTION,
		             _("property “%s” must be “udp” or “tcp”"),
		             NM_AIRVPN_KEY_PROTOCOL);
		return FALSE;
	}

	value = nm_setting_vpn_get_data_item (s_vpn, NM_AIRVPN_KEY_PORT);
	if (value) {
		long port = strtol (value, NULL, 10);

		if (port <= 0 || port > 65535) {
			g_set_error (error,
			             NM_VPN_PLUGIN_ERROR,
			             NM_VPN_PLUGIN_ERROR_INVALID_CONNECTION,
			             _("property “%s” is not a valid port"),
			             NM_AIRVPN_KEY_PORT);
			return FALSE;
		}
	}

	{
		static const char *bool_properties[] = {
			NM_AIRVPN_KEY_KEEPALIVE,
			NM_AIRVPN_KEY_KILL_SWITCH,
			NM_AIRVPN_KEY_ALLOW_LAN,
		};
		guint i;

		for (i = 0; i < G_N_ELEMENTS (bool_properties); i++) {
			value = nm_setting_vpn_get_data_item (s_vpn, bool_properties[i]);
			if (value && strcmp (value, "yes") && strcmp (value, "no")) {
				g_set_error (error,
				             NM_VPN_PLUGIN_ERROR,
				             NM_VPN_PLUGIN_ERROR_INVALID_CONNECTION,
				             _("property “%s” must be “yes” or “no”"),
				             bool_properties[i]);
				return FALSE;
			}
		}
	}

	{
		static const struct {
			const char *key;
		} seconds_properties[] = {
			{ NM_AIRVPN_KEY_PING_INTERVAL },
			{ NM_AIRVPN_KEY_PING_RESTART },
		};
		guint i;

		for (i = 0; i < G_N_ELEMENTS (seconds_properties); i++) {
			value = nm_setting_vpn_get_data_item (s_vpn, seconds_properties[i].key);
			if (value) {
				long seconds = strtol (value, NULL, 10);

				if (seconds <= 0 || seconds > 3600) {
					g_set_error (error,
					             NM_VPN_PLUGIN_ERROR,
					             NM_VPN_PLUGIN_ERROR_INVALID_CONNECTION,
					             _("property “%s” must be between 1 and 3600 seconds"),
					             seconds_properties[i].key);
					return FALSE;
				}
			}
		}
	}

	return TRUE;
}

gboolean
nm_airvpn_properties_validate_secrets (NMSettingVpn *s_vpn, GError **error)
{
	return validate_properties (valid_secrets, s_vpn, TRUE, error);
}
