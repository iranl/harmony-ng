#ifndef HARMONY_PLUGIN_MGR_H
#define HARMONY_PLUGIN_MGR_H

#include "cJSON.h"
#include <stddef.h>

#define HARMONY_PLUGINS_DIR         "/data/harmony/plugins"
#define HARMONY_PLUGINS_RUN_DIR     "/tmp/plugins"
#define HARMONY_REPOSITORIES_FILE   "/data/harmony/config/repositories.json"
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

/* Install a plugin by downloading from URL */
int plugin_mgr_install_from_url(const char *url, char *err_msg, size_t err_msg_sz);

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
