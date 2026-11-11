#define _GNU_SOURCE
#include "plugin_mgr.h"
#include "http_util.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

static int is_safe_plugin_id(const char *id) {
    if (!id || !id[0] || strlen(id) > 63) return 0;
    if (strstr(id, "..") || strchr(id, '/') || strchr(id, '\\')) return 0;
    for (const char *p = id; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) {
            return 0;
        }
    }
    return 1;
}

static pid_t read_plugin_pid(const char *plugin_id) {
    if (!is_safe_plugin_id(plugin_id)) return -1;
    char pid_path[128];
    snprintf(pid_path, sizeof(pid_path), "%s/%s.pid", HARMONY_PLUGINS_RUN_DIR, plugin_id);
    FILE *f = fopen(pid_path, "r");
    if (!f) return -1;
    long pid = -1;
    if (fscanf(f, "%ld", &pid) != 1 || pid <= 1) pid = -1;
    fclose(f);
    return (pid_t)pid;
}

int plugin_mgr_is_running(const char *plugin_id) {
    pid_t pid = read_plugin_pid(plugin_id);
    if (pid <= 1) return 0;
    return (kill(pid, 0) == 0);
}

static int is_plugin_enabled(const char *plugin_id) {
    if (!is_safe_plugin_id(plugin_id)) return 0;
    char dis_path[256];
    snprintf(dis_path, sizeof(dis_path), "%s/%s/disabled", HARMONY_PLUGINS_DIR, plugin_id);
    return (access(dis_path, F_OK) != 0);
}

int plugin_mgr_set_enabled(const char *plugin_id, int enabled) {
    if (!is_safe_plugin_id(plugin_id)) return -1;
    char dis_path[256];
    snprintf(dis_path, sizeof(dis_path), "%s/%s/disabled", HARMONY_PLUGINS_DIR, plugin_id);
    if (enabled) {
        unlink(dis_path);
        return plugin_mgr_start(plugin_id);
    } else {
        int fd = open(dis_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) close(fd);
        return plugin_mgr_stop(plugin_id);
    }
}

int plugin_mgr_start(const char *plugin_id) {
    if (!is_safe_plugin_id(plugin_id)) return -1;
    if (plugin_mgr_is_running(plugin_id)) return 0;

    char plug_dir[256];
    snprintf(plug_dir, sizeof(plug_dir), "%s/%s", HARMONY_PLUGINS_DIR, plugin_id);
    if (access(plug_dir, F_OK) != 0) return -1;

    mkdir(HARMONY_PLUGINS_RUN_DIR, 0755);

    char run_sh[256];
    snprintf(run_sh, sizeof(run_sh), "%s/run.sh", plug_dir);

    char cmd[512];
    if (access(run_sh, X_OK) == 0 || access(run_sh, F_OK) == 0) {
        snprintf(cmd, sizeof(cmd),
                 "export PATH=/cache/bin:/data/harmony/bin:$PATH; "
                 "export HARMONY_PLUGIN_ID=\"%s\"; "
                 "export HARMONY_SOCKET=\"%s/%s.sock\"; "
                 "cd \"%s\" && (sh ./run.sh start > /tmp/plugins/%s.log 2>&1 &)",
                 plugin_id, HARMONY_PLUGINS_RUN_DIR, plugin_id,
                 plug_dir, plugin_id);
        system(cmd);
    } else {
        /* Fallback: look for entrypoint in manifest.json */
        char mpath[256];
        snprintf(mpath, sizeof(mpath), "%s/manifest.json", plug_dir);
        size_t flen = 0;
        char *mtxt = read_file_text_alloc(mpath, 32768, &flen);
        if (!mtxt) return -1;
        cJSON *root = cJSON_Parse(mtxt);
        free(mtxt);
        if (!root) return -1;
        cJSON *j_ep = cJSON_GetObjectItemCaseSensitive(root, "entrypoint");
        const char *ep = (j_ep && cJSON_IsString(j_ep)) ? j_ep->valuestring : NULL;
        if (ep && ep[0]) {
            snprintf(cmd, sizeof(cmd),
                     "export PATH=/cache/bin:/data/harmony/bin:$PATH; "
                     "export HARMONY_PLUGIN_ID=\"%s\"; "
                     "export HARMONY_SOCKET=\"%s/%s.sock\"; "
                     "cd \"%s\" && (\"./%s\" > /tmp/plugins/%s.log 2>&1 & echo $! > /tmp/plugins/%s.pid)",
                     plugin_id, HARMONY_PLUGINS_RUN_DIR, plugin_id,
                     plug_dir, ep, plugin_id, plugin_id);
            system(cmd);
        }
        cJSON_Delete(root);
    }

    usleep(150000);
    return plugin_mgr_is_running(plugin_id) ? 0 : -1;
}

