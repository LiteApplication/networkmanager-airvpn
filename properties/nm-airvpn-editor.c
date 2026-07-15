/* networkmanager-airvpn - AirVPN integration with NetworkManager
 *
 * The GTK editor widget embedded in nm-connection-editor and
 * gnome-control-center. Compiled twice: against GTK3 + libnma and
 * against GTK4 + libnma-gtk4.
 *
 * Based on nm-fortisslvpn-editor.c
 * Copyright (C) 2015,2017 Lubomir Rintel <lkundrak@v3.sk>
 * Copyright (C) 2008 - 2011 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "nm-default.h"

#include "nm-airvpn-editor.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <gtk/gtk.h>

#include "nm-airvpn-properties.h"
#include "nm-airvpn-server-list.h"

/*****************************************************************************/

#if !GTK_CHECK_VERSION(4,0,0)
#define gtk_editable_set_text(editable,text)    gtk_entry_set_text(GTK_ENTRY(editable), (text))
#define gtk_editable_get_text(editable)         gtk_entry_get_text(GTK_ENTRY(editable))
#define gtk_widget_set_sensitive_compat(w,s)    gtk_widget_set_sensitive((w), (s))
#define combo_get_entry(c)                      gtk_bin_get_child (GTK_BIN (c))
/* GtkCheckButton is a GtkToggleButton subclass in GTK3. */
#define check_get_active(cb)                    gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (cb))
#define check_set_active(cb,v)                  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (cb), (v))
#else
#define gtk_widget_set_sensitive_compat(w,s)    gtk_widget_set_sensitive((w), (s))
#define combo_get_entry(c)                      gtk_combo_box_get_child (GTK_COMBO_BOX (c))
/* GTK4 rewrote GtkCheckButton as a plain GtkWidget, no longer a
 * GtkToggleButton; GTK_TOGGLE_BUTTON()'s type check fails and the get/set
 * calls silently no-op, so this needs its own API in GTK4. */
#define check_get_active(cb)                     gtk_check_button_get_active (GTK_CHECK_BUTTON (cb))
#define check_set_active(cb,v)                   gtk_check_button_set_active (GTK_CHECK_BUTTON (cb), (v))
#endif

/*****************************************************************************/

typedef struct {
	GtkBuilder *builder;
	GtkWidget *widget;
	gboolean new_connection;
	GCancellable *refresh_cancellable;
	GCancellable *devices_cancellable;
	char *uuid;
	GCancellable *reconnect_cancellable;
	gboolean reconnect_busy;
} AirvpnEditorPrivate;

static void airvpn_editor_interface_init (NMVpnEditorInterface *iface_class);

G_DEFINE_TYPE_EXTENDED (AirvpnEditor, airvpn_editor, G_TYPE_OBJECT, 0,
                        G_ADD_PRIVATE (AirvpnEditor)
                        G_IMPLEMENT_INTERFACE (NM_TYPE_VPN_EDITOR,
                                               airvpn_editor_interface_init))

#define AIRVPN_EDITOR_GET_PRIVATE(o) ((AirvpnEditorPrivate *) airvpn_editor_get_instance_private ((AirvpnEditor *) (o)))

/*****************************************************************************/

static void
stuff_changed_cb (GtkWidget *widget, gpointer user_data)
{
	g_signal_emit_by_name (AIRVPN_EDITOR (user_data), "changed");
}

static void
keepalive_toggled_cb (GtkWidget *widget, gpointer user_data)
{
	AirvpnEditor *self = AIRVPN_EDITOR (user_data);
	AirvpnEditorPrivate *priv = AIRVPN_EDITOR_GET_PRIVATE (self);
	gboolean active = check_get_active (widget);
	GtkWidget *interval = GTK_WIDGET (gtk_builder_get_object (priv->builder, "keepalive_interval_spin"));
	GtkWidget *restart = GTK_WIDGET (gtk_builder_get_object (priv->builder, "keepalive_restart_spin"));

	gtk_widget_set_sensitive_compat (interval, active);
	gtk_widget_set_sensitive_compat (restart, active);

	stuff_changed_cb (widget, user_data);
}

static void
kill_switch_toggled_cb (GtkWidget *widget, gpointer user_data)
{
	AirvpnEditor *self = AIRVPN_EDITOR (user_data);
	AirvpnEditorPrivate *priv = AIRVPN_EDITOR_GET_PRIVATE (self);
	gboolean active = check_get_active (widget);
	GtkWidget *allow_lan = GTK_WIDGET (gtk_builder_get_object (priv->builder, "allow_lan_check"));

	/* The LAN exception is meaningless without the kill switch itself. */
	gtk_widget_set_sensitive_compat (allow_lan, active);

	stuff_changed_cb (widget, user_data);
}

