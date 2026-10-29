#include "http_api_system.h"
#include "http_util.h"
#include "http_auth.h"
#include "http_api_custom_icons.h"
#include "hub_network_util.h"
#include "config_store.h"
#include "orchestrator.h"
#include "hub_mqtt.h"
#include "harmony_version.h"
#include "resource_cache.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
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

static void apply_hub_settings(const struct hub_settings *s) {
    if (!s) return;
    int lfd = open("/sys/class/input/input0/led", O_WRONLY);
    if (lfd >= 0) {
        const char *val = "1\n";
        if (strcasecmp(s->led_mode, "always_off") == 0 || strcasecmp(s->led_mode, "off") == 0) {
            val = "0\n";
        } else if (strcasecmp(s->led_mode, "flashing_fast") == 0 || strcasecmp(s->led_mode, "fast") == 0) {
            val = "2\n";
        } else if (strcasecmp(s->led_mode, "flashing_slow") == 0 || strcasecmp(s->led_mode, "slow") == 0 ||
                   strcasecmp(s->led_mode, "amber") == 0 || strcasecmp(s->led_mode, "orange") == 0) {
            val = "3\n";
        } else {
            val = "1\n";
        }
        ssize_t w = write(lfd, val, strlen(val));
        (void)w;
        close(lfd);
    }
    if (s->timezone[0]) {
        FILE *tzf = fopen("/etc/TZ", "w");
        if (tzf) {
            fprintf(tzf, "%s\n", s->timezone);
            fclose(tzf);
        }
        setenv("TZ", s->timezone, 1);
        tzset();
    }
    if (s->ntp_server[0] && !g_dry_run) {
        FILE *ntpf = fopen("/etc/ntp.conf", "w");
        if (ntpf) {
            fprintf(ntpf, "server %s\n", s->ntp_server);
            fclose(ntpf);
        }
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
        cJSON_AddBoolToObject(st, "updating", access("/tmp/hub_updating.flag", F_OK) == 0);

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
        cJSON_AddItemToObject(st, "mqtt", mj);

        http_ensure_icons_extracted();
        int icons_present = 0;
        struct stat st_icon;
        const char *icon_dir = "/tmp/www/assets/icons";
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

        cJSON *cfg = config_store_get_mqtt_json();
        if (cfg) {
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
        if (query) {
            if (strstr(query, "source=btstack")) src = "btstack";
            else if (strstr(query, "source=rf")) src = "rf";
            else if (strstr(query, "source=system")) src = "system";
            else if (strstr(query, "source=network")) src = "network";
        }

        const char *filepath = "/tmp/hub_daemon.log";
        if (strcmp(src, "btstack") == 0) filepath = "/tmp/hub-btstack.log";
        else if (strcmp(src, "rf") == 0) filepath = "/data/harmony/hub_rf.log";
        else if (strcmp(src, "network") == 0) filepath = "/tmp/hub-network.log";

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
            }

            cJSON *p_keep = cJSON_GetObjectItemCaseSensitive(params, "keep_password");
            int keep = (p_keep && (cJSON_IsTrue(p_keep) || (cJSON_IsString(p_keep) && strcmp(p_keep->valuestring, "1") == 0)));
            cJSON *p_pass = cJSON_GetObjectItemCaseSensitive(params, "password");
            if (!keep && p_pass && cJSON_IsString(p_pass) && p_pass->valuestring[0]) {
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

            cJSON *p_name = cJSON_GetObjectItemCaseSensitive(params, "name");
            if (p_name && cJSON_IsString(p_name) && p_name->valuestring[0]) {
                cJSON_DeleteItemFromObject(cfg, "name");
                cJSON_AddStringToObject(cfg, "name", p_name->valuestring);
            }

            cJSON *p_ka = cJSON_GetObjectItemCaseSensitive(params, "keepAlive");
            if (p_ka) {
                int kanum = cJSON_IsNumber(p_ka) ? p_ka->valueint : (cJSON_IsString(p_ka) ? atoi(p_ka->valuestring) : 60);
                cJSON_DeleteItemFromObject(cfg, "keepAlive");
                cJSON_AddNumberToObject(cfg, "keepAlive", kanum > 0 ? kanum : 60);
            }

            cJSON *p_ha = cJSON_GetObjectItemCaseSensitive(params, "haDiscovery");
            if (p_ha) {
                int ha_val = cJSON_IsTrue(p_ha) || (cJSON_IsNumber(p_ha) && p_ha->valueint > 0) || (cJSON_IsString(p_ha) && strcmp(p_ha->valuestring, "1") == 0);
                cJSON_DeleteItemFromObject(cfg, "haDiscovery");
                cJSON_AddBoolToObject(cfg, "haDiscovery", ha_val);
            }

            cJSON *p_btr = cJSON_GetObjectItemCaseSensitive(params, "btRemoteEvents");
            if (p_btr) {
                int btr_val = cJSON_IsTrue(p_btr) || (cJSON_IsNumber(p_btr) && p_btr->valueint > 0) || (cJSON_IsString(p_btr) && strcmp(p_btr->valuestring, "1") == 0);
                cJSON_DeleteItemFromObject(cfg, "btRemoteEvents");
                cJSON_AddBoolToObject(cfg, "btRemoteEvents", btr_val);
            }

            cJSON *p_btrt = cJSON_GetObjectItemCaseSensitive(params, "btRemoteTopic");
            if (p_btrt && cJSON_IsString(p_btrt)) {
                cJSON_DeleteItemFromObject(cfg, "btRemoteTopic");
                cJSON_AddStringToObject(cfg, "btRemoteTopic", p_btrt->valuestring);
            }

            cJSON *p_el = cJSON_GetObjectItemCaseSensitive(params, "eliteRemoteEvents");
            if (p_el) {
                int el_val = cJSON_IsTrue(p_el) || (cJSON_IsNumber(p_el) && p_el->valueint > 0) || (cJSON_IsString(p_el) && (strcmp(p_el->valuestring, "1") == 0 || strcasecmp(p_el->valuestring, "true") == 0));
                cJSON_DeleteItemFromObject(cfg, "eliteRemoteEvents");
                cJSON_AddBoolToObject(cfg, "eliteRemoteEvents", el_val);
            }

            cJSON *p_elt = cJSON_GetObjectItemCaseSensitive(params, "eliteRemoteTopic");
            if (p_elt && cJSON_IsString(p_elt)) {
                cJSON_DeleteItemFromObject(cfg, "eliteRemoteTopic");
                cJSON_AddStringToObject(cfg, "eliteRemoteTopic", p_elt->valuestring);
            }

            cJSON *p_s1e = cJSON_GetObjectItemCaseSensitive(params, "slot1RemoteEvents");
            if (p_s1e) {
                int s1_val = cJSON_IsTrue(p_s1e) || (cJSON_IsNumber(p_s1e) && p_s1e->valueint > 0) || (cJSON_IsString(p_s1e) && (strcmp(p_s1e->valuestring, "1") == 0 || strcasecmp(p_s1e->valuestring, "true") == 0));
                cJSON_DeleteItemFromObject(cfg, "slot1RemoteEvents");
                cJSON_AddBoolToObject(cfg, "slot1RemoteEvents", s1_val);
            }

            cJSON *p_s1t = cJSON_GetObjectItemCaseSensitive(params, "slot1RemoteTopic");
            if (p_s1t && cJSON_IsString(p_s1t)) {
                cJSON_DeleteItemFromObject(cfg, "slot1RemoteTopic");
                cJSON_AddStringToObject(cfg, "slot1RemoteTopic", p_s1t->valuestring);
            }

            cJSON *p_s2e = cJSON_GetObjectItemCaseSensitive(params, "slot2RemoteEvents");
            if (p_s2e) {
                int s2_val = cJSON_IsTrue(p_s2e) || (cJSON_IsNumber(p_s2e) && p_s2e->valueint > 0) || (cJSON_IsString(p_s2e) && (strcmp(p_s2e->valuestring, "1") == 0 || strcasecmp(p_s2e->valuestring, "true") == 0));
                cJSON_DeleteItemFromObject(cfg, "slot2RemoteEvents");
                cJSON_AddBoolToObject(cfg, "slot2RemoteEvents", s2_val);
            }

            cJSON *p_s2t = cJSON_GetObjectItemCaseSensitive(params, "slot2RemoteTopic");
            if (p_s2t && cJSON_IsString(p_s2t)) {
                cJSON_DeleteItemFromObject(cfg, "slot2RemoteTopic");
                cJSON_AddStringToObject(cfg, "slot2RemoteTopic", p_s2t->valuestring);
            }

            cJSON_DeleteItemFromObject(cfg, "pollSeconds");
            cJSON_DeleteItemFromObject(cfg, "mqttDevices");
            cJSON_DeleteItemFromObject(cfg, "devices");

            config_store_save_mqtt_json(cfg);
            cJSON_Delete(cfg);
            cJSON_Delete(params);

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
                if (access("/tmp/hub_updating.flag", F_OK) == 0) {
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
                sync();
                system("/data/harmony/bin/safe_reboot.sh 2>/dev/null || (killall -9 hub_rf hub_btstack 2>/dev/null; /data/harmony/bin/hub_sync 2>/dev/null; sleep 2; reboot -f) 2>/dev/null &");
                return 1;
            } else if (p_act && cJSON_IsString(p_act) && strcmp(p_act->valuestring, "factory_reset") == 0) {
                cJSON *p_conf = cJSON_GetObjectItemCaseSensitive(params, "confirm");
                if (p_conf && cJSON_IsString(p_conf) && strcmp(p_conf->valuestring, "RESET") == 0) {
                    cJSON_Delete(params);
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
            } else if (p_act && cJSON_IsString(p_act) && strcmp(p_act->valuestring, "mqtt_publish") == 0) {
                cJSON *j_top = cJSON_GetObjectItemCaseSensitive(params, "topic");
                cJSON *j_pay = cJSON_GetObjectItemCaseSensitive(params, "payload");
                const char *top = (j_top && cJSON_IsString(j_top)) ? j_top->valuestring : "";
                const char *pay = (j_pay && cJSON_IsString(j_pay)) ? j_pay->valuestring : "";
                if (top[0]) {
                    int pms = 0;
                    mqtt_dispatch_button_pulse_topic(NULL, NULL, pay[0] ? pay : "cmd", top, 1000, NULL, 0, &pms);
                }
                cJSON_Delete(params);
                cJSON *resp = cJSON_CreateObject();
                cJSON_AddBoolToObject(resp, "ok", 1);
                send_cjson_resp(client_fd, 200, resp);
                cJSON_Delete(resp);
                close(client_fd);
                return 1;
            } else if (p_act && cJSON_IsString(p_act) && strcmp(p_act->valuestring, "auth") == 0) {
                int new_auth_enabled = 0;
                cJSON *j_en = cJSON_GetObjectItemCaseSensitive(params, "authEnabled");
                if (j_en) {
                    if (cJSON_IsBool(j_en)) new_auth_enabled = cJSON_IsTrue(j_en) ? 1 : 0;
                    else if (cJSON_IsNumber(j_en)) new_auth_enabled = (j_en->valueint != 0);
                    else if (cJSON_IsString(j_en)) {
                        new_auth_enabled = (strcmp(j_en->valuestring, "1") == 0 ||
                                            strcasecmp(j_en->valuestring, "true") == 0);
                    }
                }

                cJSON *j_u = cJSON_GetObjectItemCaseSensitive(params, "authUsername");
                cJSON *j_p = cJSON_GetObjectItemCaseSensitive(params, "authPassword");
                const char *uname = (j_u && cJSON_IsString(j_u)) ? j_u->valuestring : "";
                const char *pass = (j_p && cJSON_IsString(j_p)) ? j_p->valuestring : "";

                struct user_account ulist[MAX_HTTP_USERS];
                int curr_auth_enabled = 0;
                int ucnt = load_user_accounts(ulist, MAX_HTTP_USERS, &curr_auth_enabled);

                int admin_idx = -1;
                for (int i = 0; i < ucnt; i++) {
                    if (ulist[i].role == ROLE_ADMIN) {
                        admin_idx = i;
                        break;
                    }
                }

                if (admin_idx >= 0) {
                    if (uname[0]) {
                        strncpy(ulist[admin_idx].username, uname, sizeof(ulist[admin_idx].username) - 1);
                        ulist[admin_idx].username[sizeof(ulist[admin_idx].username) - 1] = '\0';
                    }
                    if (pass[0]) {
                        strncpy(ulist[admin_idx].password, pass, sizeof(ulist[admin_idx].password) - 1);
                        ulist[admin_idx].password[sizeof(ulist[admin_idx].password) - 1] = '\0';
                        compute_ha1(ulist[admin_idx].username, HARMONY_AUTH_REALM, pass, ulist[admin_idx].ha1);
                    } else if (uname[0] && ulist[admin_idx].password[0]) {
                        compute_ha1(ulist[admin_idx].username, HARMONY_AUTH_REALM, ulist[admin_idx].password, ulist[admin_idx].ha1);
                    }
                } else {
                    if (ucnt < MAX_HTTP_USERS) {
                        admin_idx = ucnt++;
                        const char *final_user = uname[0] ? uname : "admin";
                        const char *final_pass = pass[0] ? pass : "admin";
                        strncpy(ulist[admin_idx].username, final_user, sizeof(ulist[admin_idx].username) - 1);
                        ulist[admin_idx].username[sizeof(ulist[admin_idx].username) - 1] = '\0';
                        strncpy(ulist[admin_idx].password, final_pass, sizeof(ulist[admin_idx].password) - 1);
                        ulist[admin_idx].password[sizeof(ulist[admin_idx].password) - 1] = '\0';
                        compute_ha1(final_user, HARMONY_AUTH_REALM, final_pass, ulist[admin_idx].ha1);
                        ulist[admin_idx].role = ROLE_ADMIN;
                    }
                }

                save_user_accounts(ulist, ucnt, new_auth_enabled);
                cJSON_Delete(params);

                cJSON *resp = cJSON_CreateObject();
                cJSON_AddBoolToObject(resp, "ok", 1);
                cJSON_AddBoolToObject(resp, "auth_enabled", new_auth_enabled);
                cJSON_AddStringToObject(resp, "message", "Authentication settings updated");
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
        cJSON_AddBoolToObject(resp, "dry_run", s.dry_run || g_dry_run);
        cJSON_AddStringToObject(resp, "ntp_server", s.ntp_server);
        cJSON_AddStringToObject(resp, "timezone", s.timezone[0] ? s.timezone : "UTC");
        cJSON_AddStringToObject(resp, "led_mode", s.led_mode[0] ? s.led_mode : "always_on");
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
                if (cJSON_IsBool(item)) s.dry_run = cJSON_IsTrue(item) ? 1 : 0;
                else if (cJSON_IsNumber(item)) s.dry_run = (int)item->valuedouble;
                g_dry_run = s.dry_run;
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
            if (item && cJSON_IsString(item) && item->valuestring) {
                strncpy(s.timezone, item->valuestring, sizeof(s.timezone) - 1);
                s.timezone[sizeof(s.timezone) - 1] = '\0';
            }
            item = cJSON_GetObjectItemCaseSensitive(p, "led_mode");
            if (item && cJSON_IsString(item) && item->valuestring) {
                strncpy(s.led_mode, item->valuestring, sizeof(s.led_mode) - 1);
                s.led_mode[sizeof(s.led_mode) - 1] = '\0';
            }
            config_store_save_settings(&s);
            apply_hub_settings(&s);
            cJSON_Delete(p);
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddStringToObject(resp, "hub_name", s.hub_name[0] ? s.hub_name : "Harmony Hub");
        cJSON_AddBoolToObject(resp, "dry_run", s.dry_run || g_dry_run);
        cJSON_AddStringToObject(resp, "ntp_server", s.ntp_server);
        cJSON_AddStringToObject(resp, "timezone", s.timezone[0] ? s.timezone : "UTC");
        cJSON_AddStringToObject(resp, "led_mode", s.led_mode[0] ? s.led_mode : "always_on");
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
