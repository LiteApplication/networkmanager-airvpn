/* nm-airvpn-service - AirVPN integration with NetworkManager
 *
 * Service daemon: fetches OpenVPN configuration from the AirVPN
 * generator API (cache-first) and drives the openvpn binary.
 *
 * Based on nm-fortisslvpn-service.c (C) Red Hat, Inc. / Lubomir Rintel
 * and nm-openvpn-service.c (C) Red Hat, Inc.
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

#include "nm-default.h"

#include "nm-airvpn-service.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <locale.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>

#include <glib/gstdio.h>

#include "nm-airvpn-properties.h"
#include "nm-airvpn-api.h"
#include "nm-airvpn-cache.h"
#include "nm-airvpn-firewall.h"
#include "nm-utils/nm-shared-utils.h"
#include "nm-utils/nm-vpn-plugin-macros.h"

#if !defined(DIST_VERSION)
# define DIST_VERSION VERSION
#endif

#define NM_AIRVPN_HELPER_PATH  LIBEXECDIR "/nm-airvpn-service-openvpn-helper"
#define NM_AIRVPN_USER   "nm-airvpn"
#define NM_AIRVPN_GROUP  "nm-airvpn"

/* Shorter than NetworkManager's own 60 s VPN timeout, so the daemon gets
 * to mark the cache stale before NM tears the connection down. */
#define CONNECT_TIMEOUT_SECONDS 45

static struct {
	gboolean debug;
	int log_level;
} gl/*obal*/;

/*****************************************************************************/

static void nm_airvpn_plugin_initable_iface_init (GInitableIface *iface);

typedef struct {
	GPid pid;
	guint kill_id;
	gboolean interactive;
	gboolean started;
	NMConnection *connection;
	GCancellable *fetch_cancellable;
	guint connect_timer;
	char *uuid;
	gboolean kill_switch_active;
	/* Set when openvpn dies unexpectedly after the tunnel was up. While
	 * set, cleanup_plugin() leaves the kill-switch table in place
	 * instead of tearing it down: NetworkManager calls our Disconnect
	 * vtable almost immediately after a Failure signal (observed
	 * empirically), and disarming there would defeat the kill switch's
	 * purpose of blocking traffic until the user notices or reconnects.
	 * Only a fresh connect attempt (run_openvpn) or a full daemon
	 * restart clears it. */
	gboolean unexpected_drop;
} NMAirvpnPluginPrivate;

G_DEFINE_TYPE_WITH_CODE (NMAirvpnPlugin, nm_airvpn_plugin, NM_TYPE_VPN_SERVICE_PLUGIN,
                         G_ADD_PRIVATE (NMAirvpnPlugin)
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, nm_airvpn_plugin_initable_iface_init));

#define NM_AIRVPN_PLUGIN_GET_PRIVATE(o) ((NMAirvpnPluginPrivate *) nm_airvpn_plugin_get_instance_private ((NMAirvpnPlugin *) (o)))

#define _NMLOG(level, ...) \
    G_STMT_START { \
         if (gl.log_level >= (level)) { \
              g_printerr ("nm-airvpn[%ld] %-7s " _NM_UTILS_MACRO_FIRST (__VA_ARGS__) "\n", \
                         (long) getpid (), \
                          nm_utils_syslog_to_str (level) \
                          _NM_UTILS_MACRO_REST (__VA_ARGS__)); \
         } \
    } G_STMT_END

#define _LOGD(...) _NMLOG(LOG_INFO,    __VA_ARGS__)
#define _LOGI(...) _NMLOG(LOG_NOTICE,  __VA_ARGS__)
#define _LOGW(...) _NMLOG(LOG_WARNING, __VA_ARGS__)

/*****************************************************************************/

static void
get_generator_params (NMSettingVpn *s_vpn, NMAirvpnGeneratorParams *params)
{
	params->server = (char *) nm_setting_vpn_get_data_item (s_vpn, NM_AIRVPN_KEY_SERVER);
	params->device = (char *) nm_setting_vpn_get_data_item (s_vpn, NM_AIRVPN_KEY_DEVICE);
	params->protocol = (char *) nm_setting_vpn_get_data_item (s_vpn, NM_AIRVPN_KEY_PROTOCOL);
	params->port = (char *) nm_setting_vpn_get_data_item (s_vpn, NM_AIRVPN_KEY_PORT);
	params->custom_directives = (char *) nm_setting_vpn_get_data_item (s_vpn, NM_AIRVPN_KEY_CUSTOM_DIRECTIVES);
}

