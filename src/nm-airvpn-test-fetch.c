/* nm-airvpn-test-fetch - developer test runner for the API and cache modules.
 * Not installed.
 *
 * Usage: nm-airvpn-test-fetch <statedir> <uuid> <server> <protocol> <port> [device]
 * The API key is read from the AIRVPN_API_KEY environment variable.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "nm-default.h"

#include <stdio.h>
#include <stdlib.h>

#include "nm-airvpn-api.h"
#include "nm-airvpn-cache.h"

int
main (int argc, char *argv[])
{
	NMAirvpnGeneratorParams params = { 0 };
	GError *error = NULL;
	const char *statedir, *uuid, *api_key;
	gs_free char *config = NULL;
	gs_free char *config_path = NULL;

	if (argc < 6) {
		g_printerr ("usage: %s <statedir> <uuid> <server> <protocol> <port> [device]\n", argv[0]);
		return 2;
	}

	api_key = g_getenv ("AIRVPN_API_KEY");
	if (!api_key) {
		g_printerr ("AIRVPN_API_KEY is not set\n");
		return 2;
	}

	statedir = argv[1];
	uuid = argv[2];
	params.server = argv[3];
	params.protocol = argv[4];
	params.port = argv[5];
	params.device = argc > 6 ? argv[6] : NULL;

	if (nm_airvpn_cache_is_valid (statedir, uuid, &params)) {
		config_path = nm_airvpn_cache_get_config_path (statedir, uuid);
		g_print ("cache: valid (%s)\n", config_path);
		return 0;
	}

	g_print ("cache: miss/stale, fetching from generator...\n");
	config = nm_airvpn_generator_fetch_sync (&params, api_key, NULL, &error);
	if (!config) {
		g_printerr ("fetch failed: %s\n", error->message);
		return 1;
	}

	g_print ("fetched %zu bytes\n", strlen (config));

	if (!nm_airvpn_cache_store (statedir, uuid, &params, config, &error)) {
		g_printerr ("cache store failed: %s\n", error->message);
		return 1;
	}

	config_path = nm_airvpn_cache_get_config_path (statedir, uuid);
	g_print ("cache: stored at %s\n", config_path);
	g_print ("cache valid now: %d\n", nm_airvpn_cache_is_valid (statedir, uuid, &params));

	nm_airvpn_cache_mark_stale (statedir, uuid);
	g_print ("after mark_stale: valid=%d exists=%d\n",
	         nm_airvpn_cache_is_valid (statedir, uuid, &params),
	         nm_airvpn_cache_exists (statedir, uuid, &params));

	return 0;
}
