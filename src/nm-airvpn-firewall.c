/* nm-airvpn-service - AirVPN integration with NetworkManager
 *
 * Kill-switch: blocks non-VPN traffic via a dedicated nftables table
 * while the tunnel is expected to be up, so an unexpected drop doesn't
 * silently fall back to the regular route.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "nm-default.h"

#include "nm-airvpn-firewall.h"

#include <string.h>
#include <sys/stat.h>

#include <gio/gio.h>
#include <glib/gstdio.h>

#define NFT_TABLE_NAME  "nm-airvpn-killswitch"
#define RULESET_FILE_NAME  "killswitch.nft"

typedef struct {
	char *host;
	char *port;
	char *proto;
} RemoteEntry;

static void
remote_entry_free (gpointer data)
{
	RemoteEntry *entry = data;

	g_free (entry->host);
	g_free (entry->port);
	g_free (entry->proto);
	g_slice_free (RemoteEntry, entry);
}

gboolean
nm_airvpn_firewall_available (void)
{
	gs_free char *path = g_find_program_in_path ("nft");

	return path != NULL;
}

static GSList *
parse_remotes (const char *config_path, GError **error)
{
	gs_free char *contents = NULL;
	gs_strfreev char **lines = NULL;
	gs_free char *default_proto = NULL;
	GSList *remotes = NULL;
	guint i;

	if (!g_file_get_contents (config_path, &contents, NULL, error))
		return NULL;

	lines = g_strsplit (contents, "\n", -1);

	/* A "proto" directive, when present, applies to every "remote" that
	 * doesn't specify its own protocol as a third field. */
	for (i = 0; lines[i]; i++) {
		gs_strfreev char **tokens = NULL;
		char *line = g_strstrip (lines[i]);

		if (!g_str_has_prefix (line, "proto "))
			continue;
		tokens = g_strsplit_set (line, " \t", -1);
		if (tokens[1] && tokens[1][0]) {
			g_free (default_proto);
			default_proto = g_strdup (tokens[1]);
		}
	}

	for (i = 0; lines[i]; i++) {
		gs_strfreev char **tokens = NULL;
		char *line = g_strstrip (lines[i]);
		RemoteEntry *entry;

		if (!g_str_has_prefix (line, "remote "))
			continue;
		tokens = g_strsplit_set (line, " \t", -1);
		if (!tokens[1] || !tokens[1][0])
			continue;

		entry = g_slice_new0 (RemoteEntry);
		entry->host = g_strdup (tokens[1]);
		entry->port = g_strdup ((tokens[2] && tokens[2][0]) ? tokens[2] : NM_AIRVPN_DEFAULT_PORT);
		entry->proto = g_strdup ((tokens[3] && tokens[3][0]) ? tokens[3]
		                          : default_proto ?: NM_AIRVPN_PROTOCOL_UDP);
		remotes = g_slist_prepend (remotes, entry);
	}

	if (!remotes) {
		g_set_error_literal (error,
		                     G_FILE_ERROR,
		                     G_FILE_ERROR_INVAL,
		                     _("The OpenVPN config has no “remote” directive."));
		return NULL;
	}

	return g_slist_reverse (remotes);
}

/* "udp6"/"tcp6" style suffixes are only meaningful to OpenVPN's own
 * socket setup; nft only cares about the base protocol. */
static const char *
nft_proto (const char *proto)
{
	return g_str_has_prefix (proto, "tcp") ? "tcp" : "udp";
}

/* OpenVPN resolves its own "remote" hostname itself (independently of
 * whatever we resolve below for the allow-list), and the system
 * resolver may not be reachable over loopback (e.g. a VPN's own
 * split-DNS resolver reachable only through a tun interface). Without
 * this, arming the kill switch before openvpn starts blocks its DNS
 * lookup and the connection never comes up. */
static void
append_resolver_rules (GString *ruleset)
{
	gs_free char *contents = NULL;
	gs_strfreev char **lines = NULL;
	guint i;

	if (!g_file_get_contents ("/etc/resolv.conf", &contents, NULL, NULL))
		return;

	lines = g_strsplit (contents, "\n", -1);
	for (i = 0; lines[i]; i++) {
		gs_strfreev char **tokens = NULL;
		char *line = g_strstrip (lines[i]);
		gboolean is_v6;

		if (!g_str_has_prefix (line, "nameserver "))
			continue;
		tokens = g_strsplit_set (line, " \t", -1);
		if (!tokens[1] || !tokens[1][0] || !g_hostname_is_ip_address (tokens[1]))
			continue;

		is_v6 = strchr (tokens[1], ':') != NULL;
		g_string_append_printf (ruleset, "\t\t%s daddr %s udp dport 53 accept\n",
		                        is_v6 ? "ip6" : "ip", tokens[1]);
		g_string_append_printf (ruleset, "\t\t%s daddr %s tcp dport 53 accept\n",
		                        is_v6 ? "ip6" : "ip", tokens[1]);
	}
}

