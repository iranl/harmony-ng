#define _GNU_SOURCE
#include "plugin_mgr.h"
#include "http_util.h"
#include "config_store.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include <time.h>

static uint64_t compute_dir_size_bytes(const char *dir_path) {
    uint64_t total = 0;
    DIR *d = opendir(dir_path);
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char subpath[512];
        snprintf(subpath, sizeof(subpath), "%s/%s", dir_path, de->d_name);
        struct stat st;
        if (lstat(subpath, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                total += compute_dir_size_bytes(subpath);
            } else if (S_ISREG(st.st_mode)) {
                total += (uint64_t)st.st_size;
            }
        }
    }
    closedir(d);
    return total;
}

static void format_bytes(uint64_t bytes, char *out, size_t out_sz) {
    if (bytes < 1024) {
        snprintf(out, out_sz, "%llu B", (unsigned long long)bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(out, out_sz, "%.1f KB", (double)bytes / 1024.0);
    } else if (bytes < 1024ULL * 1024 * 1024) {
        snprintf(out, out_sz, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        snprintf(out, out_sz, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    }
}

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

static void delete_dir_recursive(const char *dir_path) {
    if (!dir_path || !dir_path[0]) return;
    DIR *d = opendir(dir_path);
    if (!d) {
        unlink(dir_path);
        rmdir(dir_path);
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char subpath[512];
        snprintf(subpath, sizeof(subpath), "%s/%s", dir_path, de->d_name);
        struct stat st;
        if (lstat(subpath, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                delete_dir_recursive(subpath);
            } else {
                unlink(subpath);
            }
        }
    }
    closedir(d);
    rmdir(dir_path);
}

static void copy_dir_recursive(const char *src_dir, const char *dst_dir) {
    if (!src_dir || !dst_dir) return;
    mkdir(dst_dir, 0755);
    DIR *d = opendir(src_dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char s_path[512], d_path[512];
        snprintf(s_path, sizeof(s_path), "%s/%s", src_dir, de->d_name);
        snprintf(d_path, sizeof(d_path), "%s/%s", dst_dir, de->d_name);
        struct stat st;
        if (lstat(s_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                copy_dir_recursive(s_path, d_path);
            } else if (S_ISREG(st.st_mode)) {
                FILE *sf = fopen(s_path, "rb");
                if (sf) {
                    FILE *df = fopen(d_path, "wb");
                    if (df) {
                        char buf[4096];
                        size_t n;
                        while ((n = fread(buf, 1, sizeof(buf), sf)) > 0) {
                            fwrite(buf, 1, n, df);
                        }
                        fclose(df);
                    }
                    fclose(sf);
                    mode_t m = (st.st_mode & 0777);
                    if (strstr(d_path, ".sh") != NULL) m |= 0755;
                    chmod(d_path, m);
                }
            }
        }
    }
    closedir(d);
}

int plugin_mgr_uninstall(const char *plugin_id) {
    if (!is_safe_plugin_id(plugin_id)) return -1;
    plugin_mgr_stop(plugin_id);

    char plug_dir[256];
    snprintf(plug_dir, sizeof(plug_dir), "%s/%s", HARMONY_PLUGINS_DIR, plugin_id);
    delete_dir_recursive(plug_dir);
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

        char plug_dir[256];
        snprintf(plug_dir, sizeof(plug_dir), "%s/%s", HARMONY_PLUGINS_DIR, de->d_name);
        uint64_t sz_b = compute_dir_size_bytes(plug_dir);
        cJSON_AddNumberToObject(p_obj, "size_bytes", (double)sz_b);
        char sz_fmt[32];
        format_bytes(sz_b, sz_fmt, sizeof(sz_fmt));
        cJSON_AddStringToObject(p_obj, "size_formatted", sz_fmt);

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

    char plug_id[64] = {0};
    snprintf(plug_id, sizeof(plug_id), "%s", j_id->valuestring);
    plugin_mgr_stop(plug_id);

    char dest_dir[256];
    snprintf(dest_dir, sizeof(dest_dir), "%s/%s", HARMONY_PLUGINS_DIR, plug_id);
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", dest_dir);
    system(cmd);

    snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", dest_dir);
    system(cmd);

    /* Move unpacked files */
    char src_dir[256];
    char *last_slash = strrchr(mpath, '/');
    if (last_slash) *last_slash = '\0';
    snprintf(src_dir, sizeof(src_dir), "%s", mpath);

    copy_dir_recursive(src_dir, dest_dir);
    snprintf(cmd, sizeof(cmd), "chmod -R 755 \"%s\"", dest_dir);
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

int plugin_mgr_install_dev_raw(const char *url, char *err_msg, size_t err_msg_sz) {
    if (!url || strncmp(url, "http", 4) != 0) {
        if (err_msg) snprintf(err_msg, err_msg_sz, "Invalid URL scheme");
        return -1;
    }

    char manifest_url[512] = {0};
    char base_url[512] = {0};

    size_t ulen = strlen(url);
    if (ulen > sizeof(manifest_url) - 32) {
        if (err_msg) snprintf(err_msg, err_msg_sz, "URL too long");
        return -1;
    }

    if (strstr(url, "manifest.json") != NULL) {
        snprintf(manifest_url, sizeof(manifest_url), "%s", url);
        snprintf(base_url, sizeof(base_url), "%s", url);
        char *slash = strrchr(base_url, '/');
        if (slash) *slash = '\0';
    } else {
        if (url[ulen - 1] == '/') {
            snprintf(manifest_url, sizeof(manifest_url), "%smanifest.json", url);
            snprintf(base_url, sizeof(base_url), "%s", url);
            base_url[ulen - 1] = '\0';
        } else {
            snprintf(manifest_url, sizeof(manifest_url), "%s/manifest.json", url);
            snprintf(base_url, sizeof(base_url), "%s", url);
        }
    }

    char tmp_dev[128];
    snprintf(tmp_dev, sizeof(tmp_dev), "/tmp/p_dev_%ld", (long)time(NULL));
    mkdir(tmp_dev, 0755);

    /* 1. Download manifest.json */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "export PATH=/cache/bin:/data/harmony/bin:$PATH; "
             "wget --no-check-certificate -q -O \"%s/manifest.json\" \"%s\"",
             tmp_dev, manifest_url);
    int rc = system(cmd);

    char mpath[256];
    snprintf(mpath, sizeof(mpath), "%s/manifest.json", tmp_dev);
    if (rc != 0 || access(mpath, R_OK) != 0) {
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmp_dev);
        system(cmd);
        if (err_msg) snprintf(err_msg, err_msg_sz, "Failed to download manifest.json from URL");
        return -1;
    }

    size_t flen = 0;
    char *mtxt = read_file_text_alloc(mpath, 32768, &flen);
    if (!mtxt || flen < 2) {
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmp_dev);
        system(cmd);
        if (mtxt) free(mtxt);
        if (err_msg) snprintf(err_msg, err_msg_sz, "Downloaded manifest.json is empty");
        return -1;
    }

    cJSON *manifest = cJSON_Parse(mtxt);
    free(mtxt);
    if (!manifest) {
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmp_dev);
        system(cmd);
        if (err_msg) snprintf(err_msg, err_msg_sz, "Invalid manifest.json syntax");
        return -1;
    }

    cJSON *j_id = cJSON_GetObjectItemCaseSensitive(manifest, "id");
    if (!j_id || !cJSON_IsString(j_id) || !is_safe_plugin_id(j_id->valuestring)) {
        cJSON_Delete(manifest);
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmp_dev);
        system(cmd);
        if (err_msg) snprintf(err_msg, err_msg_sz, "Manifest missing valid alphanumeric plugin id");
        return -1;
    }

    char plug_id[64] = {0};
    snprintf(plug_id, sizeof(plug_id), "%s", j_id->valuestring);

    /* 2. Download files declared in manifest */
    cJSON *j_files = cJSON_GetObjectItemCaseSensitive(manifest, "files");
    if (!j_files || !cJSON_IsArray(j_files)) {
        j_files = cJSON_GetObjectItemCaseSensitive(manifest, "dev_files");
    }

    if (j_files && cJSON_IsArray(j_files) && cJSON_GetArraySize(j_files) > 0) {
        cJSON *f_item = NULL;
        cJSON_ArrayForEach(f_item, j_files) {
            if (!f_item || !cJSON_IsString(f_item) || !f_item->valuestring[0]) continue;
            const char *fname = f_item->valuestring;
            if (strcmp(fname, "manifest.json") == 0) continue;
            if (strstr(fname, "..") || fname[0] == '/') continue;

            char local_path[384];
            snprintf(local_path, sizeof(local_path), "%s/%s", tmp_dev, fname);
            char *last_slash = strrchr(local_path, '/');
            if (last_slash) {
                *last_slash = '\0';
                snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", local_path);
                system(cmd);
                *last_slash = '/';
            }

            snprintf(cmd, sizeof(cmd),
                     "export PATH=/cache/bin:/data/harmony/bin:$PATH; "
                     "wget --no-check-certificate -q -O \"%s\" \"%s/%s\"",
                     local_path, base_url, fname);
            system(cmd);
            if (strstr(fname, ".sh") != NULL) {
                chmod(local_path, 0755);
            }
        }
    } else {
        cJSON *j_entry = cJSON_GetObjectItemCaseSensitive(manifest, "entrypoint");
        const char *entry = (j_entry && cJSON_IsString(j_entry)) ? j_entry->valuestring : NULL;
        if (!entry) {
            cJSON *j_drv = cJSON_GetObjectItemCaseSensitive(manifest, "driver");
            entry = (j_drv && cJSON_IsString(j_drv)) ? j_drv->valuestring : NULL;
        }

        const char *defaults[4] = { "run.sh", NULL, NULL, NULL };
        int d_count = 1;
        if (entry && strcmp(entry, "run.sh") != 0 && strcmp(entry, "manifest.json") != 0 && !strstr(entry, "..") && entry[0] != '/') {
            defaults[d_count++] = entry;
        }

        for (int i = 0; i < d_count; i++) {
            const char *fname = defaults[i];
            char local_path[384];
            snprintf(local_path, sizeof(local_path), "%s/%s", tmp_dev, fname);
            snprintf(cmd, sizeof(cmd),
                     "export PATH=/cache/bin:/data/harmony/bin:$PATH; "
                     "wget --no-check-certificate -q -O \"%s\" \"%s/%s\"",
                     local_path, base_url, fname);
            system(cmd);
            if (strstr(fname, ".sh") != NULL || (entry && strcmp(fname, entry) == 0)) {
                chmod(local_path, 0755);
            }
        }
    }

    cJSON_Delete(manifest);

    /* 3. Stop running plugin, install into /data/harmony/plugins/<plug_id> */
    plugin_mgr_stop(plug_id);

    char dest_dir[256];
    snprintf(dest_dir, sizeof(dest_dir), "%s/%s", HARMONY_PLUGINS_DIR, plug_id);
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", dest_dir);
    system(cmd);

    snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", dest_dir);
    system(cmd);

    copy_dir_recursive(tmp_dev, dest_dir);
    snprintf(cmd, sizeof(cmd), "chmod -R 755 \"%s\"", dest_dir);
    system(cmd);

    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmp_dev);
    system(cmd);

    if (is_plugin_enabled(plug_id)) {
        plugin_mgr_start(plug_id);
    }

    return 0;
}