static gboolean
ensure_killed (gpointer data)
{
	int pid = GPOINTER_TO_INT (data);

	if (kill (pid, 0) == 0)
		kill (pid, SIGKILL);

	return FALSE;
}

static void
cleanup_plugin (NMAirvpnPlugin *plugin)
{
	NMAirvpnPluginPrivate *priv = NM_AIRVPN_PLUGIN_GET_PRIVATE (plugin);

	priv->interactive = FALSE;
	priv->started = FALSE;

	nm_clear_g_source (&priv->connect_timer);

	if (priv->fetch_cancellable) {
		g_cancellable_cancel (priv->fetch_cancellable);
		g_clear_object (&priv->fetch_cancellable);
	}

	if (priv->pid) {
		if (kill (priv->pid, SIGTERM) == 0)
			g_timeout_add (2000, ensure_killed, GINT_TO_POINTER (priv->pid));
		else
			kill (priv->pid, SIGKILL);

		_LOGI ("Terminated openvpn with PID %d.", priv->pid);
		priv->pid = 0;
	}

	if (priv->kill_switch_active && !priv->unexpected_drop) {
		nm_airvpn_firewall_disarm ();
		priv->kill_switch_active = FALSE;
	}

	g_clear_object (&priv->connection);
	g_clear_pointer (&priv->uuid, g_free);
}

static void
mark_stale_and_fail (NMAirvpnPlugin *plugin, NMVpnPluginFailure reason)
{
	NMAirvpnPluginPrivate *priv = NM_AIRVPN_PLUGIN_GET_PRIVATE (plugin);

	if (priv->uuid) {
		_LOGI ("Marking cached profile of %s stale; next attempt will refresh it.", priv->uuid);
		nm_airvpn_cache_mark_stale (NM_AIRVPN_STATEDIR, priv->uuid);
	}
	nm_vpn_service_plugin_failure (NM_VPN_SERVICE_PLUGIN (plugin), reason);
}

static void
openvpn_watch_cb (GPid pid, gint status, gpointer user_data)
{
	NMAirvpnPlugin *plugin = NM_AIRVPN_PLUGIN (user_data);
	NMAirvpnPluginPrivate *priv = NM_AIRVPN_PLUGIN_GET_PRIVATE (plugin);
	guint error = 0;
	gboolean signaled = FALSE;

	if (WIFEXITED (status)) {
		error = WEXITSTATUS (status);
		if (error != 0)
			_LOGW ("openvpn exited with error code %u", error);
		else
			_LOGI ("openvpn exited with success");
	} else if (WIFSTOPPED (status)) {
		_LOGW ("openvpn stopped unexpectedly with signal %d", WSTOPSIG (status));
		signaled = TRUE;
	} else if (WIFSIGNALED (status)) {
		_LOGW ("openvpn died with signal %d", WTERMSIG (status));
		signaled = TRUE;
	} else
		_LOGW ("openvpn died from an unknown cause");

	waitpid (priv->pid, NULL, WNOHANG);
	priv->pid = 0;

	nm_clear_g_source (&priv->connect_timer);

	if (error || signaled) {
		if (!priv->started) {
			/* Never reached the connected state: the cached profile may
			 * point at a dead entry IP or hold a revoked key. */
			mark_stale_and_fail (plugin, NM_VPN_PLUGIN_FAILURE_CONNECT_FAILED);
		} else {
			/* Transient drop of an established connection; the profile
			 * itself was fine. */
			priv->unexpected_drop = TRUE;
			nm_vpn_service_plugin_failure (NM_VPN_SERVICE_PLUGIN (plugin),
			                               NM_VPN_PLUGIN_FAILURE_CONNECT_FAILED);
		}
	} else
		nm_vpn_service_plugin_disconnect (NM_VPN_SERVICE_PLUGIN (plugin), NULL);
}

static const char *
nm_find_openvpn (void)
{
	static const char *openvpn_binary_paths[] = {
		"/usr/sbin/openvpn",
		"/usr/bin/openvpn",
		"/usr/local/sbin/openvpn",
		NULL
	};
	const char **openvpn_binary = openvpn_binary_paths;

	while (*openvpn_binary != NULL) {
		if (g_file_test (*openvpn_binary, G_FILE_TEST_EXISTS))
			break;
		openvpn_binary++;
	}

	return *openvpn_binary;
}

