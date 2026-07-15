/* networkmanager-airvpn - AirVPN integration with NetworkManager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __NM_AIRVPN_EDITOR_PLUGIN_H__
#define __NM_AIRVPN_EDITOR_PLUGIN_H__

#define AIRVPN_TYPE_EDITOR_PLUGIN            (airvpn_editor_plugin_get_type ())
#define AIRVPN_EDITOR_PLUGIN(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), AIRVPN_TYPE_EDITOR_PLUGIN, AirvpnEditorPlugin))
#define AIRVPN_EDITOR_PLUGIN_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), AIRVPN_TYPE_EDITOR_PLUGIN, AirvpnEditorPluginClass))
#define AIRVPN_IS_EDITOR_PLUGIN(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), AIRVPN_TYPE_EDITOR_PLUGIN))
#define AIRVPN_IS_EDITOR_PLUGIN_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), AIRVPN_TYPE_EDITOR_PLUGIN))
#define AIRVPN_EDITOR_PLUGIN_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), AIRVPN_TYPE_EDITOR_PLUGIN, AirvpnEditorPluginClass))

typedef struct _AirvpnEditorPlugin AirvpnEditorPlugin;
typedef struct _AirvpnEditorPluginClass AirvpnEditorPluginClass;

struct _AirvpnEditorPlugin {
	GObject parent;
};

struct _AirvpnEditorPluginClass {
	GObjectClass parent;
};

GType airvpn_editor_plugin_get_type (void);

typedef NMVpnEditor *(*NMVpnEditorFactory) (NMVpnEditorPlugin *editor_plugin,
                                            NMConnection *connection,
                                            GError **error);

NMVpnEditor *
nm_vpn_editor_factory_airvpn (NMVpnEditorPlugin *editor_plugin,
                              NMConnection *connection,
                              GError **error);

#endif /* __NM_AIRVPN_EDITOR_PLUGIN_H__ */