static gboolean
append_remote_rules (GString *ruleset, const RemoteEntry *entry, GError **error)
{
	GResolver *resolver;
	GList *addresses, *l;
	const char *proto = nft_proto (entry->proto);

	if (g_hostname_is_ip_address (entry->host)) {
		gs_unref_object GInetAddress *addr = g_inet_address_new_from_string (entry->host);
		gboolean is_v6 = g_inet_address_get_family (addr) == G_SOCKET_FAMILY_IPV6;

		g_string_append_printf (ruleset, "\t\t%s daddr %s %s dport %s accept\n",
		                        is_v6 ? "ip6" : "ip", entry->host, proto, entry->port);
		return TRUE;
	}

	resolver = g_resolver_get_default ();
	addresses = g_resolver_lookup_by_name (resolver, entry->host, NULL, error);
	g_object_unref (resolver);
	if (!addresses)
		return FALSE;

	for (l = addresses; l; l = l->next) {
		GInetAddress *addr = l->data;
		gs_free char *addr_str = g_inet_address_to_string (addr);
		gboolean is_v6 = g_inet_address_get_family (addr) == G_SOCKET_FAMILY_IPV6;

		g_string_append_printf (ruleset, "\t\t%s daddr %s %s dport %s accept\n",
		                        is_v6 ? "ip6" : "ip", addr_str, proto, entry->port);
	}
	g_resolver_free_addresses (addresses);

	return TRUE;
}

static char *
build_ruleset (const char *config_path, gboolean allow_lan, GError **error)
{
	GString *ruleset;
	GSList *remotes, *l;

	remotes = parse_remotes (config_path, error);
	if (!remotes)
		return NULL;

	ruleset = g_string_new (NULL);
	g_string_append_printf (ruleset,
	                        "table inet %s {\n"
	                        "\tchain output {\n"
	                        "\t\ttype filter hook output priority 0; policy drop;\n"
	                        "\t\toifname \"lo\" accept\n"
	                        "\t\toifname \"tun*\" accept\n"
	                        "\t\tct state established,related accept\n",
	                        NFT_TABLE_NAME);

	append_resolver_rules (ruleset);

	for (l = remotes; l; l = l->next) {
		if (!append_remote_rules (ruleset, l->data, error)) {
			g_slist_free_full (remotes, remote_entry_free);
			g_string_free (ruleset, TRUE);
			return NULL;
		}
	}
	g_slist_free_full (remotes, remote_entry_free);

	if (allow_lan) {
		g_string_append (ruleset,
		                  "\t\tip daddr { 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16 } accept\n"
		                  "\t\tip6 daddr { fe80::/10, fc00::/7 } accept\n");
	}

	g_string_append (ruleset, "\t}\n}\n");

	return g_string_free (ruleset, FALSE);
}

static gboolean
run_nft (char **argv, GError **error)
{
	gs_free char *stdout_buf = NULL;
	gs_free char *stderr_buf = NULL;
	int exit_status;

	if (!g_spawn_sync (NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
	                   NULL, NULL, &stdout_buf, &stderr_buf, &exit_status, error))
		return FALSE;

	if (!g_spawn_check_wait_status (exit_status, error)) {
		if (stderr_buf && stderr_buf[0]) {
			g_clear_error (error);
			g_set_error (error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED, "%s", stderr_buf);
		}
		return FALSE;
	}

	return TRUE;
}

gboolean
nm_airvpn_firewall_arm (const char *config_path, gboolean allow_lan, GError **error)
{
	gs_free char *ruleset = NULL;
	gs_free char *ruleset_path = NULL;
	mode_t old_umask;
	gboolean wrote;
	char *argv[] = { "nft", "-f", NULL, NULL };

	if (!nm_airvpn_firewall_available ()) {
		g_set_error_literal (error,
		                     G_SPAWN_ERROR,
		                     G_SPAWN_ERROR_NOENT,
		                     _("The “nft” binary is not installed; cannot enforce the kill switch."));
		return FALSE;
	}

	ruleset = build_ruleset (config_path, allow_lan, error);
	if (!ruleset)
		return FALSE;

	ruleset_path = g_build_filename (NM_AIRVPN_STATEDIR, RULESET_FILE_NAME, NULL);

	old_umask = umask (0077);
	wrote = g_file_set_contents (ruleset_path, ruleset, -1, error);
	umask (old_umask);
	if (!wrote)
		return FALSE;

	argv[2] = ruleset_path;
	if (!run_nft (argv, error)) {
		g_unlink (ruleset_path);
		return FALSE;
	}

	g_unlink (ruleset_path);
	return TRUE;
}

void
nm_airvpn_firewall_disarm (void)
{
	char *argv[] = { "nft", "delete", "table", "inet", NFT_TABLE_NAME, NULL };

	if (!nm_airvpn_firewall_available ())
		return;

	/* Best-effort: the table may not exist (never armed, or already
	 * torn down), which nft reports as a non-zero exit we don't care
	 * about here. */
	run_nft (argv, NULL);
}