static gboolean
connect_timeout_cb (gpointer user_data)
{
	NMAirvpnPlugin *plugin = NM_AIRVPN_PLUGIN (user_data);
	NMAirvpnPluginPrivate *priv = NM_AIRVPN_PLUGIN_GET_PRIVATE (plugin);

	priv->connect_timer = 0;

	if (!priv->started) {
		_LOGW ("Connection attempt timed out after %d seconds.", CONNECT_TIMEOUT_SECONDS);
		if (priv->pid)
			kill (priv->pid, SIGTERM);
		mark_stale_and_fail (plugin, NM_VPN_PLUGIN_FAILURE_CONNECT_FAILED);
	}

	return FALSE;
}

static gboolean
run_openvpn (NMAirvpnPlugin *plugin, GError **error)
{
	NMAirvpnPluginPrivate *priv = NM_AIRVPN_PLUGIN_GET_PRIVATE (plugin);
	GPid pid;
	const char *openvpn;
	NMSettingVpn *s_vpn;
	const char *kill_switch;
	gs_unref_ptrarray GPtrArray *argv = NULL;
	gs_free char *config_path = NULL;
	gs_free char *bus_name = NULL;
	gs_free char *cmd_log = NULL;

	s_vpn = (NMSettingVpn *) nm_connection_get_setting (priv->connection, NM_TYPE_SETTING_VPN);

	openvpn = nm_find_openvpn ();
	if (!openvpn) {
		g_set_error_literal (error,
		                     NM_VPN_PLUGIN_ERROR,
		                     NM_VPN_PLUGIN_ERROR_LAUNCH_FAILED,
		                     _("Could not find the openvpn binary."));
		return FALSE;
	}

	config_path = nm_airvpn_cache_get_config_path (NM_AIRVPN_STATEDIR, priv->uuid);

	argv = g_ptr_array_new_with_free_func (g_free);
	g_ptr_array_add (argv, g_strdup (openvpn));
	g_ptr_array_add (argv, g_strdup ("--config"));
	g_ptr_array_add (argv, g_strdup (config_path));

	{
		const char *protocol = s_vpn ? nm_setting_vpn_get_data_item (s_vpn, NM_AIRVPN_KEY_PROTOCOL) : NULL;
		const char *directives = s_vpn ? nm_setting_vpn_get_data_item (s_vpn, NM_AIRVPN_KEY_CUSTOM_DIRECTIVES) : NULL;
		const char *keepalive = s_vpn ? nm_setting_vpn_get_data_item (s_vpn, NM_AIRVPN_KEY_KEEPALIVE) : NULL;
		const char *ping_interval = s_vpn ? nm_setting_vpn_get_data_item (s_vpn, NM_AIRVPN_KEY_PING_INTERVAL) : NULL;
		const char *ping_restart = s_vpn ? nm_setting_vpn_get_data_item (s_vpn, NM_AIRVPN_KEY_PING_RESTART) : NULL;

		/* Stalled UDP tunnels should self-heal; TCP already gets dead-peer
		 * detection from the transport. Skip if disabled by the user or if
		 * their custom directives already tune ping/keepalive themselves. */
		if (   (!keepalive || strcmp (keepalive, "no"))
		    && (!protocol || !strcmp (protocol, NM_AIRVPN_PROTOCOL_UDP))
		    && !(directives && (strstr (directives, "keepalive") || strstr (directives, "ping")))) {
			g_ptr_array_add (argv, g_strdup ("--keepalive"));
			g_ptr_array_add (argv, g_strdup (ping_interval && ping_interval[0] ? ping_interval : NM_AIRVPN_DEFAULT_PING_INTERVAL));
			g_ptr_array_add (argv, g_strdup (ping_restart && ping_restart[0] ? ping_restart : NM_AIRVPN_DEFAULT_PING_RESTART));
		}
	}

	/* NM applies addresses and routes itself. */
	g_ptr_array_add (argv, g_strdup ("--route-noexec"));
	g_ptr_array_add (argv, g_strdup ("--ifconfig-noexec"));

	/* Up script, called when the connection is established or restarted. */
	g_ptr_array_add (argv, g_strdup ("--script-security"));
	g_ptr_array_add (argv, g_strdup ("2"));
	g_object_get (plugin, NM_VPN_SERVICE_PLUGIN_DBUS_SERVICE_NAME, &bus_name, NULL);
	g_ptr_array_add (argv, g_strdup ("--up"));
	g_ptr_array_add (argv, g_strdup_printf ("%s --debug %d %ld --bus-name %s --tun --",
	                                        NM_AIRVPN_HELPER_PATH,
	                                        gl.log_level,
	                                        (long) getpid (),
	                                        bus_name));
	g_ptr_array_add (argv, g_strdup ("--up-restart"));

	g_ptr_array_add (argv, g_strdup ("--persist-key"));
	g_ptr_array_add (argv, g_strdup ("--persist-tun"));

	/* Drop privileges after the tunnel is up. */
	if (getpwnam (NM_AIRVPN_USER) && getgrnam (NM_AIRVPN_GROUP)) {
		g_ptr_array_add (argv, g_strdup ("--user"));
		g_ptr_array_add (argv, g_strdup (NM_AIRVPN_USER));
		g_ptr_array_add (argv, g_strdup ("--group"));
		g_ptr_array_add (argv, g_strdup (NM_AIRVPN_GROUP));
	} else
		_LOGW ("User/group “%s” not found; openvpn will keep running as root.", NM_AIRVPN_USER);

	g_ptr_array_add (argv, g_strdup ("--syslog"));
	g_ptr_array_add (argv, g_strdup ("nm-airvpn"));

	g_ptr_array_add (argv, NULL);

	_LOGD ("EXEC: '%s'", (cmd_log = g_strjoinv (" ", (char **) argv->pdata)));

	/* Supersede whatever firewall state a previous attempt left behind
	 * (including rules kept armed across an unexpected drop): this new
	 * attempt's own outcome decides what happens from here. */
	if (priv->kill_switch_active)
		nm_airvpn_firewall_disarm ();
	priv->kill_switch_active = FALSE;
	priv->unexpected_drop = FALSE;

	kill_switch = s_vpn ? nm_setting_vpn_get_data_item (s_vpn, NM_AIRVPN_KEY_KILL_SWITCH) : NULL;
	if (kill_switch && !strcmp (kill_switch, "yes")) {
		const char *allow_lan = nm_setting_vpn_get_data_item (s_vpn, NM_AIRVPN_KEY_ALLOW_LAN);

		/* Arm before the process even starts, so the initial handshake
		 * is covered too. If we can't enforce it, refuse to connect
		 * rather than silently connecting unprotected. */
		if (!nm_airvpn_firewall_arm (config_path, allow_lan && !strcmp (allow_lan, "yes"), error))
			return FALSE;
		priv->kill_switch_active = TRUE;
	}

	if (!g_spawn_async (NULL, (char **) argv->pdata, NULL,
	                    G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &pid, error)) {
		if (priv->kill_switch_active) {
			nm_airvpn_firewall_disarm ();
			priv->kill_switch_active = FALSE;
		}
		return FALSE;
	}

	_LOGI ("openvpn started with pid %d", pid);

	priv->pid = pid;
	g_child_watch_add (pid, openvpn_watch_cb, plugin);

	nm_clear_g_source (&priv->connect_timer);
	priv->connect_timer = g_timeout_add_seconds (CONNECT_TIMEOUT_SECONDS,
	                                             connect_timeout_cb, plugin);

	return TRUE;
}

