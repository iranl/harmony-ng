#include "http_api_system.h"
#include "hub_info.h"
#include "hub_mdns.h"
#include "http_util.h"
#include "http_auth.h"
#include "http_api_custom_icons.h"
#include "icon_pack.h"
#include "hub_network_util.h"
#include "config_store.h"
#include "hw_action.h"
#include "orchestrator.h"
#include "hub_mqtt.h"
#include "hbus_server.h"
#include "hub_ssdp.h"
#include "harmony_version.h"
#include "resource_cache.h"
#include "cJSON.h"
#include "sha256.h"
#include "timezone_db.h"
#include "hub_led.h"
#include "http_api_ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/socket.h>

extern int g_dry_run;



static void get_disk_space_kb(uint64_t *total_kb, uint64_t *free_kb) {
    struct statvfs sv;
    *total_kb = 5120;
    *free_kb = 0;
    if (statvfs("/data", &sv) == 0 && sv.f_frsize > 0) {
        *total_kb = ((uint64_t)sv.f_blocks * sv.f_frsize) / 1024;
        *free_kb = ((uint64_t)sv.f_bavail * sv.f_frsize) / 1024;
    } else if (statvfs("/", &sv) == 0 && sv.f_frsize > 0) {
        *total_kb = ((uint64_t)sv.f_blocks * sv.f_frsize) / 1024;
        *free_kb = ((uint64_t)sv.f_bavail * sv.f_frsize) / 1024;
    }
}

static void get_cache_space_kb(uint64_t *total_kb, uint64_t *free_kb) {
    struct statvfs sv;
    *total_kb = 0;
    *free_kb = 0;
    if (statvfs("/cache", &sv) == 0 && sv.f_frsize > 0) {
        *total_kb = ((uint64_t)sv.f_blocks * sv.f_frsize) / 1024;
        *free_kb = ((uint64_t)sv.f_bavail * sv.f_frsize) / 1024;
    }
}

static uint64_t get_directory_size_bytes(const char *path) {
    DIR *d = opendir(path);
    if (!d) return 0;
    uint64_t total = 0;
    struct dirent *de;
    char subpath[512];
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        int n = snprintf(subpath, sizeof(subpath), "%s/%s", path, de->d_name);
        if (n < 0 || (size_t)n >= sizeof(subpath)) continue;
        struct stat st;
        if (lstat(subpath, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                total += get_directory_size_bytes(subpath);
            } else {
                total += (uint64_t)st.st_size;
            }
        }
    }
    closedir(d);
    return total;
}

#define STOCK_RES_DIR "/data/resources"
/* Written by tools/backup_hub.py once a local archive holds exactly the stock files there, as
 * "<archive name> <fingerprint>": the only way back after the delete, so the route refuses
 * unless the fingerprint matches the files now in /data/resources */
#define STOCK_RES_SAVED "/data/harmony/stock_resources_saved"