static void
password_storage_changed_cb (GObject *entry,
                             GParamSpec *pspec,
                             gpointer user_data)
{
	stuff_changed_cb (NULL, AIRVPN_EDITOR (user_data));
}

static void
refresh_done_cb (GObject *source_object, GAsyncResult *result, gpointer user_data)
{
	AirvpnEditor *self;
	AirvpnEditorPrivate *priv;
	GTask *task = G_TASK (result);
	gs_free char *json = NULL;
	gs_free_error GError *error = NULL;
	GtkComboBoxText *combo;
	GtkWidget *button;
	gs_free char *token = NULL;

	json = g_task_propagate_pointer (task, &error);

	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		return;

	self = AIRVPN_EDITOR (user_data);
	priv = AIRVPN_EDITOR_GET_PRIVATE (self);
	g_clear_object (&priv->refresh_cancellable);

	button = GTK_WIDGET (gtk_builder_get_object (priv->builder, "refresh_button"));
	gtk_widget_set_sensitive_compat (button, TRUE);

	if (!json) {
		g_warning ("Could not refresh the AirVPN server list: %s", error->message);
		return;
	}

	combo = GTK_COMBO_BOX_TEXT (gtk_builder_get_object (priv->builder, "server_combo"));
	token = g_strdup (gtk_combo_box_get_active_id (GTK_COMBO_BOX (combo)));
	if (!nm_airvpn_server_list_fill_combo (combo, json, -1, token, &error))
		g_warning ("Could not parse the refreshed server list: %s", error->message);
}

static void
refresh_thread_cb (GTask *task,
                   gpointer source_object,
                   gpointer task_data,
                   GCancellable *cancellable)
{
	GError *error = NULL;
	char *json;

	json = nm_airvpn_server_list_fetch (&error);
	if (json)
		g_task_return_pointer (task, json, g_free);
	else
		g_task_return_error (task, error);
}

static void
refresh_button_clicked_cb (GtkWidget *button, gpointer user_data)
{
	AirvpnEditor *self = AIRVPN_EDITOR (user_data);
	AirvpnEditorPrivate *priv = AIRVPN_EDITOR_GET_PRIVATE (self);
	GTask *task;

	if (priv->refresh_cancellable)
		return;

	gtk_widget_set_sensitive_compat (button, FALSE);

	priv->refresh_cancellable = g_cancellable_new ();
	task = g_task_new (NULL, priv->refresh_cancellable, refresh_done_cb, self);
	g_task_set_source_tag (task, refresh_button_clicked_cb);
	g_task_run_in_thread (task, refresh_thread_cb);
	g_object_unref (task);
}

/*****************************************************************************/
/* "Load devices": fill the device combo with the account's devices. */

static void
devices_done_cb (GObject *source_object, GAsyncResult *result, gpointer user_data)
{
	AirvpnEditor *self;
	AirvpnEditorPrivate *priv;
	GTask *task = G_TASK (result);
	gs_strfreev char **names = NULL;
	gs_free_error GError *error = NULL;
	GtkComboBoxText *combo;
	GtkWidget *button;
	guint i;

	names = g_task_propagate_pointer (task, &error);

	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		return;

	self = AIRVPN_EDITOR (user_data);
	priv = AIRVPN_EDITOR_GET_PRIVATE (self);
	g_clear_object (&priv->devices_cancellable);

	button = GTK_WIDGET (gtk_builder_get_object (priv->builder, "devices_button"));
	gtk_widget_set_sensitive_compat (button, TRUE);

	if (!names) {
		g_warning ("Could not fetch the AirVPN device list: %s", error->message);
		gtk_widget_set_tooltip_text (button, error->message);
		return;
	}

	combo = GTK_COMBO_BOX_TEXT (gtk_builder_get_object (priv->builder, "device_combo"));
	/* Clears only the list; the entry text is preserved. */
	gtk_combo_box_text_remove_all (combo);
	for (i = 0; names[i]; i++)
		gtk_combo_box_text_append (combo, names[i], names[i]);
}

static void
devices_thread_cb (GTask *task,
                   gpointer source_object,
                   gpointer task_data,
                   GCancellable *cancellable)
{
	GError *error = NULL;
	char **names;

	names = nm_airvpn_device_list_fetch (task_data, &error);
	if (names)
		g_task_return_pointer (task, names, (GDestroyNotify) g_strfreev);
	else
		g_task_return_error (task, error);
}