int plugin_mgr_stop(const char *plugin_id) {
    if (!is_safe_plugin_id(plugin_id)) return -1;

    char plug_dir[256];
    snprintf(plug_dir, sizeof(plug_dir), "%s/%s", HARMONY_PLUGINS_DIR, plugin_id);
    char run_sh[256];
    snprintf(run_sh, sizeof(run_sh), "%s/run.sh", plug_dir);

    if (access(run_sh, F_OK) == 0) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "export PATH=/cache/bin:/data/harmony/bin:$PATH; "
                 "cd \"%s\" && sh ./run.sh stop > /dev/null 2>&1",
                 plug_dir);
        system(cmd);
    }

    pid_t pid = read_plugin_pid(plugin_id);
    if (pid > 1 && kill(pid, 0) == 0) {
        kill(pid, SIGTERM);
        usleep(250000);
        if (kill(pid, 0) == 0) {
            kill(pid, SIGKILL);
        }
    }

    char pid_path[128], sock_path[128];
    snprintf(pid_path, sizeof(pid_path), "%s/%s.pid", HARMONY_PLUGINS_RUN_DIR, plugin_id);
    snprintf(sock_path, sizeof(sock_path), "%s/%s.sock", HARMONY_PLUGINS_RUN_DIR, plugin_id);
    unlink(pid_path);
    unlink(sock_path);
    return 0;
}

int plugin_mgr_restart(const char *plugin_id) {
    plugin_mgr_stop(plugin_id);
    usleep(200000);
    return plugin_mgr_start(plugin_id);
}

int plugin_mgr_uninstall(const char *plugin_id) {
    if (!is_safe_plugin_id(plugin_id)) return -1;
    plugin_mgr_stop(plugin_id);

    char plug_dir[256];
    snprintf(plug_dir, sizeof(plug_dir), "%s/%s", HARMONY_PLUGINS_DIR, plugin_id);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", plug_dir);
    system(cmd);
    return 0;
}

int plugin_mgr_init(void) {
    mkdir(HARMONY_PLUGINS_RUN_DIR, 0755);
    mkdir(HARMONY_PLUGINS_DIR, 0755);

    DIR *d = opendir(HARMONY_PLUGINS_DIR);
    if (!d) return 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (!is_safe_plugin_id(de->d_name)) continue;
        if (is_plugin_enabled(de->d_name)) {
            plugin_mgr_start(de->d_name);
        }
    }
    closedir(d);
    return 0;
}

void plugin_mgr_shutdown(void) {
    DIR *d = opendir(HARMONY_PLUGINS_DIR);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (!is_safe_plugin_id(de->d_name)) continue;
        plugin_mgr_stop(de->d_name);
    }
    closedir(d);
}

void plugin_mgr_watchdog_tick(void) {
    DIR *d = opendir(HARMONY_PLUGINS_DIR);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (!is_safe_plugin_id(de->d_name)) continue;
        if (is_plugin_enabled(de->d_name)) {
            if (!plugin_mgr_is_running(de->d_name)) {
                plugin_mgr_start(de->d_name);
            }
        }
    }
    closedir(d);
}