int plugin_mgr_pull(const char *plugin_id, char *err_msg, size_t err_msg_sz) {
    if (!plugin_id || !is_safe_plugin_id(plugin_id)) {
        if (err_msg) snprintf(err_msg, err_msg_sz, "Invalid plugin ID");
        return -1;
    }

    /* Build raw github manifest URL for this plugin */
    char raw_url[512];
    snprintf(raw_url, sizeof(raw_url), "https://raw.githubusercontent.com/iranl/harmony-ng-plugins/main/%s/manifest.json", plugin_id);
    return plugin_mgr_install_dev_raw(raw_url, err_msg, err_msg_sz);
}

int plugin_mgr_resolve_device_plugin(const char *type_or_plugin, char *out_id, size_t out_sz) {
    if (!type_or_plugin || !type_or_plugin[0]) return 0;

    const char *cand = type_or_plugin;
    if (strncasecmp(cand, "plugin:", 7) == 0) {
        cand += 7;
    }

    /* 1. Direct match on plugin directory name if safe id */
    if (is_safe_plugin_id(cand)) {
        char mpath[256];
        snprintf(mpath, sizeof(mpath), "%s/%s/manifest.json", HARMONY_PLUGINS_DIR, cand);
        if (access(mpath, F_OK) == 0) {
            if (out_id && out_sz > 0) snprintf(out_id, out_sz, "%s", cand);
            return 1;
        }
    }

    /* 2. Search installed plugins manifests for device_definition matching device_type or display_name */
    DIR *d = opendir(HARMONY_PLUGINS_DIR);
    if (!d) return 0;

    struct dirent *de;
    int found = 0;
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

        cJSON *dev_def = cJSON_GetObjectItemCaseSensitive(manifest, "device_definition");
        if (dev_def && cJSON_IsObject(dev_def)) {
            cJSON *j_dtype = cJSON_GetObjectItemCaseSensitive(dev_def, "device_type");
            cJSON *j_disp = cJSON_GetObjectItemCaseSensitive(dev_def, "display_name");
            const char *dt = (j_dtype && cJSON_IsString(j_dtype)) ? j_dtype->valuestring : NULL;
            const char *dn = (j_disp && cJSON_IsString(j_disp)) ? j_disp->valuestring : NULL;

            if ((dt && strcasecmp(dt, type_or_plugin) == 0) ||
                (dt && strcasecmp(dt, cand) == 0) ||
                (dn && strcasecmp(dn, type_or_plugin) == 0) ||
                (dn && strcasecmp(dn, cand) == 0)) {
                if (out_id && out_sz > 0) snprintf(out_id, out_sz, "%s", de->d_name);
                found = 1;
                cJSON_Delete(manifest);
                break;
            }
        }
        cJSON_Delete(manifest);
    }
    closedir(d);
    return found;
}