static void
devices_button_clicked_cb (GtkWidget *button, gpointer user_data)
{
	AirvpnEditor *self = AIRVPN_EDITOR (user_data);
	AirvpnEditorPrivate *priv = AIRVPN_EDITOR_GET_PRIVATE (self);
	GtkWidget *api_key_entry;
	const char *api_key;
	GTask *task;

	if (priv->devices_cancellable)
		return;

	api_key_entry = GTK_WIDGET (gtk_builder_get_object (priv->builder, "api_key_entry"));
	api_key = gtk_editable_get_text (GTK_EDITABLE (api_key_entry));
	if (!api_key || !api_key[0]) {
		gtk_widget_set_tooltip_text (button,
		                             _("Enter your API key first (see the link above)."));
		return;
	}

	gtk_widget_set_sensitive_compat (button, FALSE);

	priv->devices_cancellable = g_cancellable_new ();
	task = g_task_new (NULL, priv->devices_cancellable, devices_done_cb, self);
	g_task_set_source_tag (task, devices_button_clicked_cb);
	g_task_set_task_data (task, g_strdup (api_key), g_free);
	g_task_run_in_thread (task, devices_thread_cb);
	g_object_unref (task);
}

/*****************************************************************************/
/* "Apply & Reconnect": save the edited settings straight to the remote
 * connection and, when the connection is currently active, bounce it so
 * the new server takes effect with one click. */

static gboolean update_connection (NMVpnEditor *iface, NMConnection *connection, GError **error);

typedef struct {
	AirvpnEditor *self;          /* strong reference */
	NMClient *client;
	NMRemoteConnection *remote;
	guint poll_id;
	guint poll_count;
} ReconnectCtx;

static void
reconnect_ctx_finish (ReconnectCtx *ctx, const char *error_msg)
{
	AirvpnEditorPrivate *priv = AIRVPN_EDITOR_GET_PRIVATE (ctx->self);
	GtkWidget *button;

	button = priv->builder
	         ? GTK_WIDGET (gtk_builder_get_object (priv->builder, "reconnect_button"))
	         : NULL;
	if (button) {
		gtk_widget_set_sensitive (button, TRUE);
		gtk_button_set_label (GTK_BUTTON (button),
		                      error_msg ? _("Failed — try again") : _("Appl_y & Reconnect"));
		gtk_button_set_use_underline (GTK_BUTTON (button), TRUE);
		if (error_msg)
			gtk_widget_set_tooltip_text (button, error_msg);
	}

	if (error_msg)
		g_warning ("AirVPN reconnect: %s", error_msg);

	priv->reconnect_busy = FALSE;
	g_clear_object (&priv->reconnect_cancellable);

	nm_clear_g_source (&ctx->poll_id);
	g_clear_object (&ctx->remote);
	g_clear_object (&ctx->client);
	g_object_unref (ctx->self);
	g_slice_free (ReconnectCtx, ctx);
}

static void
reconnect_set_label (ReconnectCtx *ctx, const char *label)
{
	AirvpnEditorPrivate *priv = AIRVPN_EDITOR_GET_PRIVATE (ctx->self);
	GtkWidget *button;

	button = priv->builder
	         ? GTK_WIDGET (gtk_builder_get_object (priv->builder, "reconnect_button"))
	         : NULL;
	if (button)
		gtk_button_set_label (GTK_BUTTON (button), label);
}

static NMActiveConnection *
reconnect_find_active (ReconnectCtx *ctx)
{
	AirvpnEditorPrivate *priv = AIRVPN_EDITOR_GET_PRIVATE (ctx->self);
	const GPtrArray *actives;
	guint i;

	actives = nm_client_get_active_connections (ctx->client);
	for (i = 0; actives && i < actives->len; i++) {
		NMActiveConnection *ac = actives->pdata[i];

		if (nm_streq0 (nm_active_connection_get_uuid (ac), priv->uuid))
			return ac;
	}
	return NULL;
}

static void
reconnect_activate_done (GObject *source_object, GAsyncResult *result, gpointer user_data)
{
	ReconnectCtx *ctx = user_data;
	gs_free_error GError *error = NULL;
	NMActiveConnection *ac;

	ac = nm_client_activate_connection_finish (NM_CLIENT (source_object), result, &error);
	if (!ac) {
		reconnect_ctx_finish (ctx,
		                      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)
		                          ? NULL : error->message);
		return;
	}
	g_object_unref (ac);
	reconnect_ctx_finish (ctx, NULL);
}

