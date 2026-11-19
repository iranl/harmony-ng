#ifndef HARMONY_PLUGIN_MGR_H
#define HARMONY_PLUGIN_MGR_H

#include "cJSON.h"
#include <stddef.h>

/* Where plugins and their state live; a host test sets its own folder (-DHARMONY_PLUGIN_ROOT) */
#ifndef HARMONY_PLUGIN_ROOT
#define HARMONY_PLUGIN_ROOT         "/data/harmony"
#endif
#define HARMONY_PLUGINS_DIR         HARMONY_PLUGIN_ROOT "/plugins"
#define HARMONY_PLUGINS_RUN_DIR     "/tmp/plugins"
/* Per-plugin state kept across updates (passed as HARMONY_PLUGIN_DATA) */
#define HARMONY_PLUGINS_DATA_DIR    HARMONY_PLUGIN_ROOT "/plugin-data"
#define HARMONY_REPOSITORIES_FILE   HARMONY_PLUGIN_ROOT "/config/repositories.json"
#define HARMONY_DEFAULT_REPO_URL    "https://raw.githubusercontent.com/iranl/harmony-ng-plugins/main/repository.json"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize plugin manager: creates /tmp/plugins, loads manifests, starts enabled plugins */
int plugin_mgr_init(void);

/* Cleanly shut down all running plugins (SIGTERM, then SIGKILL after 1s) */
void plugin_mgr_shutdown(void);

/* Periodic watchdog: checks enabled plugins and restarts dead processes */
void plugin_mgr_watchdog_tick(void);

/* Returns cJSON array of all installed plugins, their status, manifest, and device_definition */
cJSON *plugin_mgr_get_installed_json(void);

/* Check if a plugin is currently running */
int plugin_mgr_is_running(const char *plugin_id);

/* Start, stop, or restart a plugin daemon */
int plugin_mgr_start(const char *plugin_id);
int plugin_mgr_stop(const char *plugin_id);
int plugin_mgr_restart(const char *plugin_id);

/* Enable or disable a plugin (disabled = no autostart) */
int plugin_mgr_set_enabled(const char *plugin_id, int enabled);

/* Uninstall plugin: stop, remove directory and runtime files */
int plugin_mgr_uninstall(const char *plugin_id);

/* Install a plugin from a .tar.gz archive path */
int plugin_mgr_install_archive(const char *archive_path, char *err_msg, size_t err_msg_sz);

/* Install a plugin by downloading from URL. sha256_hex (64 hex characters, or NULL to use the
   marketplace entry for this URL, if it lists one) must match the archive. */
int plugin_mgr_install_from_url(const char *url, const char *sha256_hex, char *err_msg, size_t err_msg_sz);

/* 1 when id is a plugin id safe to put in a path (letters, digits, '-', '_') */
int plugin_mgr_is_safe_id(const char *id);

/* Install a dev version of a plugin by downloading raw files from repository URL */
int plugin_mgr_install_dev_raw(const char *url, char *err_msg, size_t err_msg_sz);

/* Pull committed files for an installed plugin directly from git repo without release */
int plugin_mgr_pull(const char *plugin_id, char *err_msg, size_t err_msg_sz);

/* Resolve whether type or plugin identifier corresponds to an installed plugin.
   Matches plugin_id, "plugin:<id>", or device_type / display_name from any installed plugin's device_definition.
   Writes resolved plugin_id to out_id (if not NULL) up to out_sz. Returns 1 if resolved, 0 otherwise. */
int plugin_mgr_resolve_device_plugin(const char *type_or_plugin, char *out_id, size_t out_sz);

/* Retrieve cloned device_definition cJSON from an installed plugin. Caller frees with cJSON_Delete. Returns NULL if none. */
cJSON *plugin_mgr_get_device_definition(const char *plugin_id);

/* Check if a plugin is currently in use by any configured device */
int plugin_mgr_is_in_use_by_device(const char *plugin_id, char *device_name_out, size_t dev_name_sz);

/* Get storage space metrics for the plugins partition (/data) */
cJSON *plugin_mgr_get_storage_json(void);

/* Get configured repositories as cJSON array */
cJSON *plugin_mgr_get_repositories(void);

/* Add or remove repository URL */
int plugin_mgr_add_repository(const char *url);
int plugin_mgr_remove_repository(const char *url);

/* Fetch and aggregate marketplace catalog from all repositories */
cJSON *plugin_mgr_get_marketplace_catalog(void);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_PLUGIN_MGR_H */
