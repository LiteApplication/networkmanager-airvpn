/* nm-airvpn-service - AirVPN integration with NetworkManager
 *
 * On-disk cache of generator-produced OpenVPN profiles. The profile
 * contains the device private key, so files are 0600 inside a 0700
 * per-connection directory under the root-owned state directory.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "nm-default.h"

#include "nm-airvpn-cache.h"

#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib/gstdio.h>

#define CONFIG_FILE_NAME  "config.ovpn"
#define META_FILE_NAME    "meta.ini"

#define META_GROUP           "airvpn-cache"
#define META_KEY_HASH        "params-hash"
#define META_KEY_FETCHED     "fetched"
#define META_KEY_STALE       "stale"

/* Device keys are valid for years, but refresh the profile eventually
 * in case AirVPN rotates entry addresses or directives. */
#define CACHE_MAX_AGE_SECONDS ((gint64) 90 * 24 * 3600)

static char *
cache_dir (const char *statedir, const char *uuid)
{
	return g_build_filename (statedir, uuid, NULL);
}

char *
nm_airvpn_cache_get_config_path (const char *statedir, const char *uuid)
{
	return g_build_filename (statedir, uuid, CONFIG_FILE_NAME, NULL);
}

static char *
params_hash (const NMAirvpnGeneratorParams *params)
{
	gs_free char *joined = NULL;

	joined = g_strdup_printf ("%s|%s|%s|%s|%s",
	                          params->server ?: "",
	                          params->device ?: "",
	                          params->protocol ?: "",
	                          params->port ?: "",
	                          params->custom_directives ?: "");
	return g_compute_checksum_for_string (G_CHECKSUM_SHA256, joined, -1);
}

static void
_nm_auto_keyfile_unref (GKeyFile **kf)
{
	if (*kf)
		g_key_file_unref (*kf);
}

static GKeyFile *
load_meta (const char *statedir, const char *uuid)
{
	gs_free char *dir = cache_dir (statedir, uuid);
	gs_free char *path = g_build_filename (dir, META_FILE_NAME, NULL);
	GKeyFile *kf;

	kf = g_key_file_new ();
	if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL)) {
		g_key_file_unref (kf);
		return NULL;
	}
	return kf;
}

static gboolean
check_cache (const char *statedir,
             const char *uuid,
             const NMAirvpnGeneratorParams *params,
             gboolean accept_stale)
{
	gs_free char *config_path = nm_airvpn_cache_get_config_path (statedir, uuid);
	gs_free char *hash = NULL;
	gs_free char *stored_hash = NULL;
	nm_auto(_nm_auto_keyfile_unref) GKeyFile *meta = NULL;
	gint64 fetched;

	if (!g_file_test (config_path, G_FILE_TEST_IS_REGULAR))
		return FALSE;

	meta = load_meta (statedir, uuid);
	if (!meta)
		return FALSE;

	stored_hash = g_key_file_get_string (meta, META_GROUP, META_KEY_HASH, NULL);
	hash = params_hash (params);
	if (!stored_hash || strcmp (stored_hash, hash) != 0)
		return FALSE;

	if (accept_stale)
		return TRUE;

	if (g_key_file_get_boolean (meta, META_GROUP, META_KEY_STALE, NULL))
		return FALSE;

	fetched = g_key_file_get_int64 (meta, META_GROUP, META_KEY_FETCHED, NULL);
	if (fetched <= 0 || g_get_real_time () / G_USEC_PER_SEC - fetched > CACHE_MAX_AGE_SECONDS)
		return FALSE;

	return TRUE;
}

gboolean
nm_airvpn_cache_is_valid (const char *statedir,
                          const char *uuid,
                          const NMAirvpnGeneratorParams *params)
{
	return check_cache (statedir, uuid, params, FALSE);
}

gboolean
nm_airvpn_cache_exists (const char *statedir,
                        const char *uuid,
                        const NMAirvpnGeneratorParams *params)
{
	return check_cache (statedir, uuid, params, TRUE);
}

static gboolean
write_file_0600 (const char *path, const char *contents, gssize len, GError **error)
{
	mode_t old_umask;
	gboolean success;

	old_umask = umask (0077);
	success = g_file_set_contents (path, contents, len, error);
	umask (old_umask);
	return success;
}

gboolean
nm_airvpn_cache_store (const char *statedir,
                       const char *uuid,
                       const NMAirvpnGeneratorParams *params,
                       const char *config,
                       GError **error)
{
	gs_free char *dir = cache_dir (statedir, uuid);
	gs_free char *config_path = nm_airvpn_cache_get_config_path (statedir, uuid);
	gs_free char *meta_path = g_build_filename (dir, META_FILE_NAME, NULL);
	gs_free char *hash = NULL;
	gs_free char *meta_data = NULL;
	nm_auto(_nm_auto_keyfile_unref) GKeyFile *meta = NULL;

	if (g_mkdir_with_parents (dir, 0700) != 0) {
		g_set_error (error,
		             G_FILE_ERROR,
		             g_file_error_from_errno (errno),
		             _("Could not create cache directory “%s”: %s"),
		             dir, g_strerror (errno));
		return FALSE;
	}

	if (!write_file_0600 (config_path, config, -1, error))
		return FALSE;

	meta = g_key_file_new ();
	hash = params_hash (params);
	g_key_file_set_string (meta, META_GROUP, META_KEY_HASH, hash);
	g_key_file_set_int64 (meta, META_GROUP, META_KEY_FETCHED,
	                      g_get_real_time () / G_USEC_PER_SEC);
	g_key_file_set_boolean (meta, META_GROUP, META_KEY_STALE, FALSE);

	meta_data = g_key_file_to_data (meta, NULL, NULL);
	return write_file_0600 (meta_path, meta_data, -1, error);
}

void
nm_airvpn_cache_mark_stale (const char *statedir, const char *uuid)
{
	gs_free char *dir = cache_dir (statedir, uuid);
	gs_free char *meta_path = g_build_filename (dir, META_FILE_NAME, NULL);
	gs_free char *meta_data = NULL;
	nm_auto(_nm_auto_keyfile_unref) GKeyFile *meta = NULL;

	meta = load_meta (statedir, uuid);
	if (!meta)
		return;

	g_key_file_set_boolean (meta, META_GROUP, META_KEY_STALE, TRUE);
	meta_data = g_key_file_to_data (meta, NULL, NULL);
	write_file_0600 (meta_path, meta_data, -1, NULL);
}
