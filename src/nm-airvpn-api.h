/* nm-airvpn-service - AirVPN integration with NetworkManager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __NM_AIRVPN_API_H__
#define __NM_AIRVPN_API_H__

#include <gio/gio.h>

typedef struct {
	char *server;             /* AirVPN token: public_name, country code, continent or "earth" */
	char *device;             /* device name, NULL/empty for the account default */
	char *protocol;           /* "udp" or "tcp" */
	char *port;               /* e.g. "443" */
	char *custom_directives;  /* extra openvpn directives, newline separated, or NULL */
} NMAirvpnGeneratorParams;

void nm_airvpn_generator_fetch_async (const NMAirvpnGeneratorParams *params,
                                      const char *api_key,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data);

/* Returns the .ovpn config text (caller owns), or NULL and @error. */
char *nm_airvpn_generator_fetch_finish (GAsyncResult *result, GError **error);

/* Synchronous variant, used by the test runner. The configured device
 * name is validated against the account first: the generator silently
 * falls back to the Default device for unknown names, so an unknown
 * name is reported as an error instead. */
char *nm_airvpn_generator_fetch_sync (const NMAirvpnGeneratorParams *params,
                                      const char *api_key,
                                      GCancellable *cancellable,
                                      GError **error);

#endif /* __NM_AIRVPN_API_H__ */