cJSON *plugin_mgr_get_installed_json(void) {
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;

    DIR *d = opendir(HARMONY_PLUGINS_DIR);
    if (!d) return arr;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (!is_safe_plugin_id(de->d_name)) continue;

        char mpath[256];
        snprintf(mpath, sizeof(mpath), "%s/%s/manifest.json", HARMONY_PLUGINS_DIR, de->d_name);
        size_t flen = 0;
        char *mtxt = read_file_text_alloc(mpath, 32768, &flen);
        if (!mtxt) continue;

        cJSON *manifest = cJSON_Parse(mtxt);
        free(mtxt);
        if (!manifest) continue;

        cJSON *p_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(p_obj, "id", de->d_name);

        cJSON *j_name = cJSON_GetObjectItemCaseSensitive(manifest, "name");
        cJSON_AddStringToObject(p_obj, "name", (j_name && cJSON_IsString(j_name)) ? j_name->valuestring : de->d_name);

        cJSON *j_ver = cJSON_GetObjectItemCaseSensitive(manifest, "version");
        cJSON_AddStringToObject(p_obj, "version", (j_ver && cJSON_IsString(j_ver)) ? j_ver->valuestring : "1.0.0");

        cJSON *j_desc = cJSON_GetObjectItemCaseSensitive(manifest, "description");
        if (j_desc && cJSON_IsString(j_desc)) cJSON_AddStringToObject(p_obj, "description", j_desc->valuestring);

        int enabled = is_plugin_enabled(de->d_name);
        cJSON_AddBoolToObject(p_obj, "enabled", enabled);

        pid_t pid = read_plugin_pid(de->d_name);
        int running = (pid > 1 && kill(pid, 0) == 0);
        cJSON_AddBoolToObject(p_obj, "running", running);
        if (running) cJSON_AddNumberToObject(p_obj, "pid", (double)pid);

        cJSON *dev_def = cJSON_GetObjectItemCaseSensitive(manifest, "device_definition");
        if (dev_def) {
            cJSON_AddItemReferenceToObject(p_obj, "device_definition", dev_def);
        }
        cJSON_AddItemToObject(p_obj, "manifest", manifest);

        cJSON_AddItemToArray(arr, p_obj);
    }
    closedir(d);
    return arr;
}

