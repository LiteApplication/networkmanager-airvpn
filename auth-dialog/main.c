/* auth-dialog - AirVPN integration with NetworkManager
 *
 * Prompts for the AirVPN API key when it is not stored. Supports GNOME
 * Shell's external UI mode (keyfile description on stdout) and a plain
 * GTK dialog fallback.
 *
 * Based on NetworkManager-fortisslvpn's auth-dialog
 * (C) Copyright 2008 - 2011 Red Hat, Inc.
 * (C) Copyright 2015,2017,2018 Lubomir Rintel
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "nm-default.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <gtk/gtk.h>
#include <libsecret/secret.h>

#include <nma-vpn-password-dialog.h>

#define KEYRING_UUID_TAG "connection-uuid"
#define KEYRING_SN_TAG "setting-name"
#define KEYRING_SK_TAG "setting-key"

static const SecretSchema network_manager_secret_schema = {
	"org.freedesktop.NetworkManager.Connection",
	SECRET_SCHEMA_DONT_MATCH_NAME,
	{
		{ KEYRING_UUID_TAG, SECRET_SCHEMA_ATTRIBUTE_STRING },
		{ KEYRING_SN_TAG, SECRET_SCHEMA_ATTRIBUTE_STRING },
		{ KEYRING_SK_TAG, SECRET_SCHEMA_ATTRIBUTE_STRING },
		{ NULL, 0 },
	}
};

#define UI_KEYFILE_GROUP "VPN Plugin UI"

static char *
keyring_lookup_secret (const char *uuid, const char *secret_name)
{
	GHashTable *attrs;
	GList *list;
	char *secret = NULL;

	attrs = secret_attributes_build (&network_manager_secret_schema,
	                                 KEYRING_UUID_TAG, uuid,
	                                 KEYRING_SN_TAG, NM_SETTING_VPN_SETTING_NAME,
	                                 KEYRING_SK_TAG, secret_name,
	                                 NULL);

	list = secret_service_search_sync (NULL, &network_manager_secret_schema, attrs,
	                                   SECRET_SEARCH_ALL | SECRET_SEARCH_UNLOCK | SECRET_SEARCH_LOAD_SECRETS,
	                                   NULL, NULL);
	if (list && list->data) {
		SecretItem *item = list->data;
		SecretValue *value = secret_item_get_secret (item);

		if (value) {
			secret = g_strdup (secret_value_get (value, NULL));
			secret_value_unref (value);
		}
	}

	g_list_free_full (list, g_object_unref);
	g_hash_table_unref (attrs);
	return secret;
}

static void
keyfile_add_entry_info (GKeyFile    *keyfile,
                        const gchar *key,
                        const gchar *value,
                        const gchar *label,
                        gboolean     is_secret,
                        gboolean     should_ask)
{
	g_key_file_set_string (keyfile, key, "Value", value);
	g_key_file_set_string (keyfile, key, "Label", label);
	g_key_file_set_boolean (keyfile, key, "IsSecret", is_secret);
	g_key_file_set_boolean (keyfile, key, "ShouldAsk", should_ask);
}

static void
keyfile_print_stdout (GKeyFile *keyfile)
{
	gchar *data;
	gsize length;

	data = g_key_file_to_data (keyfile, &length, NULL);

	fputs (data, stdout);

	g_free (data);
}

static gboolean
get_secrets (const char *vpn_uuid,
             const char *vpn_name,
             gboolean retry,
             gboolean allow_interaction,
             gboolean external_ui_mode,
             const char *in_api_key,
             char **out_api_key,
             NMSettingSecretFlags api_key_flags)
{
	NMAVpnPasswordDialog *dialog;
	gs_free char *prompt = NULL;
	gs_free char *pw = NULL;
	const char *new_password = NULL;

	g_return_val_if_fail (vpn_uuid != NULL, FALSE);
	g_return_val_if_fail (vpn_name != NULL, FALSE);
	g_return_val_if_fail (out_api_key != NULL, FALSE);
	g_return_val_if_fail (*out_api_key == NULL, FALSE);

	/* Get the existing secret, if any */
	if (   !(api_key_flags & NM_SETTING_SECRET_FLAG_NOT_SAVED)
	    && !(api_key_flags & NM_SETTING_SECRET_FLAG_NOT_REQUIRED)) {
		if (in_api_key)
			pw = g_strdup (in_api_key);
		else
			pw = keyring_lookup_secret (vpn_uuid, NM_AIRVPN_KEY_API_KEY);
	}

	if (api_key_flags & NM_SETTING_SECRET_FLAG_NOT_REQUIRED)
		return TRUE;

	prompt = g_strdup_printf (_("Your AirVPN API key is needed for the VPN connection “%s”. You can find it at %s."),
	                          vpn_name, NM_AIRVPN_API_SETTINGS_URL);

	if (external_ui_mode) {
		GKeyFile *keyfile;

		keyfile = g_key_file_new ();

		g_key_file_set_integer (keyfile, UI_KEYFILE_GROUP, "Version", 2);
		g_key_file_set_string (keyfile, UI_KEYFILE_GROUP, "Description", prompt);
		g_key_file_set_string (keyfile, UI_KEYFILE_GROUP, "Title", _("Authenticate VPN"));

		keyfile_add_entry_info (keyfile, NM_AIRVPN_KEY_API_KEY, pw ?: "",
		                        _("API key"), TRUE, allow_interaction);

		keyfile_print_stdout (keyfile);
		g_key_file_unref (keyfile);
		return TRUE;
	}

	if (   allow_interaction == FALSE
	    || (!retry && pw && !(api_key_flags & NM_SETTING_SECRET_FLAG_NOT_SAVED))) {
		/* If interaction isn't allowed, just return the existing secret.
		 * Also don't ask if we already have a saved key and this is not
		 * a re-prompt. */
		*out_api_key = g_steal_pointer (&pw);
		return TRUE;
	}

	gtk_init (NULL, NULL);

	dialog = (NMAVpnPasswordDialog *) nma_vpn_password_dialog_new (_("Authenticate VPN"), prompt, NULL);

	nma_vpn_password_dialog_set_show_password_secondary (dialog, FALSE);
	nma_vpn_password_dialog_set_password_label (dialog, _("API _key:"));

	if (pw && !(api_key_flags & NM_SETTING_SECRET_FLAG_NOT_SAVED))
		nma_vpn_password_dialog_set_password (dialog, pw);

	gtk_widget_show (GTK_WIDGET (dialog));

	if (nma_vpn_password_dialog_run_and_block (dialog)) {
		new_password = nma_vpn_password_dialog_get_password (dialog);
		if (new_password)
			*out_api_key = g_strdup (new_password);
	}

	gtk_widget_hide (GTK_WIDGET (dialog));
	gtk_widget_destroy (GTK_WIDGET (dialog));

	return TRUE;
}

