/* nm-airvpn-service - AirVPN integration with NetworkManager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __NM_AIRVPN_PROPERTIES_H__
#define __NM_AIRVPN_PROPERTIES_H__

#include <NetworkManager.h>

gboolean nm_airvpn_properties_validate (NMSettingVpn *s_vpn, GError **error);
gboolean nm_airvpn_properties_validate_secrets (NMSettingVpn *s_vpn, GError **error);

#endif /* __NM_AIRVPN_PROPERTIES_H__ */