int plugin_mgr_install_archive(const char *archive_path, char *err_msg, size_t err_msg_sz) {
    if (!archive_path || access(archive_path, R_OK) != 0) {
        if (err_msg) snprintf(err_msg, err_msg_sz, "Plugin archive not readable");
        return -1;
    }

    char tmp_unpack[128];
    snprintf(tmp_unpack, sizeof(tmp_unpack), "/tmp/p_unpack_%ld", (long)time(NULL));
    mkdir(tmp_unpack, 0755);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "export PATH=/cache/bin:/data/harmony/bin:$PATH; "
             "tar -xzf \"%s\" -C \"%s\" 2>/dev/null",
             archive_path, tmp_unpack);
    int rc = system(cmd);
    if (rc != 0) {
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmp_unpack);
        system(cmd);
        if (err_msg) snprintf(err_msg, err_msg_sz, "Failed to unpack tar.gz archive");
        return -1;
    }

    char mpath[256];
    snprintf(mpath, sizeof(mpath), "%s/manifest.json", tmp_unpack);
    if (access(mpath, R_OK) != 0) {
        /* Check if subfolder has manifest */
        DIR *td = opendir(tmp_unpack);
        if (td) {
            struct dirent *tde;
            while ((tde = readdir(td)) != NULL) {
                if (tde->d_name[0] == '.') continue;
                char sub_m[256];
                snprintf(sub_m, sizeof(sub_m), "%s/%s/manifest.json", tmp_unpack, tde->d_name);
                if (access(sub_m, R_OK) == 0) {
                    snprintf(mpath, sizeof(mpath), "%s", sub_m);
                    break;
                }
            }
            closedir(td);
        }
    }

    size_t flen = 0;
    char *mtxt = read_file_text_alloc(mpath, 32768, &flen);
    if (!mtxt) {
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmp_unpack);
        system(cmd);
        if (err_msg) snprintf(err_msg, err_msg_sz, "Archive missing valid manifest.json");
        return -1;
    }

    cJSON *manifest = cJSON_Parse(mtxt);
    free(mtxt);
    if (!manifest) {
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmp_unpack);
        system(cmd);
        if (err_msg) snprintf(err_msg, err_msg_sz, "Invalid manifest.json JSON syntax");
        return -1;
    }

    cJSON *j_id = cJSON_GetObjectItemCaseSensitive(manifest, "id");
    if (!j_id || !cJSON_IsString(j_id) || !is_safe_plugin_id(j_id->valuestring)) {
        cJSON_Delete(manifest);
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmp_unpack);
        system(cmd);
        if (err_msg) snprintf(err_msg, err_msg_sz, "Manifest missing valid alphanumeric plugin id");
        return -1;
    }

    const char *plug_id = j_id->valuestring;
    plugin_mgr_stop(plug_id);

    char dest_dir[256];
    snprintf(dest_dir, sizeof(dest_dir), "%s/%s", HARMONY_PLUGINS_DIR, plug_id);
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", dest_dir);
    system(cmd);

    mkdir(HARMONY_PLUGINS_DIR, 0755);
    mkdir(dest_dir, 0755);

    /* Move unpacked files */
    char src_dir[256];
    char *last_slash = strrchr(mpath, '/');
    if (last_slash) *last_slash = '\0';
    snprintf(src_dir, sizeof(src_dir), "%s", mpath);

    snprintf(cmd, sizeof(cmd), "cp -a \"%s\"/* \"%s\"/ 2>/dev/null; chmod -R 755 \"%s\"", src_dir, dest_dir, dest_dir);
    system(cmd);

    /* Clean up temp dir */
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmp_unpack);
    system(cmd);
    cJSON_Delete(manifest);

    /* Start newly installed plugin */
    if (is_plugin_enabled(plug_id)) {
        plugin_mgr_start(plug_id);
    }

    return 0;
}

int plugin_mgr_install_from_url(const char *url, char *err_msg, size_t err_msg_sz) {
    if (!url || strncmp(url, "http", 4) != 0) {
        if (err_msg) snprintf(err_msg, err_msg_sz, "Invalid URL scheme");
        return -1;
    }

    char tmp_tar[128];
    snprintf(tmp_tar, sizeof(tmp_tar), "/tmp/plug_dl_%ld.tar.gz", (long)time(NULL));

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "export PATH=/cache/bin:/data/harmony/bin:$PATH; "
             "wget --no-check-certificate -q -O \"%s\" \"%s\"",
             tmp_tar, url);
    int rc = system(cmd);
    if (rc != 0 || access(tmp_tar, R_OK) != 0) {
        unlink(tmp_tar);
        if (err_msg) snprintf(err_msg, err_msg_sz, "Failed to download archive from URL");
        return -1;
    }

    int res = plugin_mgr_install_archive(tmp_tar, err_msg, err_msg_sz);
    unlink(tmp_tar);
    return res;
}

cJSON *plugin_mgr_get_repositories(void) {
    size_t flen = 0;
    char *txt = read_file_text_alloc(HARMONY_REPOSITORIES_FILE, 32768, &flen);
    cJSON *root = txt ? cJSON_Parse(txt) : NULL;
    if (txt) free(txt);

    if (root && cJSON_IsArray(root)) return root;
    if (root) cJSON_Delete(root);

    cJSON *def = cJSON_CreateArray();
    cJSON_AddItemToArray(def, cJSON_CreateString(HARMONY_DEFAULT_REPO_URL));
    return def;
}

