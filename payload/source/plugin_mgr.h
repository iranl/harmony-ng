#ifndef HARMONY_PLUGIN_MGR_H
#define HARMONY_PLUGIN_MGR_H

#include "cJSON.h"
#include <stddef.h>
#include <string.h>
#include <strings.h>

/* Where plugins and their state live; a host test sets its own folder (-DHARMONY_PLUGIN_ROOT) */
#ifndef HARMONY_PLUGIN_ROOT
#define HARMONY_PLUGIN_ROOT         "/data/harmony"
#endif
#define HARMONY_PLUGINS_DIR         HARMONY_PLUGIN_ROOT "/plugins"
/* sockets, logs, markers (RAM); a host test sets its own (-DHARMONY_PLUGINS_RUN_DIR) */
#ifndef HARMONY_PLUGINS_RUN_DIR
#define HARMONY_PLUGINS_RUN_DIR     "/tmp/plugins"
#endif
/* Per-plugin state kept across updates (passed as HARMONY_PLUGIN_DATA) */
#define HARMONY_PLUGINS_DATA_DIR    HARMONY_PLUGIN_ROOT "/plugin-data"
#define HARMONY_REPOSITORIES_FILE   HARMONY_PLUGIN_ROOT "/config/repositories.json"
/* Shared libraries for plugins (libbearssl.so.0), passed as LD_LIBRARY_PATH (no rpath on uClibc 0.9.30) */
#define HARMONY_PLUGIN_LIB_DIR      HARMONY_PLUGIN_ROOT "/lib"
#define HARMONY_DEFAULT_REPO_URL    "https://raw.githubusercontent.com/iranl/harmony-ng-plugins/main/repository.json"

/* Error codes besides -1: another install is running (HTTP 409), no such plugin (404) */
#define PLUGIN_ERR_BUSY      (-3)
#define PLUGIN_ERR_NOT_FOUND (-4)
/* The archive could not be downloaded (network): HTTP 502, the browser may try instead */
#define PLUGIN_ERR_DOWNLOAD  (-5)

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize plugin manager: creates /tmp/plugins, kills plugin processes left by an earlier
   hub_daemon, starts enabled plugins */
int plugin_mgr_init(void);

/* Stop every plugin this process started (SIGTERM to its process group, SIGKILL after 1 s) */
void plugin_mgr_shutdown(void);

/* Periodic watchdog: reaps exited plugins (waitpid on their pid) and restarts enabled ones */
void plugin_mgr_watchdog_tick(void);

/* Every second: SIGKILL to leftover plugin processes whose SIGTERM grace has passed, and the
   log size cap. Never waits. */
void plugin_mgr_tick(void);

/* Returns cJSON array of all installed plugins, their status, manifest, and device_definition */
cJSON *plugin_mgr_get_installed_json(void);

/* Check if a plugin is currently running */
int plugin_mgr_is_running(const char *plugin_id);

/* Start (fork and exec of the manifest's entrypoint, which stays in the foreground), stop or
   restart a plugin */
int plugin_mgr_start(const char *plugin_id);
int plugin_mgr_stop(const char *plugin_id);
int plugin_mgr_restart(const char *plugin_id);

/* Enable or disable a plugin (disabled = no autostart) */
int plugin_mgr_set_enabled(const char *plugin_id, int enabled);

/* Uninstall plugin: stop, remove its directory, its plugin-data and its log.
   PLUGIN_ERR_NOT_FOUND when not installed, PLUGIN_ERR_BUSY during an install. */
int plugin_mgr_uninstall(const char *plugin_id);

/* Install a plugin from a .tar.gz archive path. When sha256_hex is given the archive must
   match it. The new version is copied next to the old one; on any error the old one stays. */
int plugin_mgr_install_archive(const char *archive_path, const char *sha256_hex, char *err_msg, size_t err_msg_sz);

/* Install a plugin by downloading from URL. sha256_hex (64 hex characters, or NULL to use the
   marketplace entry for this URL) must match the archive; without either the install fails. */
int plugin_mgr_install_from_url(const char *url, const char *sha256_hex, char *err_msg, size_t err_msg_sz);


/* Install a dev version of a plugin by downloading raw files from repository URL */
int plugin_mgr_install_dev_raw(const char *url, char *err_msg, size_t err_msg_sz);

/* Update an installed plugin from where it came from: its dev URL again, or the current
   marketplace entry of its id (checked against that entry's SHA-256) */
int plugin_mgr_pull(const char *plugin_id, char *err_msg, size_t err_msg_sz);


/* Retrieve cloned device_definition cJSON from an installed plugin. Caller frees with cJSON_Delete. Returns NULL if none. */
cJSON *plugin_mgr_get_device_definition(const char *plugin_id);
/* A copy of one top-level manifest item of an installed plugin (e.g. "pairing"), or NULL;
 * the caller deletes it */
cJSON *plugin_mgr_get_manifest_item(const char *plugin_id, const char *key);

/* Check if a plugin is currently in use by any configured device */
int plugin_mgr_is_in_use_by_device(const char *plugin_id, char *device_name_out, size_t dev_name_sz);

/* Get storage space metrics for the plugins partition (/data) */
cJSON *plugin_mgr_get_storage_json(void);

/* Get configured repositories as cJSON array */
cJSON *plugin_mgr_get_repositories(void);

/* Add or remove repository URL */
int plugin_mgr_add_repository(const char *url);
int plugin_mgr_remove_repository(const char *url);

/* Fetch and aggregate marketplace catalog from all repositories (cached 10 minutes unless
   force_reload) */
cJSON *plugin_mgr_get_marketplace_catalog(int force_reload);

#ifdef __cplusplus
}
#endif

/* 1 when id is a plugin id safe to put in a path (letters, digits, '-', '_', at most 63) */
static inline int plugin_mgr_is_safe_id(const char *id) {
    if (!id || !id[0] || strlen(id) > 63) return 0;
    for (const char *p = id; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) {
            return 0;
        }
    }
    return 1;
}

/* The plugin a device belongs to: its "plugin" field, or a type "plugin:<id>". Nothing else
   (a device's free-text type is never compared with plugin names). No file access: it runs in
   the key path. Writes the id to out_id; 1 when the device is a plugin device with a safe id. */
static inline int plugin_mgr_device_plugin_id(const cJSON *dev, char *out_id, size_t out_sz) {
    if (out_id && out_sz) out_id[0] = '\0';
    if (!dev) return 0;
    const cJSON *j_plug = cJSON_GetObjectItemCaseSensitive(dev, "plugin");
    const cJSON *j_type = cJSON_GetObjectItemCaseSensitive(dev, "type");
    const char *id = NULL;
    if (cJSON_IsString(j_plug) && j_plug->valuestring[0]) {
        id = j_plug->valuestring;
        if (strncasecmp(id, "plugin:", 7) == 0) id += 7;
    } else if (cJSON_IsString(j_type) && strncasecmp(j_type->valuestring, "plugin:", 7) == 0) {
        id = j_type->valuestring + 7;
    }
    if (!id || !plugin_mgr_is_safe_id(id)) return 0;
    if (out_id && out_sz) {
        size_t n = strlen(id);
        if (n >= out_sz) n = out_sz - 1;
        memcpy(out_id, id, n);
        out_id[n] = '\0';
    }
    return 1;
}

#endif /* HARMONY_PLUGIN_MGR_H */