cJSON *plugin_mgr_get_device_definition(const char *plugin_id) {
    if (!plugin_id || !is_safe_plugin_id(plugin_id)) return NULL;

    char mpath[256];
    snprintf(mpath, sizeof(mpath), "%s/%s/manifest.json", HARMONY_PLUGINS_DIR, plugin_id);
    size_t flen = 0;
    char *mtxt = read_file_text_alloc(mpath, 32768, &flen);
    if (!mtxt) return NULL;

    cJSON *manifest = cJSON_Parse(mtxt);
    free(mtxt);
    if (!manifest) return NULL;

    cJSON *dev_def = cJSON_GetObjectItemCaseSensitive(manifest, "device_definition");
    cJSON *copy = dev_def ? cJSON_Duplicate(dev_def, 1) : NULL;
    cJSON_Delete(manifest);
    return copy;
}

int plugin_mgr_is_in_use_by_device(const char *plugin_id, char *device_name_out, size_t dev_name_sz) {
    if (!plugin_id || !plugin_id[0]) return 0;
    if (device_name_out && dev_name_sz > 0) device_name_out[0] = '\0';

    cJSON *root = config_store_devices_copy();
    if (!root) return 0;

    int in_use = 0;
    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (devs && cJSON_IsArray(devs)) {
        cJSON *dev = NULL;
        cJSON_ArrayForEach(dev, devs) {
            cJSON *j_plug = cJSON_GetObjectItemCaseSensitive(dev, "plugin");
            cJSON *j_type = cJSON_GetObjectItemCaseSensitive(dev, "type");
            const char *t_str = (j_type && cJSON_IsString(j_type)) ? j_type->valuestring : "";
            const char *p_str = (j_plug && cJSON_IsString(j_plug)) ? j_plug->valuestring : "";

            int match = 0;
            char res_id[64] = {0};
            if (p_str[0] && strcasecmp(p_str, plugin_id) == 0) match = 1;
            else if (strncasecmp(t_str, "plugin:", 7) == 0 && strcasecmp(t_str + 7, plugin_id) == 0) match = 1;
            else if (strcasecmp(t_str, "plugin") == 0 && p_str[0] && strcasecmp(p_str, plugin_id) == 0) match = 1;
            else if (t_str[0] && plugin_mgr_resolve_device_plugin(t_str, res_id, sizeof(res_id)) && strcasecmp(res_id, plugin_id) == 0) match = 1;

            if (!match) {
                cJSON *cmds = cJSON_GetObjectItemCaseSensitive(dev, "commands");
                if (cmds && cJSON_IsArray(cmds)) {
                    cJSON *cmd = NULL;
                    cJSON_ArrayForEach(cmd, cmds) {
                        cJSON *raw = cJSON_GetObjectItemCaseSensitive(cmd, "raw");
                        cJSON *kc = cJSON_GetObjectItemCaseSensitive(cmd, "keyCode");
                        const char *r_str = (raw && cJSON_IsString(raw)) ? raw->valuestring : "";
                        const char *k_str = (kc && cJSON_IsString(kc)) ? kc->valuestring : "";

                        char prefix[128];
                        snprintf(prefix, sizeof(prefix), "PLUGIN:%s:", plugin_id);
                        if (strncasecmp(r_str, prefix, strlen(prefix)) == 0 ||
                            strncasecmp(k_str, prefix, strlen(prefix)) == 0) {
                            match = 1;
                            break;
                        }
                    }
                }
            }

            if (match) {
                in_use = 1;
                if (device_name_out && dev_name_sz > 0) {
                    cJSON *j_name = cJSON_GetObjectItemCaseSensitive(dev, "name");
                    const char *n_str = (j_name && cJSON_IsString(j_name) && j_name->valuestring[0]) ?
                                        j_name->valuestring : NULL;
                    if (!n_str) {
                        cJSON *j_id = cJSON_GetObjectItemCaseSensitive(dev, "id");
                        n_str = (j_id && cJSON_IsString(j_id)) ? j_id->valuestring : "Device";
                    }
                    snprintf(device_name_out, dev_name_sz, "%s", n_str);
                }
                break;
            }
        }
    }

    cJSON_Delete(root);
    return in_use;
}