static void
fetch_done_cb (GObject *source_object, GAsyncResult *result, gpointer user_data)
{
	NMAirvpnPlugin *plugin = NM_AIRVPN_PLUGIN (user_data);
	NMAirvpnPluginPrivate *priv = NM_AIRVPN_PLUGIN_GET_PRIVATE (plugin);
	gs_free_error GError *error = NULL;
	gs_free char *config = NULL;
	NMAirvpnGeneratorParams params;
	NMSettingVpn *s_vpn;

	config = nm_airvpn_generator_fetch_finish (result, &error);

	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		return;

	g_clear_object (&priv->fetch_cancellable);

	if (!priv->connection) {
		/* Disconnected while fetching. */
		return;
	}

	s_vpn = NM_SETTING_VPN (nm_connection_get_setting (priv->connection, NM_TYPE_SETTING_VPN));
	get_generator_params (s_vpn, &params);

	if (config) {
		if (!nm_airvpn_cache_store (NM_AIRVPN_STATEDIR, priv->uuid, &params,
		                            config, &error)) {
			_LOGW ("Could not store the fetched profile: %s", error->message);
			nm_vpn_service_plugin_failure (NM_VPN_SERVICE_PLUGIN (plugin),
			                               NM_VPN_PLUGIN_FAILURE_CONNECT_FAILED);
			return;
		}
		_LOGI ("Fetched a fresh profile from the AirVPN generator.");
	} else {
		/* Generator unreachable or rejected the request: fall back to a
		 * matching cached profile if we have one (even a stale one). */
		if (nm_airvpn_cache_exists (NM_AIRVPN_STATEDIR, priv->uuid, &params)) {
			_LOGW ("Generator fetch failed (%s); falling back to the cached profile.",
			       error->message);
		} else {
			_LOGW ("Generator fetch failed and no usable cache exists: %s", error->message);
			nm_vpn_service_plugin_failure (NM_VPN_SERVICE_PLUGIN (plugin),
			                               strstr (error->message, "API key")
			                                   ? NM_VPN_PLUGIN_FAILURE_LOGIN_FAILED
			                                   : NM_VPN_PLUGIN_FAILURE_CONNECT_FAILED);
			return;
		}
	}

	g_clear_error (&error);
	if (!run_openvpn (plugin, &error)) {
		_LOGW ("Could not start openvpn: %s", error->message);
		nm_vpn_service_plugin_failure (NM_VPN_SERVICE_PLUGIN (plugin),
		                               NM_VPN_PLUGIN_FAILURE_CONNECT_FAILED);
	}
}

