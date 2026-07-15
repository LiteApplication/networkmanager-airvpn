/* nm-airvpn-service - AirVPN integration with NetworkManager
 *
 * Fetches OpenVPN configuration from the AirVPN generator API
 * (https://airvpn.org/api/generator/). The blocking libcurl transfers
 * run in a GTask worker thread so the caller's main loop never stalls.
 *
 * Device handling: the generator silently issues profiles for the
 * account's Default device when the requested device name is unknown,
 * which is confusing. The configured name is therefore validated
 * against the account's device list first and unknown names fail with
 * a clear error.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "nm-default.h"

#include "nm-airvpn-api.h"

#include <string.h>

#include <curl/curl.h>
#include <json-glib/json-glib.h>

#define AIRVPN_GENERATOR_URL "https://airvpn.org/api/generator/"
#define AIRVPN_DEVICES_URL   "https://airvpn.org/api/devices/"

/* A single-server .ovpn is ~9 KiB; anything above this is not a config. */
#define MAX_RESPONSE_SIZE (4 * 1024 * 1024)

static void
generator_params_free (NMAirvpnGeneratorParams *params)
{
	if (!params)
		return;
	g_free (params->server);
	g_free (params->device);
	g_free (params->protocol);
	g_free (params->port);
	g_free (params->custom_directives);
	g_slice_free (NMAirvpnGeneratorParams, params);
}

static NMAirvpnGeneratorParams *
generator_params_copy (const NMAirvpnGeneratorParams *params)
{
	NMAirvpnGeneratorParams *copy;

	copy = g_slice_new0 (NMAirvpnGeneratorParams);
	copy->server = g_strdup (params->server);
	copy->device = g_strdup (params->device);
	copy->protocol = g_strdup (params->protocol);
	copy->port = g_strdup (params->port);
	copy->custom_directives = g_strdup (params->custom_directives);
	return copy;
}

static size_t
write_cb (char *ptr, size_t size, size_t nmemb, void *user_data)
{
	GString *buf = user_data;
	size_t total = size * nmemb;

	if (buf->len + total > MAX_RESPONSE_SIZE)
		return 0;

	g_string_append_len (buf, ptr, total);
	return total;
}

