/* nm-airvpn-service - AirVPN integration with NetworkManager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __NM_AIRVPN_CACHE_H__
#define __NM_AIRVPN_CACHE_H__

#include <glib.h>

#include "nm-airvpn-api.h"

/* On-disk cache of generator-produced OpenVPN profiles, one directory
 * per connection UUID under the state directory. */

char *nm_airvpn_cache_get_config_path (const char *statedir, const char *uuid);

/* TRUE if a cached config exists, matches @params and is neither stale
 * nor older than the maximum age. */
gboolean nm_airvpn_cache_is_valid (const char *statedir,
                                   const char *uuid,
                                   const NMAirvpnGeneratorParams *params);

/* TRUE if a cached config exists and matches @params, even if it is
 * marked stale (used as a fallback when the generator is unreachable). */
gboolean nm_airvpn_cache_exists (const char *statedir,
                                 const char *uuid,
                                 const NMAirvpnGeneratorParams *params);

gboolean nm_airvpn_cache_store (const char *statedir,
                                const char *uuid,
                                const NMAirvpnGeneratorParams *params,
                                const char *config,
                                GError **error);

/* Mark the cached config stale so the next connection attempt refreshes
 * it from the generator API. */
void nm_airvpn_cache_mark_stale (const char *statedir, const char *uuid);

#endif /* __NM_AIRVPN_CACHE_H__ */