static gboolean
reconnect_poll_deactivated (gpointer user_data)
{
	ReconnectCtx *ctx = user_data;
	AirvpnEditorPrivate *priv = AIRVPN_EDITOR_GET_PRIVATE (ctx->self);

	if (reconnect_find_active (ctx)) {
		if (++ctx->poll_count > 20) {
			ctx->poll_id = 0;
			reconnect_ctx_finish (ctx, _("The connection did not deactivate in time."));
			return FALSE;
		}
		return TRUE;
	}

	ctx->poll_id = 0;
	reconnect_set_label (ctx, _("Reconnecting…"));
	nm_client_activate_connection_async (ctx->client,
	                                     NM_CONNECTION (ctx->remote),
	                                     NULL, NULL,
	                                     priv->reconnect_cancellable,
	                                     reconnect_activate_done, ctx);
	return FALSE;
}

static void
reconnect_deactivate_done (GObject *source_object, GAsyncResult *result, gpointer user_data)
{
	ReconnectCtx *ctx = user_data;
	gs_free_error GError *error = NULL;

	if (!nm_client_deactivate_connection_finish (NM_CLIENT (source_object), result, &error)) {
		reconnect_ctx_finish (ctx,
		                      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)
		                          ? NULL : error->message);
		return;
	}

	/* Wait for the active connection to disappear before re-activating. */
	ctx->poll_count = 0;
	ctx->poll_id = g_timeout_add (500, reconnect_poll_deactivated, ctx);
}

static void
reconnect_commit_done (GObject *source_object, GAsyncResult *result, gpointer user_data)
{
	ReconnectCtx *ctx = user_data;
	AirvpnEditorPrivate *priv = AIRVPN_EDITOR_GET_PRIVATE (ctx->self);
	gs_free_error GError *error = NULL;
	NMActiveConnection *active;

	if (!nm_remote_connection_commit_changes_finish (NM_REMOTE_CONNECTION (source_object),
	                                                 result, &error)) {
		reconnect_ctx_finish (ctx,
		                      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)
		                          ? NULL : error->message);
		return;
	}

	active = reconnect_find_active (ctx);
	if (!active) {
		/* Not active: saving was all there is to do. */
		reconnect_ctx_finish (ctx, NULL);
		return;
	}

	reconnect_set_label (ctx, _("Disconnecting…"));
	nm_client_deactivate_connection_async (ctx->client, active,
	                                       priv->reconnect_cancellable,
	                                       reconnect_deactivate_done, ctx);
}

static void
reconnect_client_ready (GObject *source_object, GAsyncResult *result, gpointer user_data)
{
	ReconnectCtx *ctx = user_data;
	AirvpnEditorPrivate *priv = AIRVPN_EDITOR_GET_PRIVATE (ctx->self);
	gs_free_error GError *error = NULL;
	gs_unref_object NMConnection *dup = NULL;

	ctx->client = nm_client_new_finish (result, &error);
	if (!ctx->client) {
		reconnect_ctx_finish (ctx,
		                      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)
		                          ? NULL : error->message);
		return;
	}

	ctx->remote = nm_client_get_connection_by_uuid (ctx->client, priv->uuid);
	if (!ctx->remote) {
		reconnect_ctx_finish (ctx, _("Save the connection once before using Apply & Reconnect."));
		return;
	}
	g_object_ref (ctx->remote);

	/* Fill the current widget values into a copy, then push them to the
	 * remote connection. */
	dup = nm_simple_connection_new_clone (NM_CONNECTION (ctx->remote));
	if (!update_connection (NM_VPN_EDITOR (ctx->self), dup, &error)) {
		reconnect_ctx_finish (ctx, error->message);
		return;
	}
	nm_connection_replace_settings_from_connection (NM_CONNECTION (ctx->remote), dup);

	reconnect_set_label (ctx, _("Saving…"));
	nm_remote_connection_commit_changes_async (ctx->remote, TRUE,
	                                           priv->reconnect_cancellable,
	                                           reconnect_commit_done, ctx);
}