static void
wait_for_quit (void)
{
	GString *str;
	char c;
	ssize_t n;
	time_t start;

	str = g_string_sized_new (10);
	start = time (NULL);
	do {
		errno = 0;
		n = read (0, &c, 1);
		if (n == 0 || (n < 0 && errno == EAGAIN))
			g_usleep (G_USEC_PER_SEC / 10);
		else if (n == 1) {
			g_string_append_c (str, c);
			if (strstr (str->str, "QUIT") || (str->len > 10))
				break;
		} else
			break;
	} while (time (NULL) < start + 20);
	g_string_free (str, TRUE);
}

int
main (int argc, char *argv[])
{
	gboolean retry = FALSE, allow_interaction = FALSE, external_ui_mode = FALSE;
	gs_free char *vpn_name = NULL;
	gs_free char *vpn_uuid = NULL;
	gs_free char *vpn_service = NULL;
	gs_strfreev char **hints = NULL;
	char *api_key = NULL;
	GHashTable *data = NULL, *secrets = NULL;
	NMSettingSecretFlags api_key_flags = NM_SETTING_SECRET_FLAG_NONE;
	GOptionContext *context;
	GOptionEntry entries[] = {
			{ "reprompt", 'r', 0, G_OPTION_ARG_NONE, &retry, "Reprompt for passwords", NULL},
			{ "uuid", 'u', 0, G_OPTION_ARG_STRING, &vpn_uuid, "UUID of VPN connection", NULL},
			{ "name", 'n', 0, G_OPTION_ARG_STRING, &vpn_name, "Name of VPN connection", NULL},
			{ "service", 's', 0, G_OPTION_ARG_STRING, &vpn_service, "VPN service type", NULL},
			{ "allow-interaction", 'i', 0, G_OPTION_ARG_NONE, &allow_interaction, "Allow user interaction", NULL},
			{ "external-ui-mode", 0, 0, G_OPTION_ARG_NONE, &external_ui_mode, "External UI mode", NULL},
			{ "hint", 't', 0, G_OPTION_ARG_STRING_ARRAY, &hints, "Hints from the VPN plugin", NULL},
			{ NULL }
		};

	bindtextdomain (GETTEXT_PACKAGE, NULL);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	context = g_option_context_new ("- airvpn auth dialog");
	g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
	g_option_context_parse (context, &argc, &argv, NULL);
	g_option_context_free (context);

	if (!vpn_uuid || !vpn_service || !vpn_name) {
		fprintf (stderr, "A connection UUID, name, and VPN plugin service name are required.\n");
		return 1;
	}

	if (strcmp (vpn_service, NM_DBUS_SERVICE_AIRVPN) != 0) {
		fprintf (stderr, "This dialog only works with the '%s' service\n", NM_DBUS_SERVICE_AIRVPN);
		return 1;
	}

	if (!nm_vpn_service_plugin_read_vpn_details (0, &data, &secrets)) {
		fprintf (stderr, "Failed to read '%s' (%s) data and secrets from stdin.\n",
		         vpn_name, vpn_uuid);
		return 1;
	}

	nm_vpn_service_plugin_get_secret_flags (data, NM_AIRVPN_KEY_API_KEY, &api_key_flags);

	if (!get_secrets (vpn_uuid, vpn_name, retry, allow_interaction,
	                  external_ui_mode,
	                  g_hash_table_lookup (secrets, NM_AIRVPN_KEY_API_KEY),
	                  &api_key,
	                  api_key_flags))
		return 1;

	if (!external_ui_mode) {
		/* dump the secrets to stdout */
		if (api_key)
			printf ("%s\n%s\n", NM_AIRVPN_KEY_API_KEY, api_key);
		printf ("\n\n");

		g_free (api_key);

		/* for good measure, flush stdout since Kansas is going Bye-Bye */
		fflush (stdout);

		/* Wait for quit signal */
		wait_for_quit ();
	}

	if (data)
		g_hash_table_unref (data);
	if (secrets)
		g_hash_table_unref (secrets);
	return 0;
}
