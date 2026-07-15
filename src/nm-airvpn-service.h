/* nm-airvpn-service - AirVPN integration with NetworkManager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __NM_AIRVPN_SERVICE_H__
#define __NM_AIRVPN_SERVICE_H__

#include <glib.h>
#include <glib-object.h>

#include "nm-service-defines.h"

#define NM_TYPE_AIRVPN_PLUGIN            (nm_airvpn_plugin_get_type ())
#define NM_AIRVPN_PLUGIN(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), NM_TYPE_AIRVPN_PLUGIN, NMAirvpnPlugin))
#define NM_AIRVPN_PLUGIN_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), NM_TYPE_AIRVPN_PLUGIN, NMAirvpnPluginClass))
#define NM_IS_AIRVPN_PLUGIN(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), NM_TYPE_AIRVPN_PLUGIN))
#define NM_IS_AIRVPN_PLUGIN_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), NM_TYPE_AIRVPN_PLUGIN))
#define NM_AIRVPN_PLUGIN_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), NM_TYPE_AIRVPN_PLUGIN, NMAirvpnPluginClass))

typedef struct {
	NMVpnServicePlugin parent;
} NMAirvpnPlugin;

typedef struct {
	NMVpnServicePluginClass parent;
} NMAirvpnPluginClass;

GType nm_airvpn_plugin_get_type (void);

NMAirvpnPlugin *nm_airvpn_plugin_new (const char *bus_name);

#endif /* __NM_AIRVPN_SERVICE_H__ */
