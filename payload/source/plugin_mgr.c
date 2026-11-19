#define _GNU_SOURCE
#include "plugin_mgr.h"
#include "http_util.h"
#include "config_store.h"
#include "cJSON.h"
#include "sha256.h"

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
#include <ctype.h>

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

int plugin_mgr_is_safe_id(const char *id) {
    return is_safe_plugin_id(id);
}

/* Download limits */
#define FETCH_ARCHIVE_MAX_BYTES (16L * 1024 * 1024)
#define FETCH_ARCHIVE_MAX_MS    120000
#define FETCH_FILE_MAX_BYTES    (8L * 1024 * 1024)
#define FETCH_SMALL_MAX_BYTES   (256L * 1024)
#define FETCH_SMALL_MAX_MS      20000

/* A URL handed to wget. URLs come from the network (repositories, manifests) and from API
 * callers: one used to go into a shell command line, where a quote or $( ) ran as root. */
static int is_safe_url(const char *url) {
    if (!url || (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) return 0;
    if (strlen(url) > 400) return 0;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
        if (isalnum(*p) || strchr("-._~:/?#@&+,=%", *p)) continue;
        return 0;
    }
    return 1;
}

/* A file path named in a manifest: relative, inside the plugin directory, plain characters */
static int is_safe_rel_path(const char *f) {
    if (!f || !f[0] || f[0] == '/' || strlen(f) > 200) return 0;
    if (strstr(f, "..") || strstr(f, "//")) return 0;
    for (const unsigned char *p = (const unsigned char *)f; *p; p++) {
        if (isalnum(*p) || *p == '.' || *p == '_' || *p == '-' || *p == '/') continue;
        return 0;
    }
    return 1;
}

/* wget without a shell; the output is removed when the download failed */
static int fetch_url(const char *url, const char *out_path, long max_bytes, unsigned int max_ms) {
    if (!is_safe_url(url)) return -1;
    /* busybox with its wget applet on the Hub; a plain wget elsewhere (host tests) */
    static const char *const tools[][2] = {
        { "/cache/bin/busybox", "wget" }, { "/data/harmony/bin/busybox", "wget" },
        { "/usr/bin/wget", NULL }, { "/bin/wget", NULL }
    };
    int rc = -1;
    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
        if (access(tools[i][0], X_OK) != 0) continue;
        char *argv[11];
        int a = 0;
        argv[a++] = (char *)tools[i][0];
        if (tools[i][1]) argv[a++] = (char *)tools[i][1];
        argv[a++] = "-q";
        argv[a++] = "--no-check-certificate";
        argv[a++] = "-T";
        argv[a++] = "10";
        argv[a++] = "-O";
        argv[a++] = (char *)out_path;
        argv[a++] = (char *)url;
        argv[a] = NULL;
        rc = run_argv_limited(argv, out_path, max_bytes, max_ms);
        if (rc == 0 || rc == RUN_TOO_BIG || rc == RUN_TOO_SLOW) break;
    }
    if (rc != 0) unlink(out_path);
    return rc;
}

static int file_sha256_hex(const char *path, char out_hex[65]) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) sha256_update(&ctx, buf, n);
    int err = ferror(f);
    fclose(f);
    if (err) return -1;
    uint8_t d[32];
    sha256_final(d, &ctx);
    for (int i = 0; i < 32; i++) snprintf(out_hex + 2 * i, 3, "%02x", d[i]);
    return 0;
}

/* Every entry of the archive is a plain file or directory under the unpack directory: a
 * "../" name, an absolute name or a link entry wrote outside it during the unpack */