static gboolean
_connect_common (NMVpnServicePlugin *plugin, NMConnection *connection, GError **error)
{
	NMAirvpnPluginPrivate *priv = NM_AIRVPN_PLUGIN_GET_PRIVATE (plugin);
	NMSettingVpn *s_vpn;
	NMAirvpnGeneratorParams params;
	const char *api_key;

	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	s_vpn = NM_SETTING_VPN (nm_connection_get_setting (connection, NM_TYPE_SETTING_VPN));
	g_return_val_if_fail (NM_IS_SETTING_VPN (s_vpn), FALSE);

	if (!nm_airvpn_properties_validate (s_vpn, error))
		return FALSE;

	if (!nm_airvpn_properties_validate_secrets (s_vpn, error))
		return FALSE;

	/* Stop any previous openvpn instance and pending fetch. */
	cleanup_plugin (NM_AIRVPN_PLUGIN (plugin));

	priv->connection = g_object_ref (connection);
	priv->uuid = g_strdup (nm_connection_get_uuid (connection));

	if (g_mkdir_with_parents (NM_AIRVPN_STATEDIR, 0700) != 0) {
		g_set_error (error,
		             NM_VPN_PLUGIN_ERROR,
		             NM_VPN_PLUGIN_ERROR_LAUNCH_FAILED,
		             _("Could not create state directory “%s”."),
		             NM_AIRVPN_STATEDIR);
		return FALSE;
	}

	get_generator_params (s_vpn, &params);

	if (nm_airvpn_cache_is_valid (NM_AIRVPN_STATEDIR, priv->uuid, &params)) {
		_LOGI ("Using the cached AirVPN profile.");
		return run_openvpn (NM_AIRVPN_PLUGIN (plugin), error);
	}

	api_key = nm_setting_vpn_get_secret (s_vpn, NM_AIRVPN_KEY_API_KEY);
	if (!api_key || !api_key[0]) {
		g_set_error (error,
		             NM_VPN_PLUGIN_ERROR,
		             NM_VPN_PLUGIN_ERROR_LAUNCH_FAILED,
		             _("No cached profile and no API key; get a key at %s."),
		             NM_AIRVPN_API_SETTINGS_URL);
		return FALSE;
	}

	_LOGI ("Cached profile is missing or stale; fetching from the AirVPN generator.");
	priv->fetch_cancellable = g_cancellable_new ();
	nm_airvpn_generator_fetch_async (&params, api_key,
	                                 priv->fetch_cancellable,
	                                 fetch_done_cb, plugin);
	return TRUE;
}

static gboolean
real_connect (NMVpnServicePlugin *plugin, NMConnection *connection, GError **error)
{
	return _connect_common (plugin, connection, error);
}

static gboolean
real_connect_interactive (NMVpnServicePlugin *plugin, NMConnection *connection,
                          GVariant *details, GError **error)
{
	NMAirvpnPluginPrivate *priv = NM_AIRVPN_PLUGIN_GET_PRIVATE (plugin);

	if (!_connect_common (plugin, connection, error))
		return FALSE;

	priv->interactive = TRUE;
	return TRUE;
}