static void
reconnect_button_clicked_cb (GtkWidget *button, gpointer user_data)
{
	AirvpnEditor *self = AIRVPN_EDITOR (user_data);
	AirvpnEditorPrivate *priv = AIRVPN_EDITOR_GET_PRIVATE (self);
	ReconnectCtx *ctx;

	if (priv->reconnect_busy)
		return;

	if (!priv->uuid || !priv->uuid[0]) {
		gtk_widget_set_tooltip_text (button,
		                             _("Save the connection once before using Apply & Reconnect."));
		return;
	}

	priv->reconnect_busy = TRUE;
	priv->reconnect_cancellable = g_cancellable_new ();
	gtk_widget_set_sensitive (button, FALSE);
	gtk_button_set_label (GTK_BUTTON (button), _("Connecting to NetworkManager…"));

	ctx = g_slice_new0 (ReconnectCtx);
	ctx->self = g_object_ref (self);

	nm_client_new_async (priv->reconnect_cancellable, reconnect_client_ready, ctx);
}

/*****************************************************************************/

static gboolean
init_editor_plugin (AirvpnEditor *self, NMConnection *connection, GError **error)
{
	AirvpnEditorPrivate *priv = AIRVPN_EDITOR_GET_PRIVATE (self);
	NMSettingVpn *s_vpn;
	GtkWidget *widget;
	const char *value;
	gs_free char *snapshot = NULL;
	gsize snapshot_len = 0;
	NMSettingSecretFlags pw_flags = NM_SETTING_SECRET_FLAG_NONE;

	s_vpn = (NMSettingVpn *) nm_connection_get_setting (connection, NM_TYPE_SETTING_VPN);

	/* API key */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "api_key_entry"));
	g_return_val_if_fail (widget, FALSE);
	if (s_vpn) {
		value = nm_setting_vpn_get_secret (s_vpn, NM_AIRVPN_KEY_API_KEY);
		gtk_editable_set_text (GTK_EDITABLE (widget), value ?: "");
	}
	g_signal_connect (widget, "changed", G_CALLBACK (stuff_changed_cb), self);

	nma_utils_setup_password_storage (widget, 0, (NMSetting *) s_vpn, NM_AIRVPN_KEY_API_KEY,
	                                  TRUE, FALSE);
	if (s_vpn)
		nm_setting_get_secret_flags (NM_SETTING (s_vpn), NM_AIRVPN_KEY_API_KEY, &pw_flags, NULL);
	g_signal_connect (widget, "notify::secondary-icon-name",
	                  G_CALLBACK (password_storage_changed_cb), self);

	/* Device selection */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "device_combo"));
	g_return_val_if_fail (widget, FALSE);
	{
		GtkWidget *entry = combo_get_entry (widget);

		g_return_val_if_fail (entry, FALSE);
		if (s_vpn) {
			value = nm_setting_vpn_get_data_item (s_vpn, NM_AIRVPN_KEY_DEVICE);
			if (value && value[0])
				gtk_editable_set_text (GTK_EDITABLE (entry), value);
		}
		g_signal_connect (entry, "changed", G_CALLBACK (stuff_changed_cb), self);
	}

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "devices_button"));
	g_return_val_if_fail (widget, FALSE);
	g_signal_connect (widget, "clicked", G_CALLBACK (devices_button_clicked_cb), self);

	/* Server selection */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "server_combo"));
	g_return_val_if_fail (widget, FALSE);
	value = s_vpn ? nm_setting_vpn_get_data_item (s_vpn, NM_AIRVPN_KEY_SERVER) : NULL;
	snapshot = nm_airvpn_server_list_load_snapshot (&snapshot_len);
	if (snapshot) {
		gs_free_error GError *local = NULL;

		if (!nm_airvpn_server_list_fill_combo (GTK_COMBO_BOX_TEXT (widget),
		                                       snapshot, snapshot_len, value, &local))
			g_warning ("Could not parse the bundled server list: %s", local->message);
	}
	g_signal_connect (widget, "changed", G_CALLBACK (stuff_changed_cb), self);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "refresh_button"));
	g_return_val_if_fail (widget, FALSE);
	g_signal_connect (widget, "clicked", G_CALLBACK (refresh_button_clicked_cb), self);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "reconnect_button"));
	g_return_val_if_fail (widget, FALSE);
	g_signal_connect (widget, "clicked", G_CALLBACK (reconnect_button_clicked_cb), self);

	/* Protocol */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "protocol_combo"));
	g_return_val_if_fail (widget, FALSE);
	value = s_vpn ? nm_setting_vpn_get_data_item (s_vpn, NM_AIRVPN_KEY_PROTOCOL) : NULL;
	if (!gtk_combo_box_set_active_id (GTK_COMBO_BOX (widget), value ?: NM_AIRVPN_PROTOCOL_UDP))
		gtk_combo_box_set_active_id (GTK_COMBO_BOX (widget), NM_AIRVPN_PROTOCOL_UDP);
	g_signal_connect (widget, "changed", G_CALLBACK (stuff_changed_cb), self);

	/* Port */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "port_combo"));
	g_return_val_if_fail (widget, FALSE);
	value = s_vpn ? nm_setting_vpn_get_data_item (s_vpn, NM_AIRVPN_KEY_PORT) : NULL;
	if (value && value[0]) {
		if (!gtk_combo_box_set_active_id (GTK_COMBO_BOX (widget), value)) {
			gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (widget), value, value);
			gtk_combo_box_set_active_id (GTK_COMBO_BOX (widget), value);
		}
	} else
		gtk_combo_box_set_active_id (GTK_COMBO_BOX (widget), NM_AIRVPN_DEFAULT_PORT);
	g_signal_connect (widget, "changed", G_CALLBACK (stuff_changed_cb), self);

	/* Keepalive (Advanced) */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "keepalive_check"));
	g_return_val_if_fail (widget, FALSE);
	value = s_vpn ? nm_setting_vpn_get_data_item (s_vpn, NM_AIRVPN_KEY_KEEPALIVE) : NULL;
	check_set_active (widget, !value || strcmp (value, "no"));
	g_signal_connect (widget, "toggled", G_CALLBACK (keepalive_toggled_cb), self);
	keepalive_toggled_cb (widget, self);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "keepalive_interval_spin"));
	g_return_val_if_fail (widget, FALSE);
	value = s_vpn ? nm_setting_vpn_get_data_item (s_vpn, NM_AIRVPN_KEY_PING_INTERVAL) : NULL;
	gtk_spin_button_set_value (GTK_SPIN_BUTTON (widget), value && value[0] ? strtol (value, NULL, 10) : 10);
	g_signal_connect (widget, "value-changed", G_CALLBACK (stuff_changed_cb), self);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "keepalive_restart_spin"));
	g_return_val_if_fail (widget, FALSE);
	value = s_vpn ? nm_setting_vpn_get_data_item (s_vpn, NM_AIRVPN_KEY_PING_RESTART) : NULL;
	gtk_spin_button_set_value (GTK_SPIN_BUTTON (widget), value && value[0] ? strtol (value, NULL, 10) : 60);
	g_signal_connect (widget, "value-changed", G_CALLBACK (stuff_changed_cb), self);

	/* Kill switch (Advanced) */
	{
		GtkWidget *kill_switch_widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "kill_switch_check"));
		GtkWidget *unavailable_label = GTK_WIDGET (gtk_builder_get_object (priv->builder, "kill_switch_unavailable_label"));
		gs_free char *nft_path = NULL;
		gboolean nft_available;

		g_return_val_if_fail (kill_switch_widget, FALSE);
		g_return_val_if_fail (unavailable_label, FALSE);

		nft_path = g_find_program_in_path ("nft");
		nft_available = nft_path != NULL;

		value = s_vpn ? nm_setting_vpn_get_data_item (s_vpn, NM_AIRVPN_KEY_KILL_SWITCH) : NULL;
		check_set_active (kill_switch_widget,
		                  nft_available && value && !strcmp (value, "yes"));
		gtk_widget_set_sensitive_compat (kill_switch_widget, nft_available);
		gtk_widget_set_visible (unavailable_label, !nft_available);
		g_signal_connect (kill_switch_widget, "toggled", G_CALLBACK (kill_switch_toggled_cb), self);
		kill_switch_toggled_cb (kill_switch_widget, self);

		widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "allow_lan_check"));
		g_return_val_if_fail (widget, FALSE);
		value = s_vpn ? nm_setting_vpn_get_data_item (s_vpn, NM_AIRVPN_KEY_ALLOW_LAN) : NULL;
		check_set_active (widget, value && !strcmp (value, "yes"));
		g_signal_connect (widget, "toggled", G_CALLBACK (stuff_changed_cb), self);
	}

	/* Custom directives. The frame cannot be set in the .ui file: GTK3
	 * wants shadow-type, GTK4 wants has-frame, and gtk4-builder-tool
	 * does not convert between them. */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "directives_scroll"));
	g_return_val_if_fail (widget, FALSE);