static int archive_entries_safe(const char *archive_path, const char *list_path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "export PATH=/cache/bin:/data/harmony/bin:$PATH; tar -tvzf \"%s\" > \"%s\" 2>/dev/null",
             archive_path, list_path);
    int ok = (system(cmd) == 0);
    FILE *f = ok ? fopen(list_path, "r") : NULL;
    char line[1024];
    while (ok && f && fgets(line, sizeof(line), f)) {
        if (line[0] != '-' && line[0] != 'd') ok = 0;
    }
    if (f) fclose(f);
    if (ok) {
        snprintf(cmd, sizeof(cmd),
                 "export PATH=/cache/bin:/data/harmony/bin:$PATH; tar -tzf \"%s\" > \"%s\" 2>/dev/null",
                 archive_path, list_path);
        ok = (system(cmd) == 0);
        f = ok ? fopen(list_path, "r") : NULL;
        while (ok && f && fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            const char *name = line;
            while (name[0] == '.' && name[1] == '/') name += 2;
            if (name[0] == '/') ok = 0;
            for (const char *c = name; ok && *c; ) {
                size_t len = strcspn(c, "/");
                if (len == 2 && c[0] == '.' && c[1] == '.') ok = 0;
                c += len;
                if (*c == '/') c++;
            }
        }
        if (f) fclose(f);
    }
    unlink(list_path);
    return ok;
}

/* A temp path no other request uses: names from time(NULL) alone collided for two requests in
 * the same second (one removed the other's files) */
static void tmp_name(char *out, size_t out_sz, const char *prefix, const char *suffix) {
    static unsigned int s_seq = 0;
    unsigned int seq = __sync_fetch_and_add(&s_seq, 1);
    snprintf(out, out_sz, "%s_%ld_%d_%u%s", prefix, (long)time(NULL), (int)getpid(), seq, suffix);
}

/* /tmp/plugins/<id>.stopped: stopped by request (stop, disable, uninstall, shutdown). The
 * watchdog leaves such a plugin alone; it used to start it again within 10 s. */
static void stopped_marker_path(const char *plugin_id, char *out, size_t out_sz) {
    snprintf(out, out_sz, "%s/%s.stopped", HARMONY_PLUGINS_RUN_DIR, plugin_id);
}

