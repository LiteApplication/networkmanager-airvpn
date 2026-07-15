/* networkmanager-airvpn - AirVPN integration with NetworkManager
 *
 * Parses the AirVPN /api/status/ JSON (bundled snapshot or freshly
 * fetched) and fills the server selection combo with earth, continents,
 * countries and individual servers.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "nm-default.h"

#include "nm-airvpn-server-list.h"

#include <string.h>

#include <json-glib/json-glib.h>
#include <curl/curl.h>

#define AIRVPN_STATUS_URL  "https://airvpn.org/api/status/"
#define AIRVPN_DEVICES_URL "https://airvpn.org/api/devices/"
#define MAX_RESPONSE_SIZE (8 * 1024 * 1024)

char *
nm_airvpn_server_list_load_snapshot (gsize *len)
{
	GBytes *bytes;
	char *text;
	gsize size;

	bytes = g_resources_lookup_data ("/org/freedesktop/network-manager-airvpn/status.json",
	                                 G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
	if (!bytes)
		return NULL;

	text = g_strndup (g_bytes_get_data (bytes, &size), g_bytes_get_size (bytes));
	if (len)
		*len = g_bytes_get_size (bytes);
	g_bytes_unref (bytes);
	return text;
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

static char *
http_get (const char *url, const char *api_key, GError **error)
{
	CURL *curl;
	CURLcode res;
	struct curl_slist *headers = NULL;
	GString *body;
	char errbuf[CURL_ERROR_SIZE] = { 0 };
	long http_status = 0;

	curl = curl_easy_init ();
	if (!curl) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		                     _("Could not initialize libcurl."));
		return NULL;
	}

	body = g_string_sized_new (256 * 1024);

	if (api_key && api_key[0]) {
		gs_free char *header = g_strdup_printf ("API-KEY: %s", api_key);

		headers = curl_slist_append (NULL, header);
		curl_easy_setopt (curl, CURLOPT_HTTPHEADER, headers);
	}

	curl_easy_setopt (curl, CURLOPT_URL, url);
	curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt (curl, CURLOPT_WRITEDATA, body);
	curl_easy_setopt (curl, CURLOPT_ERRORBUFFER, errbuf);
	curl_easy_setopt (curl, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt (curl, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt (curl, CURLOPT_USERAGENT, "networkmanager-airvpn/" VERSION);
	curl_easy_setopt (curl, CURLOPT_PROTOCOLS_STR, "https");

	res = curl_easy_perform (curl);
	curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &http_status);
	if (headers)
		curl_slist_free_all (headers);
	curl_easy_cleanup (curl);

	if (res != CURLE_OK || http_status != 200) {
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		             _("Could not reach airvpn.org: %s"),
		             errbuf[0] ? errbuf : curl_easy_strerror (res));
		g_string_free (body, TRUE);
		return NULL;
	}

	return g_string_free (body, FALSE);
}

char *
nm_airvpn_server_list_fetch (GError **error)
{
	return http_get (AIRVPN_STATUS_URL, NULL, error);
}

char **
nm_airvpn_device_list_fetch (const char *api_key, GError **error)
{
	gs_free char *json = NULL;
	gs_unref_object JsonParser *parser = NULL;
	JsonObject *root;
	JsonArray *devices;
	GPtrArray *names;
	guint i, len;

	json = http_get (AIRVPN_DEVICES_URL, api_key, error);
	if (!json)
		return NULL;

	if (json[0] == '{' && strstr (json, "\"error\"")) {
		if (strstr (json, "Not authorized")) {
			g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
			             _("AirVPN rejected the API key (get a key at %s)."),
			             NM_AIRVPN_API_SETTINGS_URL);
			return NULL;
		}
	}

	parser = json_parser_new ();
	if (!json_parser_load_from_data (parser, json, -1, error))
		return NULL;

	root = json_node_get_object (json_parser_get_root (parser));
	devices = root && json_object_has_member (root, "devices")
	          ? json_object_get_array_member (root, "devices")
	          : NULL;
	if (!devices) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		                     _("Unexpected AirVPN devices list format."));
		return NULL;
	}

	names = g_ptr_array_new ();
	len = json_array_get_length (devices);
	for (i = 0; i < len; i++) {
		JsonObject *item = json_array_get_object_element (devices, i);
		const char *name = json_object_has_member (item, "name")
		                   ? json_object_get_string_member (item, "name")
		                   : NULL;

		if (name && name[0])
			g_ptr_array_add (names, g_strdup (name));
	}
	g_ptr_array_add (names, NULL);

	return (char **) g_ptr_array_free (names, FALSE);
}

static const char *
member_string (JsonObject *obj, const char *name)
{
	JsonNode *node;

	if (!json_object_has_member (obj, name))
		return NULL;
	node = json_object_get_member (obj, name);
	if (!JSON_NODE_HOLDS_VALUE (node))
		return NULL;
	return json_node_get_string (node);
}

static gint64
member_int (JsonObject *obj, const char *name)
{
	JsonNode *node;

	if (!json_object_has_member (obj, name))
		return -1;
	node = json_object_get_member (obj, name);
	if (!JSON_NODE_HOLDS_VALUE (node))
		return -1;
	return json_node_get_int (node);
}

typedef struct {
	GtkComboBoxText *combo;
	const char *select_token;
	gboolean selected;
} FillInfo;

static void
combo_add (FillInfo *info, const char *token, const char *display)
{
	gtk_combo_box_text_append (info->combo, token, display);
	if (info->select_token && !g_ascii_strcasecmp (info->select_token, token)) {
		gtk_combo_box_set_active_id (GTK_COMBO_BOX (info->combo), token);
		info->selected = TRUE;
	}
}

static int
sort_by_string (gconstpointer a, gconstpointer b)
{
	return g_utf8_collate (*(const char *const*) a, *(const char *const*) b);
}

gboolean
nm_airvpn_server_list_fill_combo (GtkComboBoxText *combo,
                                  const char *json_text,
                                  gssize json_len,
                                  const char *select_token,
                                  GError **error)
{
	JsonParser *parser;
	JsonObject *root;
	JsonArray *array;
	FillInfo info = { combo, select_token, FALSE };
	guint i, len;

	parser = json_parser_new ();
	if (!json_parser_load_from_data (parser, json_text, json_len, error)) {
		g_object_unref (parser);
		return FALSE;
	}

	root = json_node_get_object (json_parser_get_root (parser));
	if (!root) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		                     _("Unexpected server list format."));
		g_object_unref (parser);
		return FALSE;
	}

	gtk_combo_box_text_remove_all (combo);

	combo_add (&info, "earth", _("Earth — any server"));

	if (json_object_has_member (root, "continents")) {
		array = json_object_get_array_member (root, "continents");
		len = json_array_get_length (array);
		for (i = 0; i < len; i++) {
			JsonObject *item = json_array_get_object_element (array, i);
			const char *name = member_string (item, "public_name");
			gs_free char *token = NULL;
			gs_free char *display = NULL;

			if (!name)
				continue;
			token = g_ascii_strdown (name, -1);
			display = g_strdup_printf (_("%s — best server"), name);
			combo_add (&info, token, display);
		}
	}

	if (json_object_has_member (root, "countries")) {
		gs_unref_ptrarray GPtrArray *lines = g_ptr_array_new_with_free_func (g_free);

		array = json_object_get_array_member (root, "countries");
		len = json_array_get_length (array);
		for (i = 0; i < len; i++) {
			JsonObject *item = json_array_get_object_element (array, i);
			const char *name = member_string (item, "country_name");
			const char *code = member_string (item, "country_code");
			gint64 n_servers = member_int (item, "servers");

			if (!name || !code)
				continue;
			g_ptr_array_add (lines,
			                 g_strdup_printf ("%s\n%s (%" G_GINT64_FORMAT ")",
			                                  code, name, MAX (n_servers, 0)));
		}
		g_ptr_array_sort (lines, sort_by_string);
		for (i = 0; i < lines->len; i++) {
			char *line = lines->pdata[i];
			char *nl = strchr (line, '\n');

			*nl = '\0';
			combo_add (&info, line, nl + 1);
		}
	}

	if (json_object_has_member (root, "servers")) {
		gs_unref_ptrarray GPtrArray *lines = g_ptr_array_new_with_free_func (g_free);

		array = json_object_get_array_member (root, "servers");
		len = json_array_get_length (array);
		for (i = 0; i < len; i++) {
			JsonObject *item = json_array_get_object_element (array, i);
			const char *name = member_string (item, "public_name");
			const char *country = member_string (item, "country_name");
			const char *location = member_string (item, "location");
			gint64 load = member_int (item, "currentload");
			const char *health = member_string (item, "health");

			if (!name)
				continue;
			g_ptr_array_add (lines,
			                 g_strdup_printf ("%s\n%s — %s, %s (%" G_GINT64_FORMAT "%%%s%s)",
			                                  name, name,
			                                  country ?: "?", location ?: "?",
			                                  MAX (load, 0),
			                                  health && strcmp (health, "ok") ? ", " : "",
			                                  health && strcmp (health, "ok") ? health : ""));
		}
		g_ptr_array_sort (lines, sort_by_string);
		for (i = 0; i < lines->len; i++) {
			char *line = lines->pdata[i];
			char *nl = strchr (line, '\n');

			*nl = '\0';
			combo_add (&info, line, nl + 1);
		}
	}

	g_object_unref (parser);

	/* Keep an unknown (hand-entered) token selectable. */
	if (select_token && select_token[0] && !info.selected) {
		combo_add (&info, select_token, select_token);
		gtk_combo_box_set_active_id (GTK_COMBO_BOX (combo), select_token);
	}

	if (gtk_combo_box_get_active (GTK_COMBO_BOX (combo)) < 0)
		gtk_combo_box_set_active_id (GTK_COMBO_BOX (combo), "earth");

	return TRUE;
}