#if GTK_CHECK_VERSION(4,0,0)
	gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (widget), TRUE);
#else
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (widget), GTK_SHADOW_IN);
#endif

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "directives_textview"));
	g_return_val_if_fail (widget, FALSE);
	if (s_vpn) {
		value = nm_setting_vpn_get_data_item (s_vpn, NM_AIRVPN_KEY_CUSTOM_DIRECTIVES);
		if (value && value[0]) {
			GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (widget));

			gtk_text_buffer_set_text (buffer, value, -1);
		}
	}
	g_signal_connect (gtk_text_view_get_buffer (GTK_TEXT_VIEW (widget)), "changed",
	                  G_CALLBACK (stuff_changed_cb), self);

	return TRUE;
}

static GObject *
get_widget (NMVpnEditor *iface)
{
	AirvpnEditor *self = AIRVPN_EDITOR (iface);
	AirvpnEditorPrivate *priv = AIRVPN_EDITOR_GET_PRIVATE (self);

	return G_OBJECT (priv->widget);
}

static gboolean
update_connection (NMVpnEditor *iface,
                   NMConnection *connection,
                   GError **error)
{
	AirvpnEditor *self = AIRVPN_EDITOR (iface);
	AirvpnEditorPrivate *priv = AIRVPN_EDITOR_GET_PRIVATE (self);
	NMSettingVpn *s_vpn;
	NMSettingSecretFlags flags;
	GtkWidget *widget;
	GtkTextBuffer *buffer;
	GtkTextIter start, end;
	const char *str;
	gs_free char *directives = NULL;

	s_vpn = NM_SETTING_VPN (nm_setting_vpn_new ());
	g_object_set (s_vpn, NM_SETTING_VPN_SERVICE_TYPE, NM_DBUS_SERVICE_AIRVPN, NULL);

	/* Server */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "server_combo"));
	str = gtk_combo_box_get_active_id (GTK_COMBO_BOX (widget));
	if (str && str[0])
		nm_setting_vpn_add_data_item (s_vpn, NM_AIRVPN_KEY_SERVER, str);

	/* Device */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "device_combo"));
	str = gtk_editable_get_text (GTK_EDITABLE (combo_get_entry (widget)));
	if (str && str[0])
		nm_setting_vpn_add_data_item (s_vpn, NM_AIRVPN_KEY_DEVICE, str);

	/* Protocol */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "protocol_combo"));
	str = gtk_combo_box_get_active_id (GTK_COMBO_BOX (widget));
	if (str && str[0])
		nm_setting_vpn_add_data_item (s_vpn, NM_AIRVPN_KEY_PROTOCOL, str);

	/* Port */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "port_combo"));
	str = gtk_combo_box_get_active_id (GTK_COMBO_BOX (widget));
	if (str && str[0])
		nm_setting_vpn_add_data_item (s_vpn, NM_AIRVPN_KEY_PORT, str);

	/* Keepalive */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "keepalive_check"));
	if (check_get_active (widget)) {
		GtkWidget *interval_widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "keepalive_interval_spin"));
		GtkWidget *restart_widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "keepalive_restart_spin"));
		gs_free char *interval_str = g_strdup_printf ("%d", gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (interval_widget)));
		gs_free char *restart_str = g_strdup_printf ("%d", gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (restart_widget)));

		nm_setting_vpn_add_data_item (s_vpn, NM_AIRVPN_KEY_PING_INTERVAL, interval_str);
		nm_setting_vpn_add_data_item (s_vpn, NM_AIRVPN_KEY_PING_RESTART, restart_str);
	} else
		nm_setting_vpn_add_data_item (s_vpn, NM_AIRVPN_KEY_KEEPALIVE, "no");

	/* Kill switch */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "kill_switch_check"));
	if (check_get_active (widget)) {
		GtkWidget *allow_lan_widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "allow_lan_check"));

		nm_setting_vpn_add_data_item (s_vpn, NM_AIRVPN_KEY_KILL_SWITCH, "yes");
		if (check_get_active (allow_lan_widget))
			nm_setting_vpn_add_data_item (s_vpn, NM_AIRVPN_KEY_ALLOW_LAN, "yes");
	}

	/* Custom directives */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "directives_textview"));
	buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (widget));
	gtk_text_buffer_get_bounds (buffer, &start, &end);
	directives = gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
	if (directives && directives[0])
		nm_setting_vpn_add_data_item (s_vpn, NM_AIRVPN_KEY_CUSTOM_DIRECTIVES, directives);

	/* API key and its storage flags */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "api_key_entry"));
	flags = nma_utils_menu_to_secret_flags (widget);
	switch (flags) {
	case NM_SETTING_SECRET_FLAG_NONE:
	case NM_SETTING_SECRET_FLAG_AGENT_OWNED:
		str = gtk_editable_get_text (GTK_EDITABLE (widget));
		if (str && str[0])
			nm_setting_vpn_add_secret (s_vpn, NM_AIRVPN_KEY_API_KEY, str);
		break;
	default:
		break;
	}
	nm_setting_set_secret_flags (NM_SETTING (s_vpn), NM_AIRVPN_KEY_API_KEY, flags, NULL);

	if (!nm_airvpn_properties_validate (s_vpn, error)) {
		g_object_unref (s_vpn);
		return FALSE;
	}

	nm_connection_add_setting (connection, NM_SETTING (s_vpn));

	return TRUE;
}