/* Apparent size (st_size). /data is JFFS2 (compressed), so the flash space they take is smaller. */
static uint64_t get_stock_resources_kb(void) {
    struct stat st;
    if (lstat(STOCK_RES_DIR, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
    uint64_t bytes = get_directory_size_bytes(STOCK_RES_DIR);
    return (bytes > 0) ? ((bytes + 1023) / 1024) : 0;
}

/* XOR of sha256("<size> <path below /data/resources>") over every entry but directories (a link
 * counts as size 0), so the walk order does not matter: tools/backup_hub.py stock_fingerprint().
 * Returns -1 when a directory cannot be read or a path is too long. */
static int stock_fingerprint_walk(const char *path, const char *rel, uint8_t acc[32], int *count) {
    DIR *d = opendir(path);
    if (!d) return -1;
    int rc = 0;
    struct dirent *de;
    char subpath[512], subrel[512], line[560];
    while (rc == 0 && (de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        int n = snprintf(subpath, sizeof(subpath), "%s/%s", path, de->d_name);
        int m = rel[0] ? snprintf(subrel, sizeof(subrel), "%s/%s", rel, de->d_name)
                       : snprintf(subrel, sizeof(subrel), "%s", de->d_name);
        struct stat st;
        if (n < 0 || (size_t)n >= sizeof(subpath) || m < 0 || (size_t)m >= sizeof(subrel) ||
            lstat(subpath, &st) != 0) {
            rc = -1;
        } else if (S_ISDIR(st.st_mode)) {
            rc = stock_fingerprint_walk(subpath, subrel, acc, count);
        } else {
            int len = snprintf(line, sizeof(line), "%llu %s",
                               S_ISLNK(st.st_mode) ? 0ULL : (unsigned long long)st.st_size, subrel);
            uint8_t digest[32];
            sha256_hash((const uint8_t *)line, (size_t)len, digest);
            for (int i = 0; i < 32; i++) acc[i] ^= digest[i];
            (*count)++;
        }
    }
    closedir(d);
    return rc;
}

/* "<count>:<64 hex>" of /data/resources into out (at least 80 bytes); -1 on a read error */
static int stock_fingerprint(char *out, size_t out_len) {
    uint8_t acc[32] = {0};
    int count = 0;
    if (stock_fingerprint_walk(STOCK_RES_DIR, "", acc, &count) != 0) return -1;
    int n = snprintf(out, out_len, "%d:", count);
    for (int i = 0; i < 32 && n > 0 && (size_t)n + 2 < out_len; i++) n += snprintf(out + n, out_len - (size_t)n, "%02x", acc[i]);
    return 0;
}

/* The mark is "<copy name> <fingerprint>": a copy of exactly the files there now was saved */
static int stock_resources_saved(void) {
    size_t len = 0;
    char *mark = read_file_text_alloc(STOCK_RES_SAVED, 1024, &len);
    if (!mark) return 0;
    while (len > 0 && (mark[len - 1] == '\n' || mark[len - 1] == '\r' || mark[len - 1] == ' ')) mark[--len] = '\0';
    const char *sp = strrchr(mark, ' ');
    char fp[80];
    int ok = sp && sp != mark && stock_fingerprint(fp, sizeof(fp)) == 0 && strcmp(sp + 1, fp) == 0;
    free(mark);
    return ok;
}

static int stock_import_done(void) {
    struct stat st;
    return stat("/data/harmony/config/devices.json", &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

/* Returns the number of entries that could not be removed (-1: path not opened) */
static int empty_directory_contents(const char *path) {
    DIR *d = opendir(path);
    if (!d) return -1;
    int failed = 0;
    struct dirent *de;
    char subpath[512];
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        int n = snprintf(subpath, sizeof(subpath), "%s/%s", path, de->d_name);
        if (n < 0 || (size_t)n >= sizeof(subpath)) { failed++; continue; }
        struct stat st;
        if (lstat(subpath, &st) != 0) { failed++; continue; }
        if (S_ISDIR(st.st_mode)) {
            int sub = empty_directory_contents(subpath);
            if (sub != 0 || rmdir(subpath) != 0) failed += (sub > 0) ? sub : 1;
        } else if (unlink(subpath) != 0) {
            failed++;
        }
    }
    closedir(d);
    return failed;
}

/* /etc is on flash: rewrite only when the content changes (every start applies the settings) */
static void write_file_if_changed(const char *path, const char *text) {
    size_t len = 0;
    char *cur = read_file_text_alloc(path, 4096, &len);
    int same = cur && len == strlen(text) && memcmp(cur, text, len) == 0;
    free(cur);
    if (!same && write_file_content(path, text) != 0) {
        fprintf(stderr, "[-] Could not write %s\n", path);
    }
}

void apply_hub_settings(const struct hub_settings *s) {
    if (!s) return;
    hw_set_hub_name(s->hub_name);   /* sent to plugins as "hub_name" */
    hub_mdns_apply_settings(s);
    hbus_apply_settings(s);
    hub_ssdp_apply_settings(s);
    hub_led_write(hub_led_mode_value(s->led_mode));
    if (s->timezone[0]) {
        char posix_tz[64];
        timezone_to_posix(s->timezone, posix_tz, sizeof(posix_tz));
        char line[80];
        snprintf(line, sizeof(line), "%s\n", posix_tz);
        write_file_if_changed("/etc/TZ", line);
        setenv("TZ", posix_tz, 1);
        tzset();
    }
    if (s->ntp_server[0] && !g_dry_run) {
        char line[160];
        snprintf(line, sizeof(line), "server %s\n", s->ntp_server);
        write_file_if_changed("/etc/ntp.conf", line);
    }
}

int http_api_system_handle(int client_fd, const char *method, const char *clean_path,
                           const char *query, const char *body, const char *req,
                           enum http_role auth_role, const char *auth_user) {
    (void)req;

    /* Route: GET /api/status */
    if (strcmp(clean_path, "/api/status") == 0 && strcmp(method, "GET") == 0) {
        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        struct hub_settings cs;
        config_store_get_settings(&cs);
        cJSON_AddStringToObject(st, "hub_name", cs.hub_name[0] ? cs.hub_name : "Harmony Hub");
        cJSON_AddStringToObject(st, "firmware", "4.15.600");
        hub_info_add_status_fields(st, &cs);
        cJSON_AddStringToObject(st, "activity", orch_get_current_activity());
        cJSON_AddNumberToObject(st, "state", (int)orch_get_state());
        cJSON_AddBoolToObject(st, "busy", orch_is_busy() ? 1 : 0);
        char local_ip[64];
        net_get_local_ip(client_fd, local_ip, sizeof(local_ip));
        cJSON_AddStringToObject(st, "ip", local_ip);
        uint64_t total_kb = 5120, free_kb = 0;
        get_disk_space_kb(&total_kb, &free_kb);
        cJSON_AddNumberToObject(st, "total_kb", (double)total_kb);
        cJSON_AddNumberToObject(st, "free_kb", (double)free_kb);
        uint64_t cache_total_kb = 0, cache_free_kb = 0;
        get_cache_space_kb(&cache_total_kb, &cache_free_kb);
        cJSON_AddNumberToObject(st, "cache_total_kb", (double)cache_total_kb);
        cJSON_AddNumberToObject(st, "cache_free_kb", (double)cache_free_kb);
        cJSON_AddBoolToObject(st, "updating", ota_update_busy());

        int auth_on = 0;
        load_user_accounts(NULL, 0, &auth_on);
        cJSON_AddBoolToObject(st, "auth_enabled", auth_on);
        cJSON_AddStringToObject(st, "role", auth_role == ROLE_USER ? "user" : "admin");
        cJSON_AddStringToObject(st, "user", auth_user && auth_user[0] ? auth_user : "admin");

        FILE *uf = fopen("/proc/uptime", "r");
        if (uf) {
            double up_sec = 0;
            if (fscanf(uf, "%lf", &up_sec) == 1) {
                cJSON_AddNumberToObject(st, "uptime_sec", (long)up_sec);
            }
            fclose(uf);
        }

        char mhost[128] = {0}, mbase[128] = {0};
        int mport = 1883, mconn = 0;
        daemon_get_mqtt_info(mhost, sizeof(mhost), &mport, mbase, sizeof(mbase), &mconn);

        cJSON *mj = cJSON_CreateObject();
        cJSON_AddBoolToObject(mj, "connected", mconn);
        cJSON_AddStringToObject(mj, "broker", mhost);
        cJSON_AddNumberToObject(mj, "port", mport);
        cJSON_AddStringToObject(mj, "baseTopic", mbase);
        cJSON_AddBoolToObject(mj, "v2", 1);
        cJSON_AddItemToObject(st, "mqtt", mj);

        http_ensure_icons_extracted();
        int icons_present = icon_pack_available();
        struct stat st_icon;
        const char *icon_dir = HARMONY_TMP_WWW "/assets/icons";
        if (stat(icon_dir, &st_icon) != 0) {
            icon_dir = "/cache/www/assets/icons";
        }
        if (stat(icon_dir, &st_icon) == 0 && S_ISDIR(st_icon.st_mode)) {
            DIR *d = opendir(icon_dir);
            if (d) {
                struct dirent *de;
                while ((de = readdir(d)) != NULL) {
                    if (de->d_name[0] != '.') {
                        icons_present = 1;
                        break;
                    }
                }
                closedir(d);
            }
        }
        cJSON_AddBoolToObject(st, "has_icons", icons_present);

        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return 1;
    }

    /* Route: GET /api/mqtt-status */
    if (strcmp(clean_path, "/api/mqtt-status") == 0 && strcmp(method, "GET") == 0) {
        char mhost[128] = {0}, mbase[128] = {0};
        int mport = 1883, mconn = 0;
        daemon_get_mqtt_info(mhost, sizeof(mhost), &mport, mbase, sizeof(mbase), &mconn);

        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddBoolToObject(resp, "connected", mconn);
        cJSON_AddStringToObject(resp, "broker", mhost);
        cJSON_AddNumberToObject(resp, "port", mport);
        cJSON_AddStringToObject(resp, "baseTopic", mbase);
        cJSON_AddBoolToObject(resp, "v2", 1);

        cJSON *cfg = config_store_get_mqtt_json();
        if (cfg) {
            /* never send the stored broker password back: the UI only needs to know there is
             * one (an empty field on save keeps it) */
            cJSON *pw = cJSON_GetObjectItemCaseSensitive(cfg, "password");
            int has_pw = cJSON_IsString(pw) && pw->valuestring && pw->valuestring[0];
            cJSON_DeleteItemFromObjectCaseSensitive(cfg, "password");
            cJSON_AddBoolToObject(cfg, "has_password", has_pw);
            cJSON_AddItemToObject(resp, "config", cfg);
        }
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* Route: GET /api/logs */
    if (strcmp(clean_path, "/api/logs") == 0 && strcmp(method, "GET") == 0) {
        const char *src = "daemon";
        char qsrc[16];
        http_query_param(query, "source", qsrc, sizeof(qsrc));
        if (strcmp(qsrc, "btstack") == 0) src = "btstack";
        else if (strcmp(qsrc, "rf") == 0) src = "rf";
        else if (strcmp(qsrc, "system") == 0) src = "system";
        else if (strcmp(qsrc, "network") == 0) src = "network";

        /* init.sh sends the output of hub_daemon and hub_rf to harmony-init.log (hub_btstack keeps
         * its own file), the network scripts write harmony-network.log */
        const char *filepath = "/tmp/harmony-init.log";
        if (strcmp(src, "btstack") == 0) filepath = "/tmp/hub-btstack.log";
        else if (strcmp(src, "network") == 0) filepath = "/tmp/harmony-network.log";

        char log_buf[16384] = {0};
        if (strcmp(src, "system") == 0) {
            FILE *p = popen("logread 2>/dev/null || dmesg 2>/dev/null", "r");
            if (p) {
                size_t rd = fread(log_buf, 1, sizeof(log_buf) - 1, p);
                log_buf[rd] = '\0';
                pclose(p);
            }
        } else {
            FILE *f = fopen(filepath, "r");
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                long off = (sz > 12288) ? (sz - 12288) : 0;
                fseek(f, off, SEEK_SET);
                size_t rd = fread(log_buf, 1, sizeof(log_buf) - 1, f);
                log_buf[rd] = '\0';
                fclose(f);
            }
        }

        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddStringToObject(resp, "source", src);
        cJSON_AddStringToObject(resp, "logs", log_buf);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* Route: GET /api/system-info */
    if (strcmp(clean_path, "/api/system-info") == 0 && strcmp(method, "GET") == 0) {
        char uptime_str[128] = "Unknown";
        FILE *uf = fopen("/proc/uptime", "r");
        if (uf) {
            double up_sec = 0;
            if (fscanf(uf, "%lf", &up_sec) == 1) {
                long s = (long)up_sec;
                long d = s / 86400;
                long h = (s % 86400) / 3600;
                long m = (s % 3600) / 60;
                snprintf(uptime_str, sizeof(uptime_str), "%ldd %ldh %ldm (%lds)", d, h, m, s);
            }
            fclose(uf);
        }

        char load_str[64] = "N/A";
        FILE *lf = fopen("/proc/loadavg", "r");
        if (lf) {
            if (fgets(load_str, sizeof(load_str), lf)) {
                char *nl = strchr(load_str, '\n'); if (nl) *nl = '\0';
            }
            fclose(lf);
        }

        char kernel_str[256] = "Linux mips (Atheros AR9344)";
        FILE *kf = fopen("/proc/version", "r");
        if (kf) {
            if (fgets(kernel_str, sizeof(kernel_str), kf)) {
                char *nl = strchr(kernel_str, '\n'); if (nl) *nl = '\0';
            }
            fclose(kf);
        }

        long total = 0, free_mem = 0, buffers = 0, cached = 0;
        FILE *mf = fopen("/proc/meminfo", "r");
        if (mf) {
            char line[128];
            while (fgets(line, sizeof(line), mf)) {
                long val = 0;
                if (sscanf(line, "MemTotal: %ld kB", &val) == 1) total = val;
                else if (sscanf(line, "MemFree: %ld kB", &val) == 1) free_mem = val;
                else if (sscanf(line, "Buffers: %ld kB", &val) == 1) buffers = val;
                else if (sscanf(line, "Cached: %ld kB", &val) == 1) cached = val;
            }
            fclose(mf);
        }
        long avail = free_mem + buffers + cached;
        char mem_str[256];
        snprintf(mem_str, sizeof(mem_str), "Total: %ld MB, Free: %ld MB (Avail: %ld MB)",
                 total / 1024, free_mem / 1024, avail / 1024);

        char net_details[4096] = "=== Network Interfaces (/proc/net/dev) ===\n";
        FILE *ndf = fopen("/proc/net/dev", "r");
        if (ndf) {
            size_t cur_len = strlen(net_details);
            size_t r = fread(net_details + cur_len, 1, sizeof(net_details) - cur_len - 512, ndf);
            net_details[cur_len + r] = '\0';
            fclose(ndf);
        }
        strncat(net_details, "\n=== Routing Table (/proc/net/route) ===\n", sizeof(net_details) - strlen(net_details) - 1);
        FILE *nrf = fopen("/proc/net/route", "r");
        if (nrf) {
            size_t cur_len = strlen(net_details);
            size_t r = fread(net_details + cur_len, 1, sizeof(net_details) - cur_len - 1, nrf);
            net_details[cur_len + r] = '\0';
            fclose(nrf);
        }

        char fw_str[128] = "4.15.600";
        FILE *vf = fopen("/etc/version", "r");
        if (vf) {
            if (fgets(fw_str, sizeof(fw_str), vf)) {
                char *nl = strchr(fw_str, '\n'); if (nl) *nl = '\0';
                nl = strchr(fw_str, '\r'); if (nl) *nl = '\0';
            }
            fclose(vf);
        }

        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        struct hub_settings cs;
        config_store_get_settings(&cs);
        cJSON_AddStringToObject(st, "hub_name", cs.hub_name[0] ? cs.hub_name : "Harmony Hub");
        cJSON_AddStringToObject(st, "uptime", uptime_str);
        cJSON_AddStringToObject(st, "loadavg", load_str);
        cJSON_AddStringToObject(st, "kernel", kernel_str);
        cJSON_AddStringToObject(st, "firmware", fw_str);
        cJSON_AddStringToObject(st, "version", HARMONY_VERSION);
        cJSON_AddStringToObject(st, "commit", HARMONY_COMMIT);
        cJSON_AddStringToObject(st, "build_time", __DATE__ " " __TIME__);
        cJSON_AddStringToObject(st, "memory", mem_str);
        cJSON_AddNumberToObject(st, "mem_total_kb", total);
        cJSON_AddNumberToObject(st, "mem_free_kb", free_mem);
        cJSON_AddNumberToObject(st, "mem_available_kb", avail);
        cJSON_AddStringToObject(st, "network_details", net_details);

        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return 1;
    }

    /* Route: GET /api/disk-space */
    if (strcmp(clean_path, "/api/disk-space") == 0 && strcmp(method, "GET") == 0) {
        uint64_t total_kb = 5120, free_kb = 0;
        get_disk_space_kb(&total_kb, &free_kb);
        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        cJSON_AddNumberToObject(st, "total_kb", (double)total_kb);
        cJSON_AddNumberToObject(st, "free_kb", (double)free_kb);
        cJSON_AddNumberToObject(st, "used_kb", (double)(total_kb > free_kb ? total_kb - free_kb : 0));
        cJSON_AddNumberToObject(st, "percent_free", total_kb ? (double)(free_kb * 100 / total_kb) : 0);

        uint64_t cache_total_kb = 0, cache_free_kb = 0;
        get_cache_space_kb(&cache_total_kb, &cache_free_kb);
        cJSON_AddNumberToObject(st, "cache_total_kb", (double)cache_total_kb);
        cJSON_AddNumberToObject(st, "cache_free_kb", (double)cache_free_kb);
        cJSON_AddNumberToObject(st, "cache_used_kb", (double)(cache_total_kb > cache_free_kb ? cache_total_kb - cache_free_kb : 0));
        cJSON_AddNumberToObject(st, "cache_percent_free", cache_total_kb ? (double)(cache_free_kb * 100 / cache_total_kb) : 0);

        uint64_t stock_res_kb = get_stock_resources_kb();
        int stock_saved = stock_res_kb > 0 && stock_resources_saved();
        cJSON_AddNumberToObject(st, "stock_resources_kb", (double)stock_res_kb);
        cJSON_AddBoolToObject(st, "stock_resources_saved", stock_saved);
        cJSON_AddBoolToObject(st, "can_empty_stock_resources",
                              stock_res_kb > 0 && stock_saved && stock_import_done());

        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/sync or POST /api/reload */
    if ((strcmp(clean_path, "/api/sync") == 0 || strcmp(clean_path, "/api/reload") == 0) && strcmp(method, "POST") == 0) {
        invalidate_device_list_cache();
        config_store_init();
        notify_rf_push_config();
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddStringToObject(resp, "message", "Configuration reloaded and synchronized");
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* Route: POST /mqtt or POST /api/mqtt-save */
    if ((strcmp(clean_path, "/mqtt") == 0 || strcmp(clean_path, "/api/mqtt-save") == 0) && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        if (params) {
            cJSON *cfg = config_store_get_mqtt_json();
            if (!cfg) cfg = cJSON_CreateObject();

            cJSON *p_en = cJSON_GetObjectItemCaseSensitive(params, "enabled");
            if (p_en) {
                int is_en = (cJSON_IsTrue(p_en) ||
                             (cJSON_IsNumber(p_en) && p_en->valueint > 0) ||
                             (cJSON_IsString(p_en) && (strcmp(p_en->valuestring, "1") == 0 || strcasecmp(p_en->valuestring, "true") == 0)));
                cJSON_DeleteItemFromObject(cfg, "enabled");
                cJSON_AddBoolToObject(cfg, "enabled", is_en);
            } else if (!cJSON_GetObjectItemCaseSensitive(cfg, "enabled")) {
                cJSON_AddBoolToObject(cfg, "enabled", 1);
            }

            cJSON *p_host = cJSON_GetObjectItemCaseSensitive(params, "host");
            if (p_host && cJSON_IsString(p_host) && p_host->valuestring) {
                cJSON_DeleteItemFromObject(cfg, "host");
                cJSON_AddStringToObject(cfg, "host", p_host->valuestring);
            }

            cJSON *p_port = cJSON_GetObjectItemCaseSensitive(params, "port");
            if (p_port) {
                int pnum = cJSON_IsNumber(p_port) ? p_port->valueint : (cJSON_IsString(p_port) ? atoi(p_port->valuestring) : 1883);
                cJSON_DeleteItemFromObject(cfg, "port");
                cJSON_AddNumberToObject(cfg, "port", pnum > 0 ? pnum : 1883);
            }

            cJSON *p_user = cJSON_GetObjectItemCaseSensitive(params, "username");
            if (p_user && cJSON_IsString(p_user)) {
                cJSON_DeleteItemFromObject(cfg, "username");
                cJSON_AddStringToObject(cfg, "username", p_user->valuestring);
                /* No user name, no password: clearing the user name is how stored credentials are
                 * removed (an empty password field keeps the stored one) */
                if (!p_user->valuestring[0]) cJSON_DeleteItemFromObject(cfg, "password");
            }

            /* A typed password replaces the stored one; an empty or missing field keeps it. The old
             * keep_password flag is ignored: ticked by default, it threw a typed password away.
             * A password is stored only with a user name. */
            cJSON *p_pass = cJSON_GetObjectItemCaseSensitive(params, "password");
            cJSON *s_user = cJSON_GetObjectItemCaseSensitive(cfg, "username");
            int has_user = cJSON_IsString(s_user) && s_user->valuestring[0];
            if (has_user && p_pass && cJSON_IsString(p_pass) && p_pass->valuestring[0]) {
                cJSON_DeleteItemFromObject(cfg, "password");
                cJSON_AddStringToObject(cfg, "password", p_pass->valuestring);
            }

            cJSON *p_bt = cJSON_GetObjectItemCaseSensitive(params, "baseTopic");
            if (p_bt && cJSON_IsString(p_bt) && p_bt->valuestring[0]) {
                cJSON_DeleteItemFromObject(cfg, "baseTopic");
                cJSON_AddStringToObject(cfg, "baseTopic", p_bt->valuestring);
            }

            cJSON *p_cid = cJSON_GetObjectItemCaseSensitive(params, "clientId");
            if (p_cid && cJSON_IsString(p_cid) && p_cid->valuestring[0]) {
                cJSON_DeleteItemFromObject(cfg, "clientId");
                cJSON_AddStringToObject(cfg, "clientId", p_cid->valuestring);
            }

            cJSON *p_disc = cJSON_GetObjectItemCaseSensitive(params, "discoveryPrefix");
            if (p_disc && cJSON_IsString(p_disc) && p_disc->valuestring[0]) {
                cJSON_DeleteItemFromObject(cfg, "discoveryPrefix");
                cJSON_AddStringToObject(cfg, "discoveryPrefix", p_disc->valuestring);
            }
            cJSON *p_ha = cJSON_GetObjectItemCaseSensitive(params, "haDiscovery");
            if (p_ha) {
                int ha_val = (cJSON_IsTrue(p_ha) || (cJSON_IsNumber(p_ha) && p_ha->valueint > 0) ||
                              (cJSON_IsString(p_ha) && (strcmp(p_ha->valuestring, "1") == 0 || strcasecmp(p_ha->valuestring, "true") == 0)));
                cJSON_DeleteItemFromObject(cfg, "haDiscovery");
                cJSON_AddBoolToObject(cfg, "haDiscovery", ha_val);
            }

            /* Fields of the old MQTT layer that have no v2 equivalent are dropped: device name
             * (the hub name is used), keepalive and the per-remote event topics (button events
             * always go to <base>/remote/<slot>/button) */
            static const char *const k_legacy_keys[] = {
                "name", "deviceName", "keepAlive",
                "btRemoteEvents", "btRemoteTopic", "remoteEvents", "remoteTopic",
                "eliteRemoteEvents", "eliteRemoteTopic", /* names before the elite -> remote rename */
                "slot1RemoteEvents", "slot1RemoteTopic", "slot2RemoteEvents", "slot2RemoteTopic", NULL
            };
            for (int lk = 0; k_legacy_keys[lk]; lk++) cJSON_DeleteItemFromObject(cfg, k_legacy_keys[lk]);
            cJSON_DeleteItemFromObject(cfg, "pollSeconds");
            cJSON_DeleteItemFromObject(cfg, "mqttDevices");
            cJSON_DeleteItemFromObject(cfg, "devices");

            int save_rc = config_store_save_mqtt_json(cfg);
            cJSON_Delete(cfg);
            cJSON_Delete(params);
            if (save_rc != 0) {
                send_json_error(client_fd, 500, "Could not save the MQTT settings (flash full?)");
                close(client_fd);
                return 1;
            }

            daemon_sync_mqtt();

            cJSON *resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "ok", 1);
            cJSON_AddStringToObject(resp, "message", "MQTT settings saved");
            send_cjson_resp(client_fd, 200, resp);
            cJSON_Delete(resp);
            close(client_fd);
            return 1;
        }
        send_json_error(client_fd, 400, "Invalid parameters");
        close(client_fd);
        return 1;
    }

    /* Route: POST /system or POST /api/system-action */
    if ((strcmp(clean_path, "/system") == 0 || strcmp(clean_path, "/api/system-action") == 0) && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        if (params) {
            cJSON *p_act = cJSON_GetObjectItemCaseSensitive(params, "action");
            if (p_act && cJSON_IsString(p_act) && strcmp(p_act->valuestring, "reboot") == 0) {
                if (ota_update_busy()) {
                    cJSON_Delete(params);
                    send_json_error(client_fd, 409, "Software update in progress. Manual reboot blocked.");
                    close(client_fd);
                    return 1;
                }
                cJSON_Delete(params);
                cJSON *resp = cJSON_CreateObject();
                cJSON_AddBoolToObject(resp, "ok", 1);
                cJSON_AddStringToObject(resp, "message", "Rebooting hub now");
                send_cjson_resp(client_fd, 200, resp);
                cJSON_Delete(resp);
                close(client_fd);
#ifndef TEST_HARNESS
                sync();
                system("/data/harmony/bin/safe_reboot.sh 2>/dev/null || (killall -9 hub_rf hub_btstack 2>/dev/null; /data/harmony/bin/hub_sync 2>/dev/null; sleep 2; reboot -f) 2>/dev/null &");
#else
                printf("[*] reboot (test build, not run)\n"); /* never reboot the test host */
#endif
                return 1;
            } else if (p_act && cJSON_IsString(p_act) && strcmp(p_act->valuestring, "factory_reset") == 0) {
                cJSON *p_conf = cJSON_GetObjectItemCaseSensitive(params, "confirm");
                if (p_conf && cJSON_IsString(p_conf) && strcmp(p_conf->valuestring, "RESET") == 0) {
                    cJSON_Delete(params);
                    /* The installers do not put the script here: say so instead of answering
                     * "initiated" for a reset that never runs */
                    if (access("/data/harmony/bin/factory_reset.sh", X_OK) != 0) {
                        send_json_error(client_fd, 500, "Factory reset script not installed on this Hub");
                        close(client_fd);
                        return 1;
                    }
                    cJSON *resp = cJSON_CreateObject();
                    cJSON_AddBoolToObject(resp, "ok", 1);
                    cJSON_AddStringToObject(resp, "message", "Factory reset initiated. Reverting to stock firmware and rebooting.");
                    send_cjson_resp(client_fd, 200, resp);
                    cJSON_Delete(resp);
                    close(client_fd);
                    system("/data/harmony/bin/factory_reset.sh --confirm 2>/dev/null &");
                    return 1;
                } else {
                    cJSON_Delete(params);
                    send_json_error(client_fd, 400, "Confirmation parameter 'RESET' required");
                    close(client_fd);
                    return 1;
                }
            } else if (p_act && cJSON_IsString(p_act) && strcmp(p_act->valuestring, "clean_stock_resources") == 0) {
                /* The delete cannot be undone: same kind of confirmation as factory_reset */
                cJSON *p_conf = cJSON_GetObjectItemCaseSensitive(params, "confirm");
                int confirmed = p_conf && cJSON_IsString(p_conf) && strcmp(p_conf->valuestring, "EMPTY") == 0;
                cJSON_Delete(params);
                if (!confirmed) {
                    send_json_error(client_fd, 400, "Confirmation required: send \"confirm\": \"EMPTY\"");
                    close(client_fd);
                    return 1;
                }
                /* Only after the stock import: the files are its source */
                if (!stock_import_done()) {
                    send_json_error(client_fd, 409, "The stock configuration is not imported yet: /data/resources kept");
                    close(client_fd);
                    return 1;
                }
                struct stat rst;
                int present = lstat(STOCK_RES_DIR, &rst) == 0;
                if (!present) {
                    if (errno != ENOENT) {
                        send_json_error(client_fd, 500, "/data/resources cannot be read");
                        close(client_fd);
                        return 1;
                    }
                } else if (!S_ISDIR(rst.st_mode)) {
                    /* A link would empty its target; the host tools remove only the link */
                    send_json_error(client_fd, 409, "/data/resources is not a directory: nothing removed");
                    close(client_fd);
                    return 1;
                } else if (get_stock_resources_kb() > 0 && !stock_resources_saved()) {
                    /* Checked after the link test: the fingerprint walk would follow a link */
                    send_json_error(client_fd, 409, "No saved copy of the stock files as they are now: run "
                                                    "tools/backup_hub.py (build/deploy_hub.py runs it) first; "
                                                    "/data/resources kept");
                    close(client_fd);
                    return 1;
                }
                uint64_t total_kb = 0, free_before_kb = 0, free_after_kb = 0;
                get_disk_space_kb(&total_kb, &free_before_kb);
                /* -1: the directory could not be opened, also an error */
                int failed = present ? empty_directory_contents(STOCK_RES_DIR) : 0;
                sync();
                uint64_t after_kb = get_stock_resources_kb();
                if (failed != 0 || after_kb > 0) {
                    send_json_error(client_fd, 500, "Some files in /data/resources could not be removed");
                    close(client_fd);
                    return 1;
                }
                /* What the flash really got back (compressed), not the files' apparent size */
                get_disk_space_kb(&total_kb, &free_after_kb);
                uint64_t freed_kb = free_after_kb > free_before_kb ? free_after_kb - free_before_kb : 0;

                cJSON *resp = cJSON_CreateObject();
                cJSON_AddBoolToObject(resp, "ok", 1);
                cJSON_AddNumberToObject(resp, "freed_kb", (double)freed_kb);
                cJSON_AddNumberToObject(resp, "stock_resources_kb", (double)after_kb);
                cJSON_AddStringToObject(resp, "message", "Stock resources emptied successfully");
                send_cjson_resp(client_fd, 200, resp);
                cJSON_Delete(resp);
                close(client_fd);
                return 1;
            } else if (p_act && cJSON_IsString(p_act) && strcmp(p_act->valuestring, "mqtt_publish") == 0) {
                cJSON *j_top = cJSON_GetObjectItemCaseSensitive(params, "topic");
                cJSON *j_pay = cJSON_GetObjectItemCaseSensitive(params, "payload");
                const char *top = (j_top && cJSON_IsString(j_top)) ? j_top->valuestring : "";
                const char *pay = (j_pay && cJSON_IsString(j_pay)) ? j_pay->valuestring : "";
                if (top[0]) {
                    hub_mqtt_publish_plain(top, pay[0] ? pay : "cmd");
                }
                cJSON_Delete(params);
                cJSON *resp = cJSON_CreateObject();
                cJSON_AddBoolToObject(resp, "ok", 1);
                send_cjson_resp(client_fd, 200, resp);
                cJSON_Delete(resp);
                close(client_fd);
                return 1;
            } else if (p_act && cJSON_IsString(p_act) && strcmp(p_act->valuestring, "auth") == 0) {
                cJSON *j_u = cJSON_GetObjectItemCaseSensitive(params, "authUsername");
                cJSON *j_p = cJSON_GetObjectItemCaseSensitive(params, "authPassword");
                const char *uname = (j_u && cJSON_IsString(j_u)) ? j_u->valuestring : "";
                const char *pass = (j_p && cJSON_IsString(j_p)) ? j_p->valuestring : "";
                if (strlen(uname) >= MAX_USERNAME_LEN || strchr(uname, ':') || strchr(uname, '\n') || strlen(pass) > 128) {
                    cJSON_Delete(params);
                    send_json_error(client_fd, 400, "Username must be shorter than 32 bytes (no ':' or line breaks), password at most 128 bytes");
                    close(client_fd);
                    return 1;
                }

                struct user_account ulist[MAX_HTTP_USERS];
                int curr_auth_enabled = 0;
                int ucnt = load_user_accounts(ulist, MAX_HTTP_USERS, &curr_auth_enabled);

                /* A request without authEnabled keeps the current state (a password change alone
                 * must not turn authentication off) */
                int new_auth_enabled = curr_auth_enabled;
                cJSON *j_en = cJSON_GetObjectItemCaseSensitive(params, "authEnabled");
                if (j_en) {
                    if (cJSON_IsBool(j_en)) new_auth_enabled = cJSON_IsTrue(j_en) ? 1 : 0;
                    else if (cJSON_IsNumber(j_en)) new_auth_enabled = (j_en->valueint != 0);
                    else if (cJSON_IsString(j_en)) {
                        new_auth_enabled = (strcmp(j_en->valuestring, "1") == 0 ||
                                            strcasecmp(j_en->valuestring, "true") == 0);
                    }
                }

                int admin_idx = -1;
                for (int i = 0; i < ucnt; i++) {
                    if (ulist[i].role == ROLE_ADMIN) {
                        admin_idx = i;
                        break;
                    }
                }

                if (admin_idx >= 0) {
                    int renaming = uname[0] && strcmp(uname, ulist[admin_idx].username) != 0;
                    if (renaming && !pass[0]) {
                        /* the stored digest covers the name: without the password the account
                         * could not log in under either name */
                        cJSON_Delete(params);
                        send_json_error(client_fd, 400, "Give the password to rename the admin account");
                        close(client_fd);
                        return 1;
                    }
                    if (renaming) {
                        /* a name another account has: refused before anything changes (its
                         * tokens would be taken for leftovers below) */
                        for (int i = 0; i < ucnt; i++) {
                            if (i != admin_idx && strcmp(ulist[i].username, uname) == 0) {
                                cJSON_Delete(params);
                                send_json_error(client_fd, 409, "Another account already has this name");
                                close(client_fd);
                                return 1;
                            }
                        }
                        /* tokens left by a deleted account of the new name must not come back */
                        int stale = http_tokens_remove_owner(uname);
                        if (stale < 0) {
                            cJSON_Delete(params);
                            if (stale == HTTP_TOKENS_BUSY) send_json_error(client_fd, 503, "The token list is busy, try again");
                            else send_json_error(client_fd, 500, "Old access tokens of this name could not be removed: " HTTP_TOKENS_UNREADABLE_MSG);
                            close(client_fd);
                            return 1;
                        }
                    }
                    if (renaming || pass[0]) http_auth_drop_user_sessions(ulist[admin_idx].username);
                    if (renaming) {
                        memset(ulist[admin_idx].username, 0, sizeof(ulist[admin_idx].username));
                        strncpy(ulist[admin_idx].username, uname, sizeof(ulist[admin_idx].username) - 1);
                    }
                    if (pass[0]) {
                        ulist[admin_idx].password[0] = '\0';
                        compute_ha1(ulist[admin_idx].username, HARMONY_AUTH_REALM, pass, ulist[admin_idx].ha1);
                    }
                } else if (pass[0] && ucnt < MAX_HTTP_USERS) {
                    const char *new_name = uname[0] ? uname : "admin";
                    for (int i = 0; i < ucnt; i++) {
                        if (strcmp(ulist[i].username, new_name) == 0) {
                            cJSON_Delete(params);
                            send_json_error(client_fd, 409, "Another account already has this name");
                            close(client_fd);
                            return 1;
                        }
                    }
                    int stale = http_tokens_remove_owner(new_name);
                    if (stale < 0) {
                        cJSON_Delete(params);
                        if (stale == HTTP_TOKENS_BUSY) send_json_error(client_fd, 503, "The token list is busy, try again");
                        else send_json_error(client_fd, 500, "Old access tokens of this name could not be removed: " HTTP_TOKENS_UNREADABLE_MSG);
                        close(client_fd);
                        return 1;
                    }
                    admin_idx = ucnt++;
                    memset(&ulist[admin_idx], 0, sizeof(ulist[admin_idx]));
                    strncpy(ulist[admin_idx].username, uname[0] ? uname : "admin", sizeof(ulist[admin_idx].username) - 1);
                    compute_ha1(ulist[admin_idx].username, HARMONY_AUTH_REALM, pass, ulist[admin_idx].ha1);
                    ulist[admin_idx].role = ROLE_ADMIN;
                }

                /* No admin account and no password given: never invent admin/admin (the web UI
                 * used to save the moment the checkbox was ticked, before the form was filled).
                 * Enabling needs an admin account first. */
                if (new_auth_enabled && admin_idx < 0) {
                    cJSON_Delete(params);
                    send_json_error(client_fd, 400, "Set the admin username and password before enabling authentication");
                    close(client_fd);
                    return 1;
                }

                if (!save_user_accounts(ulist, ucnt, new_auth_enabled)) {
                    cJSON_Delete(params);
                    send_json_error(client_fd, 500, "Could not save the user accounts (flash full?)");
                    close(client_fd);
                    return 1;
                }
                if (new_auth_enabled != curr_auth_enabled) http_auth_drop_all_sessions();
                cJSON_Delete(params);

                cJSON *resp = cJSON_CreateObject();
                cJSON_AddBoolToObject(resp, "ok", 1);
                cJSON_AddBoolToObject(resp, "auth_enabled", new_auth_enabled);
                cJSON_AddStringToObject(resp, "message", "Authentication settings updated");
                if (!curr_auth_enabled && new_auth_enabled) {
                    /* turned on: the accounts and tokens to check, but the admin set here */
                    http_auth_add_switch_check(resp, ulist, ucnt, admin_idx >= 0 ? ulist[admin_idx].username : NULL);
                }
                send_cjson_resp(client_fd, 200, resp);
                cJSON_Delete(resp);
                close(client_fd);
                return 1;
            }
            cJSON_Delete(params);
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* Route: GET /api/settings */
    if (strcmp(clean_path, "/api/settings") == 0 && strcmp(method, "GET") == 0) {
        struct hub_settings s;
        config_store_get_settings(&s);
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddStringToObject(resp, "hub_name", s.hub_name[0] ? s.hub_name : "Harmony Hub");
        cJSON_AddBoolToObject(resp, "dry_run", g_dry_run ? 1 : 0);
        cJSON_AddStringToObject(resp, "ntp_server", s.ntp_server);
        cJSON_AddStringToObject(resp, "timezone", s.timezone[0] ? s.timezone : "UTC");
        cJSON_AddStringToObject(resp, "led_mode", s.led_mode[0] ? s.led_mode : HUB_LED_DEFAULT_MODE);
        cJSON_AddBoolToObject(resp, "mdns_enabled", s.mdns_enabled ? 1 : 0);
        cJSON_AddBoolToObject(resp, "legacy_enabled", s.legacy_enabled ? 1 : 0);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/settings */
    if (strcmp(clean_path, "/api/settings") == 0 && strcmp(method, "POST") == 0) {
        cJSON *p = parse_request_params(body);
        struct hub_settings s;
        config_store_get_settings(&s);
        if (p) {
            cJSON *item = cJSON_GetObjectItemCaseSensitive(p, "dry_run");
            if (item) {
                if (cJSON_IsBool(item)) g_dry_run = cJSON_IsTrue(item) ? 1 : 0;
                else if (cJSON_IsNumber(item)) g_dry_run = (int)item->valuedouble ? 1 : 0;
                s.dry_run = 0;
            }
            item = cJSON_GetObjectItemCaseSensitive(p, "hub_name");
            if (item && cJSON_IsString(item) && item->valuestring && item->valuestring[0]) {
                strncpy(s.hub_name, item->valuestring, sizeof(s.hub_name) - 1);
                s.hub_name[sizeof(s.hub_name) - 1] = '\0';
            }
            item = cJSON_GetObjectItemCaseSensitive(p, "ntp_server");
            if (item && cJSON_IsString(item) && item->valuestring) {
                strncpy(s.ntp_server, item->valuestring, sizeof(s.ntp_server) - 1);
                s.ntp_server[sizeof(s.ntp_server) - 1] = '\0';
            }
            item = cJSON_GetObjectItemCaseSensitive(p, "timezone");
            if (item && cJSON_IsString(item) && item->valuestring && !timezone_is_known(item->valuestring)) {
                /* it used to be stored and silently run as Central European time */
                cJSON_Delete(p);
                send_json_error(client_fd, 400, "Unknown timezone: use a name from the list or a POSIX TZ string");
                close(client_fd);
                return 1;
            }
            if (item && cJSON_IsString(item) && item->valuestring) {
                strncpy(s.timezone, item->valuestring, sizeof(s.timezone) - 1);
                s.timezone[sizeof(s.timezone) - 1] = '\0';
            }
            item = cJSON_GetObjectItemCaseSensitive(p, "led_mode");
            if (item && cJSON_IsString(item) && item->valuestring) {
                strncpy(s.led_mode, item->valuestring, sizeof(s.led_mode) - 1);
                s.led_mode[sizeof(s.led_mode) - 1] = '\0';
            }
            item = cJSON_GetObjectItemCaseSensitive(p, "mdns_enabled");
            if (item) {
                if (cJSON_IsBool(item)) s.mdns_enabled = cJSON_IsTrue(item) ? 1 : 0;
                else if (cJSON_IsNumber(item)) s.mdns_enabled = item->valueint ? 1 : 0;
            }
            item = cJSON_GetObjectItemCaseSensitive(p, "legacy_enabled");
            if (item) {
                if (cJSON_IsBool(item)) s.legacy_enabled = cJSON_IsTrue(item) ? 1 : 0;
                else if (cJSON_IsNumber(item)) s.legacy_enabled = item->valueint ? 1 : 0;
            }
            int save_rc = config_store_save_settings(&s);
            cJSON_Delete(p);
            if (save_rc != 0) {
                send_json_error(client_fd, 500, "Could not save the settings (flash full?)");
                close(client_fd);
                return 1;
            }
            apply_hub_settings(&s);
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddStringToObject(resp, "hub_name", s.hub_name[0] ? s.hub_name : "Harmony Hub");
        cJSON_AddBoolToObject(resp, "dry_run", g_dry_run ? 1 : 0);
        cJSON_AddStringToObject(resp, "ntp_server", s.ntp_server);
        cJSON_AddStringToObject(resp, "timezone", s.timezone[0] ? s.timezone : "UTC");
        cJSON_AddStringToObject(resp, "led_mode", s.led_mode[0] ? s.led_mode : HUB_LED_DEFAULT_MODE);
        cJSON_AddBoolToObject(resp, "mdns_enabled", s.mdns_enabled ? 1 : 0);
        cJSON_AddBoolToObject(resp, "legacy_enabled", s.legacy_enabled ? 1 : 0);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* Route: GET / or GET /health */
    if (strcmp(clean_path, "/") == 0 || strcmp(clean_path, "/health") == 0) {
        cJSON *hj = cJSON_CreateObject();
        cJSON_AddStringToObject(hj, "status", "ok");
        cJSON_AddStringToObject(hj, "service", "hub_daemon");
        cJSON_AddStringToObject(hj, "version", HARMONY_VERSION);
        cJSON_AddStringToObject(hj, "min_app_version", "1.3.0");
        send_cjson_resp(client_fd, 200, hj);
        cJSON_Delete(hj);
        close(client_fd);
        return 1;
    }

    return 0;
}