static GString *
http_get (const char *url, const char *api_key, GError **error)
{
	CURL *curl;
	CURLcode res;
	struct curl_slist *headers = NULL;
	gs_free char *api_key_header = NULL;
	GString *body;
	char errbuf[CURL_ERROR_SIZE] = { 0 };
	long http_status = 0;

	curl = curl_easy_init ();
	if (!curl) {
		g_set_error_literal (error,
		                     NM_VPN_PLUGIN_ERROR,
		                     NM_VPN_PLUGIN_ERROR_LAUNCH_FAILED,
		                     _("Could not initialize libcurl."));
		return NULL;
	}

	body = g_string_sized_new (16 * 1024);

	api_key_header = g_strdup_printf ("API-KEY: %s", api_key);
	headers = curl_slist_append (NULL, api_key_header);

	curl_easy_setopt (curl, CURLOPT_URL, url);
	curl_easy_setopt (curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt (curl, CURLOPT_WRITEDATA, body);
	curl_easy_setopt (curl, CURLOPT_ERRORBUFFER, errbuf);
	curl_easy_setopt (curl, CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt (curl, CURLOPT_TIMEOUT, 60L);
	curl_easy_setopt (curl, CURLOPT_CONNECTTIMEOUT, 20L);
	curl_easy_setopt (curl, CURLOPT_USERAGENT, "networkmanager-airvpn/" VERSION);
	curl_easy_setopt (curl, CURLOPT_PROTOCOLS_STR, "https");

	res = curl_easy_perform (curl);
	curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &http_status);
	curl_slist_free_all (headers);
	curl_easy_cleanup (curl);

	if (res != CURLE_OK) {
		g_set_error (error,
		             NM_VPN_PLUGIN_ERROR,
		             NM_VPN_PLUGIN_ERROR_LAUNCH_FAILED,
		             _("Could not reach the AirVPN API: %s"),
		             errbuf[0] ? errbuf : curl_easy_strerror (res));
		g_string_free (body, TRUE);
		return NULL;
	}

	if (http_status != 200) {
		g_set_error (error,
		             NM_VPN_PLUGIN_ERROR,
		             NM_VPN_PLUGIN_ERROR_LAUNCH_FAILED,
		             _("AirVPN API returned HTTP status %ld."),
		             http_status);
		g_string_free (body, TRUE);
		return NULL;
	}

	return body;
}

/* The API answers HTTP 200 even on failure, with a JSON error body. */
static gboolean
check_json_error (const GString *body, GError **error)
{
	if (body->len > 0 && body->str[0] == '{' && strstr (body->str, "\"error\"")) {
		if (strstr (body->str, "Not authorized")) {
			g_set_error (error,
			             NM_VPN_PLUGIN_ERROR,
			             NM_VPN_PLUGIN_ERROR_LAUNCH_FAILED,
			             _("AirVPN rejected the API key (get a key at %s)."),
			             NM_AIRVPN_API_SETTINGS_URL);
		} else {
			gs_free char *excerpt = g_strndup (body->str, MIN (body->len, 200));

			g_set_error (error,
			             NM_VPN_PLUGIN_ERROR,
			             NM_VPN_PLUGIN_ERROR_LAUNCH_FAILED,
			             _("AirVPN API request failed: %s"),
			             excerpt);
		}
		return FALSE;
	}
	return TRUE;
}

/* The generator silently falls back to the Default device identity for
 * unknown device names; validate the configured name instead. */
static gboolean
validate_device (const NMAirvpnGeneratorParams *params,
                 const char *api_key,
                 GError **error)
{
	nm_auto_free_gstring GString *body = NULL;
	gs_unref_object JsonParser *parser = NULL;
	JsonObject *root;
	JsonArray *devices;
	guint i, len;

	if (!params->device || !params->device[0])
		return TRUE;

	body = http_get (AIRVPN_DEVICES_URL, api_key, error);
	if (!body)
		return FALSE;
	if (!check_json_error (body, error))
		return FALSE;

	parser = json_parser_new ();
	if (!json_parser_load_from_data (parser, body->str, body->len, error))
		return FALSE;

	root = json_node_get_object (json_parser_get_root (parser));
	devices = root && json_object_has_member (root, "devices")
	          ? json_object_get_array_member (root, "devices")
	          : NULL;
	if (!devices) {
		g_set_error_literal (error,
		                     NM_VPN_PLUGIN_ERROR,
		                     NM_VPN_PLUGIN_ERROR_LAUNCH_FAILED,
		                     _("Unexpected AirVPN devices list format."));
		return FALSE;
	}

	len = json_array_get_length (devices);
	for (i = 0; i < len; i++) {
		JsonObject *item = json_array_get_object_element (devices, i);
		const char *name = json_object_has_member (item, "name")
		                   ? json_object_get_string_member (item, "name")
		                   : NULL;

		if (name && !g_ascii_strcasecmp (name, params->device))
			return TRUE;
	}

	g_set_error (error,
	             NM_VPN_PLUGIN_ERROR,
	             NM_VPN_PLUGIN_ERROR_LAUNCH_FAILED,
	             _("Device “%s” does not exist on your AirVPN account. "
	               "Pick one of your devices in the connection editor "
	               "(Load devices) or create it at airvpn.org."),
	             params->device);
	return FALSE;
}

static char *
build_generator_url (const NMAirvpnGeneratorParams *params, CURL *curl)
{
	GString *url;
	const char *protocol;
	const char *port;
	char *escaped;

	protocol = params->protocol && params->protocol[0] ? params->protocol : NM_AIRVPN_PROTOCOL_UDP;
	port = params->port && params->port[0] ? params->port : NM_AIRVPN_DEFAULT_PORT;

	url = g_string_new (AIRVPN_GENERATOR_URL);
	g_string_append (url, "?system=linux&download=auto");

	g_string_append_printf (url, "&protocols=openvpn_3_%s_%s", protocol, port);

	escaped = curl_easy_escape (curl, params->server, 0);
	g_string_append_printf (url, "&servers=%s", escaped);
	curl_free (escaped);

	if (params->device && params->device[0]) {
		escaped = curl_easy_escape (curl, params->device, 0);
		g_string_append_printf (url, "&device=%s", escaped);
		curl_free (escaped);
	}

	if (params->custom_directives && params->custom_directives[0]) {
		escaped = curl_easy_escape (curl, params->custom_directives, 0);
		g_string_append_printf (url, "&openvpn_directives=%s", escaped);
		curl_free (escaped);
	}

	return g_string_free (url, FALSE);
}

static gboolean
validate_profile (const GString *body, GError **error)
{
	if (body->len == 0) {
		g_set_error_literal (error,
		                     NM_VPN_PLUGIN_ERROR,
		                     NM_VPN_PLUGIN_ERROR_LAUNCH_FAILED,
		                     _("AirVPN generator returned an empty response."));
		return FALSE;
	}

	if (!check_json_error (body, error))
		return FALSE;

	if (   !strstr (body->str, "client")
	    || !strstr (body->str, "<ca>")
	    || !strstr (body->str, "<key>")) {
		g_set_error_literal (error,
		                     NM_VPN_PLUGIN_ERROR,
		                     NM_VPN_PLUGIN_ERROR_LAUNCH_FAILED,
		                     _("AirVPN generator response is not an OpenVPN profile."));
		return FALSE;
	}

	return TRUE;
}

char *
nm_airvpn_generator_fetch_sync (const NMAirvpnGeneratorParams *params,
                                const char *api_key,
                                GCancellable *cancellable,
                                GError **error)
{
	CURL *curl;
	gs_free char *url = NULL;
	nm_auto_free_gstring GString *body = NULL;

	g_return_val_if_fail (params && params->server && params->server[0], NULL);
	g_return_val_if_fail (api_key && api_key[0], NULL);

	if (!validate_device (params, api_key, error))
		return NULL;

	if (g_cancellable_set_error_if_cancelled (cancellable, error))
		return NULL;

	curl = curl_easy_init ();
	if (!curl) {
		g_set_error_literal (error,
		                     NM_VPN_PLUGIN_ERROR,
		                     NM_VPN_PLUGIN_ERROR_LAUNCH_FAILED,
		                     _("Could not initialize libcurl."));
		return NULL;
	}
	url = build_generator_url (params, curl);
	curl_easy_cleanup (curl);

	body = http_get (url, api_key, error);
	if (!body)
		return NULL;

	if (g_cancellable_set_error_if_cancelled (cancellable, error))
		return NULL;

	if (!validate_profile (body, error))
		return NULL;

	return g_string_free (g_steal_pointer (&body), FALSE);
}

typedef struct {
	NMAirvpnGeneratorParams *params;
	char *api_key;
} FetchTaskData;

static void
fetch_task_data_free (gpointer data)
{
	FetchTaskData *task_data = data;

	generator_params_free (task_data->params);
	if (task_data->api_key) {
		memset (task_data->api_key, 0, strlen (task_data->api_key));
		g_free (task_data->api_key);
	}
	g_slice_free (FetchTaskData, task_data);
}

static void
fetch_thread_cb (GTask *task,
                 gpointer source_object,
                 gpointer task_data_ptr,
                 GCancellable *cancellable)
{
	FetchTaskData *task_data = task_data_ptr;
	GError *error = NULL;
	char *config;

	config = nm_airvpn_generator_fetch_sync (task_data->params,
	                                         task_data->api_key,
	                                         cancellable,
	                                         &error);
	if (config)
		g_task_return_pointer (task, config, g_free);
	else
		g_task_return_error (task, error);
}

void
nm_airvpn_generator_fetch_async (const NMAirvpnGeneratorParams *params,
                                 const char *api_key,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data)
{
	GTask *task;
	FetchTaskData *task_data;

	task_data = g_slice_new0 (FetchTaskData);
	task_data->params = generator_params_copy (params);
	task_data->api_key = g_strdup (api_key);

	task = g_task_new (NULL, cancellable, callback, user_data);
	g_task_set_source_tag (task, nm_airvpn_generator_fetch_async);
	g_task_set_task_data (task, task_data, fetch_task_data_free);
	g_task_run_in_thread (task, fetch_thread_cb);
	g_object_unref (task);
}

char *
nm_airvpn_generator_fetch_finish (GAsyncResult *result, GError **error)
{
	g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

	return g_task_propagate_pointer (G_TASK (result), error);
}