static void
is_new_func (const char *key, const char *value, gpointer user_data)
{
	gboolean *is_new = user_data;

	/* If there are any VPN data items the connection isn't new */
	*is_new = FALSE;
}

NMVpnEditor *
nm_airvpn_editor_new (NMConnection *connection, GError **error)
{
	NMVpnEditor *object;
	AirvpnEditorPrivate *priv;
	gboolean new = TRUE;
	NMSettingVpn *s_vpn;

	if (error)
		g_return_val_if_fail (*error == NULL, NULL);

	object = g_object_new (AIRVPN_TYPE_EDITOR, NULL);
	if (!object) {
		g_set_error (error, NMV_EDITOR_PLUGIN_ERROR, 0, "could not create airvpn object");
		return NULL;
	}

	priv = AIRVPN_EDITOR_GET_PRIVATE (object);

	priv->builder = gtk_builder_new ();

	gtk_builder_set_translation_domain (priv->builder, GETTEXT_PACKAGE);

	if (!gtk_builder_add_from_resource (priv->builder, "/org/freedesktop/network-manager-airvpn/nm-airvpn-dialog.ui", error)) {
		g_object_unref (object);
		g_return_val_if_reached (NULL);
	}

	priv->widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "airvpn_grid"));
	if (!priv->widget) {
		g_set_error (error, NMV_EDITOR_PLUGIN_ERROR, 0, "could not load UI widget");
		g_object_unref (object);
		return NULL;
	}
	g_object_ref_sink (priv->widget);

	priv->uuid = g_strdup (nm_connection_get_uuid (connection));

	s_vpn = nm_connection_get_setting_vpn (connection);
	if (s_vpn)
		nm_setting_vpn_foreach_data_item (s_vpn, is_new_func, &new);
	priv->new_connection = new;

	if (!init_editor_plugin (AIRVPN_EDITOR (object), connection, error)) {
		g_object_unref (object);
		return NULL;
	}

	return object;
}