cJSON *plugin_mgr_get_storage_json(void) {
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    struct statvfs vfs;
    const char *check_path = HARMONY_PLUGINS_DIR;
    if (statvfs(check_path, &vfs) != 0) {
        check_path = "/data";
        if (statvfs(check_path, &vfs) != 0) {
            check_path = "/";
            if (statvfs(check_path, &vfs) != 0) {
                memset(&vfs, 0, sizeof(vfs));
            }
        }
    }

    uint64_t total_bytes = (uint64_t)vfs.f_blocks * vfs.f_frsize;
    uint64_t free_bytes = (uint64_t)vfs.f_bavail * vfs.f_frsize;
    uint64_t used_bytes = (total_bytes >= free_bytes) ? (total_bytes - free_bytes) : 0;

    cJSON_AddNumberToObject(obj, "total_bytes", (double)total_bytes);
    cJSON_AddNumberToObject(obj, "free_bytes", (double)free_bytes);
    cJSON_AddNumberToObject(obj, "used_bytes", (double)used_bytes);

    char fmt[32];
    format_bytes(total_bytes, fmt, sizeof(fmt));
    cJSON_AddStringToObject(obj, "total_formatted", fmt);
    format_bytes(free_bytes, fmt, sizeof(fmt));
    cJSON_AddStringToObject(obj, "free_formatted", fmt);
    format_bytes(used_bytes, fmt, sizeof(fmt));
    cJSON_AddStringToObject(obj, "used_formatted", fmt);

    return obj;
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
