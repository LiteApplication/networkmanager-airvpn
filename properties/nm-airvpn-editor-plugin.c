/* networkmanager-airvpn - AirVPN integration with NetworkManager
 *
 * Service-side editor plugin: loaded by libnm/nmcli/nm-applet/
 * gnome-control-center to identify the connection type and to load the
 * GTK editor. Must not link against GTK itself.
 *
 * Based on nm-fortisslvpn-editor-plugin.c
 * Copyright (C) 2015 Lubomir Rintel <lkundrak@v3.sk>
 * Copyright (C) 2008 - 2011 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "nm-default.h"

#include "nm-airvpn-editor-plugin.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <gmodule.h>

#include "nm-utils/nm-vpn-plugin-utils.h"

#define AIRVPN_PLUGIN_NAME    _("AirVPN")
#define AIRVPN_PLUGIN_DESC    _("Connect to the AirVPN network using your API key.")
#define AIRVPN_PLUGIN_SERVICE NM_DBUS_SERVICE_AIRVPN

/*****************************************************************************/

enum {
	PROP_0,
	PROP_NAME,
	PROP_DESC,
	PROP_SERVICE
};

static void airvpn_editor_plugin_interface_init (NMVpnEditorPluginInterface *iface_class);

G_DEFINE_TYPE_EXTENDED (AirvpnEditorPlugin, airvpn_editor_plugin, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (NM_TYPE_VPN_EDITOR_PLUGIN,
                                               airvpn_editor_plugin_interface_init))

/*****************************************************************************/

static guint32
get_capabilities (NMVpnEditorPlugin *iface)
{
	return NM_VPN_EDITOR_PLUGIN_CAPABILITY_NONE;
}

static NMVpnEditor *
_call_editor_factory (gpointer factory,
                      NMVpnEditorPlugin *editor_plugin,
                      NMConnection *connection,
                      gpointer user_data,
                      GError **error)
{
	return ((NMVpnEditorFactory) factory) (editor_plugin,
	                                       connection,
	                                       error);
}

static NMVpnEditor *
get_editor (NMVpnEditorPlugin *iface, NMConnection *connection, GError **error)
{
	gpointer gtk3_only_symbol;
	GModule *self_module;
	const char *editor;

	g_return_val_if_fail (AIRVPN_IS_EDITOR_PLUGIN (iface), NULL);
	g_return_val_if_fail (NM_IS_CONNECTION (connection), NULL);
	g_return_val_if_fail (!error || !*error, NULL);

	self_module = g_module_open (NULL, 0);
	g_module_symbol (self_module, "gtk_container_add", &gtk3_only_symbol);
	g_module_close (self_module);

	if (gtk3_only_symbol) {
		editor = "libnm-vpn-plugin-airvpn-editor.so";
	} else {
		editor = "libnm-gtk4-vpn-plugin-airvpn-editor.so";
	}

	return nm_vpn_plugin_utils_load_editor (editor,
	                                        "nm_vpn_editor_factory_airvpn",
	                                        _call_editor_factory,
	                                        iface,
	                                        connection,
	                                        NULL,
	                                        error);
}

static void
get_property (GObject *object, guint prop_id,
              GValue *value, GParamSpec *pspec)
{
	switch (prop_id) {
	case PROP_NAME:
		g_value_set_string (value, AIRVPN_PLUGIN_NAME);
		break;
	case PROP_DESC:
		g_value_set_string (value, AIRVPN_PLUGIN_DESC);
		break;
	case PROP_SERVICE:
		g_value_set_string (value, AIRVPN_PLUGIN_SERVICE);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
airvpn_editor_plugin_init (AirvpnEditorPlugin *plugin)
{
}

static void
airvpn_editor_plugin_interface_init (NMVpnEditorPluginInterface *iface_class)
{
	iface_class->get_editor = get_editor;
	iface_class->get_capabilities = get_capabilities;
	iface_class->import_from_file = NULL;
	iface_class->export_to_file = NULL;
	iface_class->get_suggested_filename = NULL;
}

static void
airvpn_editor_plugin_class_init (AirvpnEditorPluginClass *req_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (req_class);

	object_class->get_property = get_property;

	g_object_class_override_property (object_class,
	                                  PROP_NAME,
	                                  NM_VPN_EDITOR_PLUGIN_NAME);

	g_object_class_override_property (object_class,
	                                  PROP_DESC,
	                                  NM_VPN_EDITOR_PLUGIN_DESCRIPTION);

	g_object_class_override_property (object_class,
	                                  PROP_SERVICE,
	                                  NM_VPN_EDITOR_PLUGIN_SERVICE);
}

G_MODULE_EXPORT NMVpnEditorPlugin *
nm_vpn_editor_plugin_factory (GError **error)
{
	if (error)
		g_return_val_if_fail (*error == NULL, NULL);

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

	return g_object_new (AIRVPN_TYPE_EDITOR_PLUGIN, NULL);
}