static int save_repositories(cJSON *arr) {
    if (!arr) return -1;
    char *out = cJSON_Print(arr);
    if (!out) return -1;
    mkdir("/data/harmony/config", 0755);
    int ret = write_file_atomic_sized(HARMONY_REPOSITORIES_FILE, out, strlen(out));
    free(out);
    return ret;
}

int plugin_mgr_add_repository(const char *url) {
    if (!url || strncmp(url, "http", 4) != 0) return -1;
    cJSON *repos = plugin_mgr_get_repositories();
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, repos) {
        if (it->valuestring && strcmp(it->valuestring, url) == 0) {
            cJSON_Delete(repos);
            return 0; /* already present */
        }
    }
    cJSON_AddItemToArray(repos, cJSON_CreateString(url));
    int res = save_repositories(repos);
    cJSON_Delete(repos);
    return res;
}

int plugin_mgr_remove_repository(const char *url) {
    if (!url) return -1;
    cJSON *repos = plugin_mgr_get_repositories();
    cJSON *new_repos = cJSON_CreateArray();
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, repos) {
        if (it->valuestring && strcmp(it->valuestring, url) != 0) {
            cJSON_AddItemToArray(new_repos, cJSON_CreateString(it->valuestring));
        }
    }
    cJSON_Delete(repos);
    int res = save_repositories(new_repos);
    cJSON_Delete(new_repos);
    return res;
}

cJSON *plugin_mgr_get_marketplace_catalog(void) {
    const char *cache_path = "/tmp/plugins/marketplace_cache.json";
    struct stat st;
    if (stat(cache_path, &st) == 0 && (time(NULL) - st.st_mtime) < 600) {
        size_t flen = 0;
        char *cached = read_file_text_alloc(cache_path, 65536, &flen);
        if (cached) {
            cJSON *c_root = cJSON_Parse(cached);
            free(cached);
            if (c_root) return c_root;
        }
    }

    cJSON *repos = plugin_mgr_get_repositories();
    cJSON *catalog = cJSON_CreateArray();

    cJSON *repo_url = NULL;
    cJSON_ArrayForEach(repo_url, repos) {
        if (!repo_url->valuestring) continue;
        char tmp_repo[128];
        snprintf(tmp_repo, sizeof(tmp_repo), "/tmp/plugins/repo_tmp_%ld.json", (long)time(NULL));

        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "export PATH=/cache/bin:/data/harmony/bin:$PATH; "
                 "wget --no-check-certificate -q -T 5 -O \"%s\" \"%s\"",
                 tmp_repo, repo_url->valuestring);
        if (system(cmd) == 0 && access(tmp_repo, R_OK) == 0) {
            size_t rlen = 0;
            char *rtxt = read_file_text_alloc(tmp_repo, 65536, &rlen);
            if (rtxt) {
                cJSON *rjson = cJSON_Parse(rtxt);
                free(rtxt);
                if (rjson) {
                    cJSON *plugs = cJSON_GetObjectItemCaseSensitive(rjson, "plugins");
                    if (plugs && cJSON_IsArray(plugs)) {
                        cJSON *p = NULL;
                        cJSON_ArrayForEach(p, plugs) {
                            cJSON_AddItemToArray(catalog, cJSON_Duplicate(p, 1));
                        }
                    }
                    cJSON_Delete(rjson);
                }
            }
        }
        unlink(tmp_repo);
    }
    cJSON_Delete(repos);

    char *cat_str = cJSON_Print(catalog);
    if (cat_str) {
        mkdir("/tmp/plugins", 0755);
        int fd = open(cache_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            write(fd, cat_str, strlen(cat_str));
            close(fd);
        }
        free(cat_str);
    }

    return catalog;
}