static int is_stopped_by_request(const char *plugin_id) {
    char p[128];
    stopped_marker_path(plugin_id, p, sizeof(p));
    return access(p, F_OK) == 0;
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
    char marker[128];
    stopped_marker_path(plugin_id, marker, sizeof(marker));
    unlink(marker);

    /* State that survives an update: the plugin directory is replaced on every install */
    char data_dir[160];
    snprintf(data_dir, sizeof(data_dir), "%s/%s", HARMONY_PLUGINS_DATA_DIR, plugin_id);
    mkdir(HARMONY_PLUGINS_DATA_DIR, 0755);
    mkdir(data_dir, 0700);

    char run_sh[256];
    snprintf(run_sh, sizeof(run_sh), "%s/run.sh", plug_dir);

    char cmd[768];
    if (access(run_sh, X_OK) == 0 || access(run_sh, F_OK) == 0) {
        snprintf(cmd, sizeof(cmd),
                 "export PATH=/cache/bin:/data/harmony/bin:$PATH; "
                 "export HARMONY_PLUGIN_ID=\"%s\"; "
                 "export HARMONY_SOCKET=\"%s/%s.sock\"; "
                 "export HARMONY_PLUGIN_DATA=\"%s\"; "
                 "cd \"%s\" && (sh ./run.sh start > /tmp/plugins/%s.log 2>&1 &)",
                 plugin_id, HARMONY_PLUGINS_RUN_DIR, plugin_id, data_dir,
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
        if (ep && is_safe_rel_path(ep)) {
            snprintf(cmd, sizeof(cmd),
                     "export PATH=/cache/bin:/data/harmony/bin:$PATH; "
                     "export HARMONY_PLUGIN_ID=\"%s\"; "
                     "export HARMONY_SOCKET=\"%s/%s.sock\"; "
                     "export HARMONY_PLUGIN_DATA=\"%s\"; "
                     "cd \"%s\" && (\"./%s\" > /tmp/plugins/%s.log 2>&1 & echo $! > /tmp/plugins/%s.pid)",
                     plugin_id, HARMONY_PLUGINS_RUN_DIR, plugin_id, data_dir,
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

    mkdir(HARMONY_PLUGINS_RUN_DIR, 0755);
    char marker[128];
    stopped_marker_path(plugin_id, marker, sizeof(marker));
    int mfd = open(marker, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (mfd >= 0) close(mfd);

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
    char marker[128];
    stopped_marker_path(plugin_id, marker, sizeof(marker));
    unlink(marker);
    return 0;
}

/* Puts the files of src_dir in place of the installed plugin. A plugin that was disabled
 * stays disabled: the marker was inside the directory that is replaced. */
static void replace_plugin_dir(const char *plug_id, const char *src_dir) {
    int was_disabled = !is_plugin_enabled(plug_id);
    plugin_mgr_stop(plug_id);

    char dest_dir[256], cmd[512];
    snprintf(dest_dir, sizeof(dest_dir), "%s/%s", HARMONY_PLUGINS_DIR, plug_id);
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", dest_dir);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", dest_dir);
    system(cmd);
    copy_dir_recursive(src_dir, dest_dir);
    snprintf(cmd, sizeof(cmd), "chmod -R 755 \"%s\"", dest_dir);
    system(cmd);

    if (was_disabled) {
        char dis_path[300];
        snprintf(dis_path, sizeof(dis_path), "%s/disabled", dest_dir);
        int fd = open(dis_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) close(fd);
    } else {
        plugin_mgr_start(plug_id);
    }
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

/* Restarts of a plugin that keeps dying: 20 s, 40 s, ... up to 5 min apart. Each start is a
 * fork and a 150 ms wait in the main loop, and a crash loop used to pay it every 10 s. */
#define WD_SLOTS 16
#define WD_MAX_DELAY_S 300
static struct {
    char id[64];
    int restarts;
    time_t next_try;
} s_wd[WD_SLOTS];

static int wd_slot(const char *id) {
    int free_slot = -1;
    for (int i = 0; i < WD_SLOTS; i++) {
        if (strcmp(s_wd[i].id, id) == 0) return i;
        if (!s_wd[i].id[0] && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) {
        snprintf(s_wd[free_slot].id, sizeof(s_wd[free_slot].id), "%s", id);
        s_wd[free_slot].restarts = 0;
        s_wd[free_slot].next_try = 0;
    }
    return free_slot;
}

static int wd_restarts(const char *id) {
    for (int i = 0; i < WD_SLOTS; i++) {
        if (strcmp(s_wd[i].id, id) == 0) return s_wd[i].restarts;
    }
    return 0;
}

void plugin_mgr_watchdog_tick(void) {
    DIR *d = opendir(HARMONY_PLUGINS_DIR);
    if (!d) return;

    time_t now = time(NULL);
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (!is_safe_plugin_id(de->d_name)) continue;
        if (!is_plugin_enabled(de->d_name) || is_stopped_by_request(de->d_name)) continue;
        int slot = wd_slot(de->d_name);
        if (plugin_mgr_is_running(de->d_name)) {
            /* ran a whole watchdog period: no longer counted as crashing */
            if (slot >= 0) s_wd[slot].restarts = 0;
            continue;
        }
        if (slot >= 0) {
            if (now < s_wd[slot].next_try) continue;
            int n = s_wd[slot].restarts < 5 ? s_wd[slot].restarts : 5;
            int delay = 10 << (n + 1);
            if (delay > WD_MAX_DELAY_S) delay = WD_MAX_DELAY_S;
            s_wd[slot].restarts++;
            s_wd[slot].next_try = now + delay;
            printf("[!] Plugin '%s' is not running: restart %d (next one not before %d s)\n",
                   de->d_name, s_wd[slot].restarts, delay);
        }
        plugin_mgr_start(de->d_name);
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
        cJSON_AddBoolToObject(p_obj, "stopped", is_stopped_by_request(de->d_name));
        cJSON_AddNumberToObject(p_obj, "restarts", wd_restarts(de->d_name));
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
    tmp_name(tmp_unpack, sizeof(tmp_unpack), "/tmp/p_unpack", "");

    char list_path[160];
    snprintf(list_path, sizeof(list_path), "%s.list", tmp_unpack);
    if (!archive_entries_safe(archive_path, list_path)) {
        if (err_msg) snprintf(err_msg, err_msg_sz, "Archive unreadable, or has a link, absolute or '..' entry");
        return -1;
    }
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

    /* Move unpacked files */
    char src_dir[256];
    char *last_slash = strrchr(mpath, '/');
    if (last_slash) *last_slash = '\0';
    snprintf(src_dir, sizeof(src_dir), "%s", mpath);
    replace_plugin_dir(plug_id, src_dir);

    /* Clean up temp dir */
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmp_unpack);
    system(cmd);
    cJSON_Delete(manifest);

    return 0;
}

/* SHA-256 the marketplace lists for this download URL, if any */
static int catalog_sha256_for(const char *url, char out[65]) {
    out[0] = '\0';
    cJSON *cat = plugin_mgr_get_marketplace_catalog();
    const cJSON *p = NULL;
    cJSON_ArrayForEach(p, cat) {
        const cJSON *u = cJSON_GetObjectItemCaseSensitive(p, "download_url");
        const cJSON *h = cJSON_GetObjectItemCaseSensitive(p, "sha256");
        if (cJSON_IsString(u) && strcmp(u->valuestring, url) == 0 && cJSON_IsString(h) && strlen(h->valuestring) == 64) {
            snprintf(out, 65, "%s", h->valuestring);
            break;
        }
    }
    cJSON_Delete(cat);
    return out[0] ? 0 : -1;
}

int plugin_mgr_install_from_url(const char *url, const char *sha256_hex, char *err_msg, size_t err_msg_sz) {
    if (!is_safe_url(url)) {
        if (err_msg) snprintf(err_msg, err_msg_sz, "Invalid URL (http/https, plain characters only)");
        return -1;
    }

    /* busybox wget does not check certificates: the SHA-256 (from the caller or the
     * marketplace entry) is what tells a damaged or swapped archive */
    char want[65] = {0};
    if (sha256_hex && sha256_hex[0]) snprintf(want, sizeof(want), "%s", sha256_hex);
    else catalog_sha256_for(url, want);

    char tmp_tar[128];
    tmp_name(tmp_tar, sizeof(tmp_tar), "/tmp/plug_dl", ".tar.gz");

    int rc = fetch_url(url, tmp_tar, FETCH_ARCHIVE_MAX_BYTES, FETCH_ARCHIVE_MAX_MS);
    if (rc != 0) {
        if (err_msg) snprintf(err_msg, err_msg_sz, rc == RUN_TOO_BIG ? "Archive larger than 16 MB" :
                                                   rc == RUN_TOO_SLOW ? "Archive download took too long" :
                                                   "Failed to download archive from URL");
        return -1;
    }

    if (want[0]) {
        char got[65];
        if (file_sha256_hex(tmp_tar, got) != 0 || strcasecmp(got, want) != 0) {
            unlink(tmp_tar);
            if (err_msg) snprintf(err_msg, err_msg_sz, "Archive SHA-256 does not match the published checksum");
            return -1;
        }
    }

    int res = plugin_mgr_install_archive(tmp_tar, err_msg, err_msg_sz);
    unlink(tmp_tar);
    return res;
}

int plugin_mgr_install_dev_raw(const char *url, char *err_msg, size_t err_msg_sz) {
    if (!is_safe_url(url)) {
        if (err_msg) snprintf(err_msg, err_msg_sz, "Invalid URL (http/https, plain characters only)");
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
    tmp_name(tmp_dev, sizeof(tmp_dev), "/tmp/p_dev", "");
    mkdir(tmp_dev, 0755);

    /* 1. Download manifest.json */
    char cmd[1024];
    char mpath[256];
    snprintf(mpath, sizeof(mpath), "%s/manifest.json", tmp_dev);
    int rc = fetch_url(manifest_url, mpath, FETCH_SMALL_MAX_BYTES, FETCH_SMALL_MAX_MS);
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

    /* A file that is unsafe to name or fails to download stops the install: installed
     * without it, the plugin was left broken */
    const char *bad_file = NULL;
    if (j_files && cJSON_IsArray(j_files) && cJSON_GetArraySize(j_files) > 0) {
        cJSON *f_item = NULL;
        cJSON_ArrayForEach(f_item, j_files) {
            if (!f_item || !cJSON_IsString(f_item) || !f_item->valuestring[0]) continue;
            const char *fname = f_item->valuestring;
            if (strcmp(fname, "manifest.json") == 0) continue;
            if (!is_safe_rel_path(fname)) {
                bad_file = fname;
                break;
            }

            char local_path[384];
            snprintf(local_path, sizeof(local_path), "%s/%s", tmp_dev, fname);
            char *last_slash = strrchr(local_path, '/');
            if (last_slash) {
                *last_slash = '\0';
                snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", local_path);
                system(cmd);
                *last_slash = '/';
            }

            char file_url[768];
            snprintf(file_url, sizeof(file_url), "%s/%s", base_url, fname);
            if (fetch_url(file_url, local_path, FETCH_FILE_MAX_BYTES, FETCH_ARCHIVE_MAX_MS) != 0) {
                bad_file = fname;
                break;
            }
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
        if (entry && strcmp(entry, "run.sh") != 0 && strcmp(entry, "manifest.json") != 0 && is_safe_rel_path(entry)) {
            defaults[d_count++] = entry;
        }

        for (int i = 0; i < d_count; i++) {
            const char *fname = defaults[i];
            char local_path[384];
            snprintf(local_path, sizeof(local_path), "%s/%s", tmp_dev, fname);
            char file_url[768];
            snprintf(file_url, sizeof(file_url), "%s/%s", base_url, fname);
            if (fetch_url(file_url, local_path, FETCH_FILE_MAX_BYTES, FETCH_ARCHIVE_MAX_MS) != 0) {
                bad_file = fname;
                break;
            }
            if (strstr(fname, ".sh") != NULL || (entry && strcmp(fname, entry) == 0)) {
                chmod(local_path, 0755);
            }
        }
    }

    if (bad_file) {
        if (err_msg) snprintf(err_msg, err_msg_sz, "Manifest file '%.60s' is unsafe or failed to download", bad_file);
        cJSON_Delete(manifest);
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmp_dev);
        system(cmd);
        return -1;
    }
    cJSON_Delete(manifest);

    /* 3. Stop running plugin, install into /data/harmony/plugins/<plug_id> */
    replace_plugin_dir(plug_id, tmp_dev);

    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmp_dev);
    system(cmd);

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
    mkdir(HARMONY_PLUGIN_ROOT, 0755);
    mkdir(HARMONY_PLUGIN_ROOT "/config", 0755);
    int ret = write_file_atomic_sized(HARMONY_REPOSITORIES_FILE, out, strlen(out));
    free(out);
    return ret;
}

int plugin_mgr_add_repository(const char *url) {
    if (!is_safe_url(url)) return -1;
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

static cJSON *read_catalog_cache(const char *cache_path) {
    size_t flen = 0;
    char *cached = read_file_text_alloc(cache_path, FETCH_SMALL_MAX_BYTES * 4, &flen);
    if (!cached) return NULL;
    cJSON *c_root = cJSON_Parse(cached);
    free(cached);
    return c_root;
}

cJSON *plugin_mgr_get_marketplace_catalog(void) {
    const char *cache_path = "/tmp/plugins/marketplace_cache.json";
    struct stat st;
    if (stat(cache_path, &st) == 0 && (time(NULL) - st.st_mtime) < 600) {
        cJSON *c_root = read_catalog_cache(cache_path);
        if (c_root) return c_root;
    }

    cJSON *repos = plugin_mgr_get_repositories();
    cJSON *catalog = cJSON_CreateArray();

    cJSON *repo_url = NULL;
    cJSON_ArrayForEach(repo_url, repos) {
        if (!repo_url->valuestring) continue;
        char tmp_repo[128];
        tmp_name(tmp_repo, sizeof(tmp_repo), "/tmp/plugins/repo_tmp", ".json");

        if (fetch_url(repo_url->valuestring, tmp_repo, FETCH_SMALL_MAX_BYTES, FETCH_SMALL_MAX_MS) == 0 &&
            access(tmp_repo, R_OK) == 0) {
            size_t rlen = 0;
            char *rtxt = read_file_text_alloc(tmp_repo, FETCH_SMALL_MAX_BYTES, &rlen);
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

    /* Every repository failed (offline): the last catalog, even old, beats an empty one */
    if (cJSON_GetArraySize(catalog) == 0) {
        cJSON *stale = read_catalog_cache(cache_path);
        if (stale) {
            cJSON_Delete(catalog);
            return stale;
        }
        return catalog;
    }

    char *cat_str = cJSON_PrintUnformatted(catalog);
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