static void
dispose (GObject *object)
{
	AirvpnEditor *plugin = AIRVPN_EDITOR (object);
	AirvpnEditorPrivate *priv = AIRVPN_EDITOR_GET_PRIVATE (plugin);
	GtkWidget *widget;

	if (priv->refresh_cancellable) {
		g_cancellable_cancel (priv->refresh_cancellable);
		g_clear_object (&priv->refresh_cancellable);
	}

	if (priv->devices_cancellable) {
		g_cancellable_cancel (priv->devices_cancellable);
		g_clear_object (&priv->devices_cancellable);
	}

	if (priv->reconnect_cancellable)
		g_cancellable_cancel (priv->reconnect_cancellable);

	g_clear_pointer (&priv->uuid, g_free);

	if (priv->builder) {
		widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "api_key_entry"));
		if (widget) {
			g_signal_handlers_disconnect_by_func (G_OBJECT (widget),
			                                      (GCallback) password_storage_changed_cb,
			                                      plugin);
		}
	}

	g_clear_object (&priv->widget);
	g_clear_object (&priv->builder);

	G_OBJECT_CLASS (airvpn_editor_parent_class)->dispose (object);
}

static void
airvpn_editor_init (AirvpnEditor *plugin)
{
}

static void
airvpn_editor_class_init (AirvpnEditorClass *req_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (req_class);

	object_class->dispose = dispose;
}

static void
airvpn_editor_interface_init (NMVpnEditorInterface *iface_class)
{
	iface_class->get_widget = get_widget;
	iface_class->update_connection = update_connection;
}

/*****************************************************************************/

#include "nm-airvpn-editor-plugin.h"

G_MODULE_EXPORT NMVpnEditor *
nm_vpn_editor_factory_airvpn (NMVpnEditorPlugin *editor_plugin,
                              NMConnection *connection,
                              GError **error)
{
	g_return_val_if_fail (!error || !*error, NULL);

	return nm_airvpn_editor_new (connection, error);
}
