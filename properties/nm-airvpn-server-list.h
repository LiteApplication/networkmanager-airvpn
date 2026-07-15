/* networkmanager-airvpn - AirVPN integration with NetworkManager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __NM_AIRVPN_SERVER_LIST_H__
#define __NM_AIRVPN_SERVER_LIST_H__

#include <gtk/gtk.h>

/* Fill @combo with server tokens parsed from AirVPN /api/status/ JSON:
 * "earth", the continents, the countries (by code) and every individual
 * server (by public name). @select_token, if non-NULL, is selected,
 * appended as a custom entry when unknown. */
gboolean nm_airvpn_server_list_fill_combo (GtkComboBoxText *combo,
                                           const char *json_text,
                                           gssize json_len,
                                           const char *select_token,
                                           GError **error);

/* Load the status.json snapshot bundled in the GResource. */
char *nm_airvpn_server_list_load_snapshot (gsize *len);

/* Blocking fetch of https://airvpn.org/api/status/ (no auth); run it in
 * a worker thread. Returns the JSON text. */
char *nm_airvpn_server_list_fetch (GError **error);

/* Blocking fetch of the account's device names (requires the API key);
 * run it in a worker thread. Returns a GStrv of names. */
char **nm_airvpn_device_list_fetch (const char *api_key, GError **error);

#endif /* __NM_AIRVPN_SERVER_LIST_H__ */