static gboolean
real_need_secrets (NMVpnServicePlugin *plugin,
                   NMConnection *connection,
                   const char **setting_name,
                   GError **error)
{
	NMSetting *s_vpn;
	NMSettingSecretFlags flags = NM_SETTING_SECRET_FLAG_NONE;

	g_return_val_if_fail (NM_IS_VPN_SERVICE_PLUGIN (plugin), FALSE);
	g_return_val_if_fail (NM_IS_CONNECTION (connection), FALSE);

	s_vpn = nm_connection_get_setting (connection, NM_TYPE_SETTING_VPN);

	*setting_name = NM_SETTING_VPN_SETTING_NAME;

	/* The API key is always required: even with a valid on-disk config
	 * cache, a failed attempt may demand a refresh from the generator. */
	nm_setting_get_secret_flags (NM_SETTING (s_vpn), NM_AIRVPN_KEY_API_KEY, &flags, NULL);
	if (   !(flags & NM_SETTING_SECRET_FLAG_NOT_REQUIRED)
	    && !nm_setting_vpn_get_secret (NM_SETTING_VPN (s_vpn), NM_AIRVPN_KEY_API_KEY))
		return TRUE;

	*setting_name = NULL;
	return FALSE;
}

static gboolean
real_disconnect (NMVpnServicePlugin *plugin, GError **err)
{
	NMAirvpnPluginPrivate *priv = NM_AIRVPN_PLUGIN_GET_PRIVATE (plugin);

	/* A teardown while openvpn was still trying to connect (NM timeout,
	 * user cancel) most likely means the cached profile is bad. */
	if (priv->pid && !priv->started && priv->uuid) {
		_LOGI ("Disconnected while connecting; marking cached profile of %s stale.", priv->uuid);
		nm_airvpn_cache_mark_stale (NM_AIRVPN_STATEDIR, priv->uuid);
	}

	cleanup_plugin (NM_AIRVPN_PLUGIN (plugin));
	return TRUE;
}

static gboolean
real_new_secrets (NMVpnServicePlugin *plugin, NMConnection *connection, GError **error)
{
	return _connect_common (plugin, connection, error);
}

static void
state_changed_cb (GObject *object, NMVpnServiceState state, gpointer user_data)
{
	NMAirvpnPluginPrivate *priv = NM_AIRVPN_PLUGIN_GET_PRIVATE (object);

	switch (state) {
	case NM_VPN_SERVICE_STATE_STARTED:
		priv->started = TRUE;
		nm_clear_g_source (&priv->connect_timer);
		break;
	case NM_VPN_SERVICE_STATE_UNKNOWN:
	case NM_VPN_SERVICE_STATE_INIT:
	case NM_VPN_SERVICE_STATE_SHUTDOWN:
	case NM_VPN_SERVICE_STATE_STOPPING:
	case NM_VPN_SERVICE_STATE_STOPPED:
		nm_clear_g_source (&priv->connect_timer);
		g_clear_object (&priv->connection);
		break;
	default:
		break;
	}
}

static void
dispose (GObject *object)
{
	cleanup_plugin (NM_AIRVPN_PLUGIN (object));

	G_OBJECT_CLASS (nm_airvpn_plugin_parent_class)->dispose (object);
}

static void
nm_airvpn_plugin_init (NMAirvpnPlugin *plugin)
{
}

static void
nm_airvpn_plugin_class_init (NMAirvpnPluginClass *airvpn_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (airvpn_class);
	NMVpnServicePluginClass *parent_class = NM_VPN_SERVICE_PLUGIN_CLASS (airvpn_class);

	object_class->dispose = dispose;
	parent_class->connect = real_connect;
	parent_class->connect_interactive = real_connect_interactive;
	parent_class->need_secrets = real_need_secrets;
	parent_class->disconnect = real_disconnect;
	parent_class->new_secrets = real_new_secrets;
}

static GInitableIface *ginitable_parent_iface = NULL;

static gboolean
init_sync (GInitable *object, GCancellable *cancellable, GError **error)
{
	if (!ginitable_parent_iface->init (object, cancellable, error))
		return FALSE;

	g_signal_connect (G_OBJECT (object), "state-changed", G_CALLBACK (state_changed_cb), NULL);

	return TRUE;
}

static void
nm_airvpn_plugin_initable_iface_init (GInitableIface *iface)
{
	ginitable_parent_iface = g_type_interface_peek_parent (iface);
	iface->init = init_sync;
}

NMAirvpnPlugin *
nm_airvpn_plugin_new (const char *bus_name)
{
	NMAirvpnPlugin *plugin;
	GError *error = NULL;

	plugin = (NMAirvpnPlugin *) g_initable_new (NM_TYPE_AIRVPN_PLUGIN, NULL, &error,
	                                            NM_VPN_SERVICE_PLUGIN_DBUS_SERVICE_NAME, bus_name,
	                                            NM_VPN_SERVICE_PLUGIN_DBUS_WATCH_PEER, !gl.debug,
	                                            NULL);
	if (!plugin) {
		_LOGW ("Failed to initialize a plugin instance: %s", error->message);
		g_error_free (error);
	}

	return plugin;
}

static void
quit_mainloop (NMAirvpnPlugin *plugin, gpointer user_data)
{
	g_main_loop_quit ((GMainLoop *) user_data);
}

int
main (int argc, char *argv[])
{
	NMAirvpnPlugin *plugin;
	GMainLoop *main_loop;
	gboolean persist = FALSE;
	GOptionContext *opt_ctx = NULL;
	gs_free char *bus_name_free = NULL;
	const char *bus_name;
	GError *error = NULL;
	char sbuf[30];

	GOptionEntry options[] = {
		{ "persist", 0, 0, G_OPTION_ARG_NONE, &persist, N_("Don’t quit when VPN connection terminates"), NULL },
		{ "debug", 0, 0, G_OPTION_ARG_NONE, &gl.debug, N_("Enable verbose debug logging (may expose passwords)"), NULL },
		{ "bus-name", 0, 0, G_OPTION_ARG_STRING, &bus_name_free, N_("D-Bus name to use for this instance"), NULL },
		{NULL}
	};

	nm_g_type_init ();

	/* locale will be set according to environment LC_* variables */
	setlocale (LC_ALL, "");

	bindtextdomain (GETTEXT_PACKAGE, NM_AIRVPN_LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	/* Parse options */
	opt_ctx = g_option_context_new (NULL);
	g_option_context_set_translation_domain (opt_ctx, GETTEXT_PACKAGE);
	g_option_context_set_ignore_unknown_options (opt_ctx, FALSE);
	g_option_context_set_help_enabled (opt_ctx, TRUE);
	g_option_context_add_main_entries (opt_ctx, options, NULL);

	g_option_context_set_summary (opt_ctx,
	    _("nm-airvpn-service provides integrated AirVPN capability to NetworkManager."));

	if (!g_option_context_parse (opt_ctx, &argc, &argv, &error)) {
		g_printerr ("Error parsing the command line options: %s\n", error->message);
		g_option_context_free (opt_ctx);
		g_error_free (error);
		return EXIT_FAILURE;
	}
	g_option_context_free (opt_ctx);

	bus_name = bus_name_free ?: NM_DBUS_SERVICE_AIRVPN;

	gl.log_level = _nm_utils_ascii_str_to_int64 (getenv ("NM_VPN_LOG_LEVEL"),
	                                             10, 0, LOG_DEBUG, -1);
	if (gl.log_level < 0)
		gl.log_level = gl.debug ? LOG_DEBUG : LOG_NOTICE;

	_LOGD ("nm-airvpn-service (version " DIST_VERSION ") starting...");
	_LOGD ("   uses%s --bus-name \"%s\"", bus_name_free ? "" : " default", bus_name);

	setenv ("NM_VPN_LOG_LEVEL", nm_sprintf_buf (sbuf, "%d", gl.log_level), TRUE);
	setenv ("NM_VPN_LOG_PREFIX_TOKEN", nm_sprintf_buf (sbuf, "%ld", (long) getpid ()), TRUE);

	/* Clear any kill-switch table left behind by a previous instance that
	 * crashed instead of disarming it on its way out. */
	nm_airvpn_firewall_disarm ();

	plugin = nm_airvpn_plugin_new (bus_name);
	if (!plugin)
		exit (EXIT_FAILURE);

	main_loop = g_main_loop_new (NULL, FALSE);

	if (!persist)
		g_signal_connect (plugin, "quit", G_CALLBACK (quit_mainloop), main_loop);

	g_main_loop_run (main_loop);

	g_main_loop_unref (main_loop);
	g_object_unref (plugin);

	exit (EXIT_SUCCESS);
}
