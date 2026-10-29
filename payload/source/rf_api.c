#define _GNU_SOURCE
#include "rf_api.h"
#include "rf_spi.h"
#include "rf_keymap.h"
#include "rf_ota.h"
#include "rf_sync.h"
#include "rf_pairing.h"
#include "rf_config_push.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

void *api_server_worker(void *arg) {
    (void)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return NULL;

    fcntl(srv, F_SETFD, FD_CLOEXEC);

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(RF_API_PORT);
    sin.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(srv, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        close(srv);
        return NULL;
    }
    listen(srv, 4);

    while (g_running) {
        int cli = accept(srv, NULL, NULL);
        if (cli < 0) {
            if (errno == EINTR) continue;
            break;
        }
        fcntl(cli, F_SETFD, FD_CLOEXEC);

        char buf[4096];
        ssize_t n = read(cli, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            if (strstr(buf, "GET /api/rf/pair") == buf || strstr(buf, "POST /api/rf/pair") == buf) {
                if (is_ota_busy()) {
                    const char *res = "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\":\"RF OTA in progress\",\"ok\":false}";
                    write(cli, res, strlen(res));
                } else {
                    int slot = strstr(buf, "slot=2") ? 2 : (strstr(buf, "slot=1") ? 1 : 0);
                    start_pairing_mode(slot);
                    const char *res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"pairing_started\"}";
                    write(cli, res, strlen(res));
                }
            } else if (strstr(buf, "GET /api/rf/stop-pair") == buf) {
                stop_pairing_mode();
                const char *res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"pairing_stopped\"}";
                write(cli, res, strlen(res));
            } else if (strstr(buf, "GET /api/rf/unpair") == buf || strstr(buf, "POST /api/rf/unpair") == buf) {
                if (is_ota_busy()) {
                    const char *res = "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\":\"RF OTA in progress\",\"ok\":false}";
                    write(cli, res, strlen(res));
                } else {
                    int slot = strstr(buf, "slot=2") ? 2 : (strstr(buf, "slot=1") ? 1 : 0);
                    unpair_device(slot);
                    const char *res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"unpaired\",\"ok\":true}";
                    write(cli, res, strlen(res));
                }
            } else if (strstr(buf, "GET /api/rf/pull-config") == buf || strstr(buf, "POST /api/rf/pull-config") == buf) {
                if (is_ota_busy()) {
                    const char *res = "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\":\"RF OTA in progress\",\"ok\":false}";
                    write(cli, res, strlen(res));
                } else {
                    int slot = strstr(buf, "slot=2") ? 2 : 1;
                    if (is_companion_slot((uint8_t)slot)) {
                        const char *res = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\":\"Companion remotes do not support config pull\",\"ok\":false}";
                        write(cli, res, strlen(res));
                    } else {
                        rf_frame_t pull_frame;
                    memset(&pull_frame, 0, sizeof(pull_frame));
                    pull_frame.report_id = 0x20;
                    pull_frame.sub_id = 0x12;
                    pull_frame.dev_slot = (uint8_t)slot;
                    pull_frame.seq = g_tx_seq++;
                    pull_frame.hdr_flag = 0x50 | (pull_frame.seq & 0x0F);
                    pull_frame.msg_type = RF_MSG_CONFIG_PULL_REQ;
                    rf_send_frame(&pull_frame);
                        char res[128];
                        int rlen = snprintf(res, sizeof(res),
                            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"pull_requested\",\"slot\":%d,\"ok\":true}", slot);
                        write(cli, res, rlen);
                    }
                }
            } else if (strstr(buf, "GET /api/rf/sleep") == buf || strstr(buf, "POST /api/rf/sleep") == buf) {
                if (is_ota_busy()) {
                    const char *res = "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\":\"RF OTA in progress\",\"ok\":false}";
                    write(cli, res, strlen(res));
                } else {
                    int slot = strstr(buf, "slot=2") ? 2 : (strstr(buf, "slot=1") ? 1 : 0);
                    int sent_count = 0;
                    for (int s = 1; s <= 2; s++) {
                        if (slot != 0 && slot != s) continue;
                        if (slot == 0 && !g_paired_remotes[s - 1].active) continue;
                        if (is_companion_slot((uint8_t)s)) continue;
                        rf_frame_t f;
                        memset(&f, 0, sizeof(f));
                        f.report_id = 0x20;
                        f.sub_id = 0x12;
                        f.dev_slot = (uint8_t)s;
                        f.seq = g_tx_seq++;
                        f.hdr_flag = 0x50 | (f.seq & 0x0F);
                        f.msg_type = RF_MSG_SLEEP_REQ;
                        printf("[*] API: Sending SLEEP_REQ to remote slot %d (seq=%d)\n", s, f.seq);
                        rf_send_frame_acked(&f, 500);
                        sent_count++;
                    }
                    char res[160];
                    int rlen = snprintf(res, sizeof(res),
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"sleep_dispatched\",\"slot\":%d,\"sent\":%d,\"ok\":true}",
                        slot, sent_count);
                    write(cli, res, rlen);
                }
            } else if (strstr(buf, "GET /api/rf/log-level") == buf || strstr(buf, "POST /api/rf/log-level") == buf) {
                char *lp = strstr(buf, "level=");
                if (lp) {
                    int lvl = atoi(lp + 6);
                    if (lvl >= 0 && lvl <= 3) {
                        g_log_level = lvl;
                        g_debug_logging = (g_log_level >= LOG_LEVEL_DEBUG);
                    }
                }
                char res[128];
                int rlen = snprintf(res, sizeof(res),
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"log_level\":%d,\"ok\":true}", g_log_level);
                write(cli, res, rlen);
            } else if (strstr(buf, "GET /api/rf/send?hex=") == buf) {
                if (is_ota_busy()) {
                    const char *res = "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\":\"RF OTA in progress\",\"ok\":false}";
                    write(cli, res, strlen(res));
                } else {
                    char *hex = strstr(buf, "hex=") + 4;
                    char *end = strchr(hex, ' ');
                    if (end) *end = '\0';
                    uint8_t raw[64] = {0};
                    size_t raw_len = 0;
                    while (*hex && *(hex + 1) && raw_len < sizeof(raw)) {
                        unsigned int byte_val = 0;
                        char hex_byte[3] = { hex[0], hex[1], '\0' };
                        if (sscanf(hex_byte, "%02x", &byte_val) == 1 || sscanf(hex_byte, "%02X", &byte_val) == 1) {
                            raw[raw_len++] = (uint8_t)byte_val;
                            hex += 2;
                        } else break;
                    }
                    if (raw_len > 0) {
                        printf("[*] API RF TX (%zu bytes): ", raw_len);
                        for (size_t i = 0; i < raw_len; i++) printf("%02X ", raw[i]);
                        printf("\n");
                        rf_write_cmd(raw, raw_len);
                    }
                    const char *res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"sent\"}";
                    write(cli, res, strlen(res));
                }
            } else if (strstr(buf, "/api/rf/push-icon") == buf || strstr(buf, "GET /api/rf/push-icon") == buf || strstr(buf, "POST /api/rf/push-icon") == buf) {
                if (is_ota_busy()) {
                    const char *res = "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\":\"RF OTA in progress\",\"ok\":false}";
                    write(cli, res, strlen(res));
                } else {
                    char id_str[32] = {0}, type_str[32] = "activity";
                    char *qid = strstr(buf, "id=");
                    if (qid) {
                        strncpy(id_str, qid + 3, sizeof(id_str) - 1);
                        char *amp = strpbrk(id_str, "& \r\n");
                        if (amp) *amp = '\0';
                    }
                    char *qtype = strstr(buf, "type=");
                    if (qtype) {
                        strncpy(type_str, qtype + 5, sizeof(type_str) - 1);
                        char *amp = strpbrk(type_str, "& \r\n");
                        if (amp) *amp = '\0';
                    }
                    int slot = strstr(buf, "slot=2") ? 2 : (strstr(buf, "slot=1") ? 1 : 0);
                    int pushed = 0;
                    for (int s = 1; s <= 2; s++) {
                        if (slot != 0 && slot != s) continue;
                        if (slot == 0 && !g_paired_remotes[s - 1].active) continue;
                        if (rf_push_custom_icon(s, type_str, id_str) == 0) pushed++;
                    }
                    char res[160];
                    int rlen = snprintf(res, sizeof(res),
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"icon_pushed\",\"id\":\"%s\",\"pushed\":%d,\"ok\":true}",
                        id_str, pushed);
                    write(cli, res, rlen);
                }
            } else if (strstr(buf, "GET /api/rf/push-config") == buf || strstr(buf, "POST /api/rf/push-config") == buf) {
                if (is_ota_busy()) {
                    const char *res = "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\":\"RF OTA in progress\",\"ok\":false}";
                    write(cli, res, strlen(res));
                } else {
                    g_config_version++;
                    if (g_config_version == 0) g_config_version = 1;
                    FILE *vf = fopen("/data/harmony/config_version", "w");
                    if (vf) { fprintf(vf, "%d\n", g_config_version); fclose(vf); }
                    pthread_t push_th;
                    pthread_create(&push_th, NULL, config_push_worker, NULL);
                    pthread_detach(push_th);
                    rf_notify_update_available(1);
                    rf_notify_update_available(2);
                    const char *res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"push_started\"}";
                    write(cli, res, strlen(res));
                }
            } else if (strstr(buf, "GET /api/rf/sync") == buf) {
                if (is_ota_busy()) {
                    const char *res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"ota_in_progress\",\"ok\":true}";
                    write(cli, res, strlen(res));
                } else {
                    char cur[32] = "-1";
                    get_current_activity(cur, sizeof(cur));
                    rf_send_hub_activity_sync(cur);
                    const char *res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"synced\"}";
                    write(cli, res, strlen(res));
                }
            } else if (strstr(buf, "GET /api/rf/remote-config") == buf) {
                int slot = strstr(buf, "slot=2") ? 2 : 1;
                char res_body[4096] = {0};
                if (is_companion_slot((uint8_t)slot)) {
                    snprintf(res_body, sizeof(res_body),
                        "{\"slot\":%d,\"model\":\"companion\",\"has_screen\":false,\"supports_ota\":false,\"in_sync\":true}",
                        slot);
                } else {
                    char path[64];
                    snprintf(path, sizeof(path), "/data/harmony/elite_slot%d_config.json", slot);
                    FILE *f = fopen(path, "r");
                    if (f) {
                        size_t rn = fread(res_body, 1, sizeof(res_body) - 1, f);
                        res_body[rn] = '\0';
                        fclose(f);
                    } else {
                        snprintf(res_body, sizeof(res_body),
                            "{\"slot\":%d,\"brightness\":2,\"timeout\":10,\"touchEnabled\":true,\"touch\":1,\"capButtonsEnabled\":true,\"cap_buttons\":1,\"gyroEnabled\":true,\"gyro\":1,\"gyroRate\":25,\"gyro_rate\":25,\"btnBacklight\":2,\"btn_backlight\":2,\"nightStart\":22,\"night_start\":22,\"nightEnd\":7,\"night_end\":7,\"navToggleKey\":139,\"nav_key\":139,\"navToggleMode\":1,\"nav_mode\":1,\"usbEth\":false,\"usbeth\":0,\"confirmActivity\":true,\"confirm_act\":1,\"touchSensitivity\":2,\"touch_sensitivity\":2,\"remoteMode\":0,\"remote_mode\":0,\"noScreen\":false,\"no_screen\":0,\"powerDebug\":false,\"power_debug\":0}",
                            slot);
                    }
                }
                char header[256];
                int hlen = snprintf(header, sizeof(header),
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", (int)strlen(res_body));
                write(cli, header, hlen);
                write(cli, res_body, strlen(res_body));
            } else if (strstr(buf, "POST /api/rf/remote-config") == buf) {
                if (is_ota_busy()) {
                    const char *res = "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\":\"RF OTA in progress\",\"ok\":false}";
                    write(cli, res, strlen(res));
                } else {
                    int slot = strstr(buf, "slot=2") ? 2 : 1;
                    if (is_companion_slot((uint8_t)slot)) {
                        const char *res = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\":\"Companion remotes do not have screen or motion configuration\",\"ok\":false}";
                        write(cli, res, strlen(res));
                    } else {
                        char *body_start = strstr(buf, "\r\n\r\n");
                    if (body_start) {
                        body_start += 4;
                        cJSON *root = cJSON_Parse(body_start);
                        if (root) {
                            cJSON *jb = cJSON_GetObjectItem(root, "brightness");
                            cJSON *jt = cJSON_GetObjectItem(root, "timeout");
                            cJSON *jtouch = cJSON_GetObjectItem(root, "touchEnabled");
                            if (!jtouch) jtouch = cJSON_GetObjectItem(root, "touch");
                            cJSON *jgyro = cJSON_GetObjectItem(root, "gyroEnabled");
                            if (!jgyro) jgyro = cJSON_GetObjectItem(root, "gyro");
                            cJSON *jnk = cJSON_GetObjectItem(root, "navToggleKey");
                            if (!jnk) jnk = cJSON_GetObjectItem(root, "nav_key");
                            cJSON *jnm = cJSON_GetObjectItem(root, "navToggleMode");
                            if (!jnm) jnm = cJSON_GetObjectItem(root, "nav_mode");
                            cJSON *jca = cJSON_GetObjectItem(root, "confirmActivity");
                            if (!jca) jca = cJSON_GetObjectItem(root, "confirm_act");
                            cJSON *jts = cJSON_GetObjectItem(root, "touchSensitivity");
                            if (!jts) jts = cJSON_GetObjectItem(root, "touch_sensitivity");
                            cJSON *jrm = cJSON_GetObjectItem(root, "remoteMode");
                            if (!jrm) jrm = cJSON_GetObjectItem(root, "remote_mode");
                            cJSON *jgr = cJSON_GetObjectItem(root, "gyroRate");
                            if (!jgr) jgr = cJSON_GetObjectItem(root, "gyro_rate");
                            cJSON *jbb = cJSON_GetObjectItem(root, "btnBacklight");
                            if (!jbb) jbb = cJSON_GetObjectItem(root, "btn_backlight");
                            cJSON *jns = cJSON_GetObjectItem(root, "nightStart");
                            if (!jns) jns = cJSON_GetObjectItem(root, "night_start");
                            cJSON *jne = cJSON_GetObjectItem(root, "nightEnd");
                            if (!jne) jne = cJSON_GetObjectItem(root, "night_end");
                            cJSON *jue = cJSON_GetObjectItem(root, "usbEth");
                            if (!jue) jue = cJSON_GetObjectItem(root, "usbeth");
                            cJSON *jpd = cJSON_GetObjectItem(root, "powerDebug");
                            if (!jpd) jpd = cJSON_GetObjectItem(root, "power_debug");
                            cJSON *jnscreen = cJSON_GetObjectItem(root, "noScreen");
                            if (!jnscreen) jnscreen = cJSON_GetObjectItem(root, "no_screen");
                            cJSON *jcap = cJSON_GetObjectItem(root, "capButtonsEnabled");
                            if (!jcap) jcap = cJSON_GetObjectItem(root, "cap_buttons");

                            int bright = jb ? jb->valueint : 2;
                            int timeout = jt ? jt->valueint : 10;
                            int touch = (jtouch && (cJSON_IsTrue(jtouch) || (cJSON_IsNumber(jtouch) && jtouch->valueint != 0))) ? 1 : 0;
                            int gyro = (jgyro && (cJSON_IsTrue(jgyro) || (cJSON_IsNumber(jgyro) && jgyro->valueint != 0))) ? 1 : (jgyro ? 0 : 1);
                            int nav_key = jnk ? jnk->valueint : 139;
                            int nav_mode = jnm ? jnm->valueint : 1;
                            int confirm_act = (jca && (cJSON_IsTrue(jca) || (cJSON_IsNumber(jca) && jca->valueint != 0))) ? 1 : 0;
                            int touch_sens = (jts && jts->valueint >= 1 && jts->valueint <= 3) ? jts->valueint : 2;
                            int remote_mode = 0;
                            int gyro_rate = (jgr && jgr->valueint > 0) ? jgr->valueint : 25;
                            int btn_backlight = (jbb && jbb->valueint >= 0 && jbb->valueint <= 2) ? jbb->valueint : 2;
                            int night_start = (jns && jns->valueint >= 0 && jns->valueint <= 23) ? jns->valueint : 22;
                            int night_end = (jne && jne->valueint >= 0 && jne->valueint <= 23) ? jne->valueint : 7;
                            int usbeth = (jue && (cJSON_IsTrue(jue) || (cJSON_IsNumber(jue) && jue->valueint != 0))) ? 1 : 0;
                            int power_debug = (jpd && (cJSON_IsTrue(jpd) || (cJSON_IsNumber(jpd) && jpd->valueint != 0))) ? 1 : 0;
                            int no_screen = (jnscreen && (cJSON_IsTrue(jnscreen) || (cJSON_IsNumber(jnscreen) && jnscreen->valueint != 0))) ? 1 : 0;
                            int cap_buttons = (jcap && (cJSON_IsFalse(jcap) || (cJSON_IsNumber(jcap) && jcap->valueint == 0))) ? 0 : 1;

                            /* Create normalized JSON root saving all fields */
                            cJSON *norm = cJSON_CreateObject();
                            cJSON_AddNumberToObject(norm, "slot", slot);
                            cJSON_AddNumberToObject(norm, "brightness", bright);
                            cJSON_AddNumberToObject(norm, "timeout", timeout);
                            cJSON_AddBoolToObject(norm, "touchEnabled", touch != 0);
                            cJSON_AddNumberToObject(norm, "touch", touch);
                            cJSON_AddBoolToObject(norm, "capButtonsEnabled", cap_buttons != 0);
                            cJSON_AddNumberToObject(norm, "cap_buttons", cap_buttons);
                            cJSON_AddBoolToObject(norm, "gyroEnabled", gyro != 0);
                            cJSON_AddNumberToObject(norm, "gyro", gyro);
                            cJSON_AddNumberToObject(norm, "navToggleKey", nav_key);
                            cJSON_AddNumberToObject(norm, "nav_key", nav_key);
                            cJSON_AddNumberToObject(norm, "navToggleMode", nav_mode);
                            cJSON_AddNumberToObject(norm, "nav_mode", nav_mode);
                            cJSON_AddBoolToObject(norm, "confirmActivity", confirm_act != 0);
                            cJSON_AddNumberToObject(norm, "confirm_act", confirm_act);
                            cJSON_AddNumberToObject(norm, "touchSensitivity", touch_sens);
                            cJSON_AddNumberToObject(norm, "touch_sensitivity", touch_sens);
                            cJSON_AddNumberToObject(norm, "remoteMode", remote_mode);
                            cJSON_AddNumberToObject(norm, "remote_mode", remote_mode);
                            cJSON_AddNumberToObject(norm, "gyroRate", gyro_rate);
                            cJSON_AddNumberToObject(norm, "gyro_rate", gyro_rate);
                            cJSON_AddNumberToObject(norm, "btnBacklight", btn_backlight);
                            cJSON_AddNumberToObject(norm, "btn_backlight", btn_backlight);
                            cJSON_AddNumberToObject(norm, "nightStart", night_start);
                            cJSON_AddNumberToObject(norm, "night_start", night_start);
                            cJSON_AddNumberToObject(norm, "nightEnd", night_end);
                            cJSON_AddNumberToObject(norm, "night_end", night_end);
                            cJSON_AddBoolToObject(norm, "usbEth", usbeth != 0);
                            cJSON_AddNumberToObject(norm, "usbeth", usbeth);
                            cJSON_AddBoolToObject(norm, "powerDebug", power_debug != 0);
                            cJSON_AddNumberToObject(norm, "power_debug", power_debug);
                            cJSON_AddBoolToObject(norm, "noScreen", no_screen != 0);
                            cJSON_AddNumberToObject(norm, "no_screen", no_screen);

                            char path[64];
                            snprintf(path, sizeof(path), "/data/harmony/elite_slot%d_config.json", slot);

                            /* Read existing config to compute diff for pending push banner */
                            char old_cfg[4096] = {0};
                            FILE *cur_f = fopen(path, "r");
                            if (cur_f) {
                                size_t cr = fread(old_cfg, 1, sizeof(old_cfg) - 1, cur_f);
                                old_cfg[cr] = '\0';
                                fclose(cur_f);
                            }
                            record_config_diff(slot, old_cfg, norm);

                            char *norm_str = cJSON_Print(norm);
                            cJSON_Delete(norm);
                            if (norm_str) {
                                FILE *f = fopen(path, "w");
                                if (f) {
                                    fputs(norm_str, f);
                                    fclose(f);
                                }
                                free(norm_str);
                            }

                            rf_send_settings_push_slot((uint8_t)slot, bright, timeout, touch, gyro, nav_key, nav_mode,
                                                       confirm_act, touch_sens, remote_mode, gyro_rate,
                                                       btn_backlight, night_start, night_end, usbeth, no_screen,
                                                       cap_buttons, power_debug);
                            cJSON_Delete(root);
                        }
                    }
                    const char *res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"saved_and_pushed\",\"ok\":true}";
                    write(cli, res, strlen(res));
                    }
                }
            } else if (strstr(buf, "GET /api/rf/remote-direct-buttons") == buf) {
                int slot = strstr(buf, "slot=2") ? 2 : 1;
                char path[64];
                snprintf(path, sizeof(path), "/data/harmony/elite_slot%d_direct_buttons.json", slot);
                char res_body[4096] = {0};
                FILE *f = fopen(path, "r");
                if (f) {
                    size_t rn = fread(res_body, 1, sizeof(res_body) - 1, f);
                    res_body[rn] = '\0';
                    fclose(f);
                } else {
                    snprintf(res_body, sizeof(res_body), "{\"page1\":[],\"page2\":[],\"buttons\":{}}");
                }
                char header[256];
                int hlen = snprintf(header, sizeof(header),
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", (int)strlen(res_body));
                write(cli, header, hlen);
                write(cli, res_body, strlen(res_body));
            } else if (strstr(buf, "POST /api/rf/remote-direct-buttons") == buf) {
                if (is_ota_busy()) {
                    const char *res = "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\":\"RF OTA in progress\",\"ok\":false}";
                    write(cli, res, strlen(res));
                } else {
                    int slot = strstr(buf, "slot=2") ? 2 : 1;
                    char *body_start = strstr(buf, "\r\n\r\n");
                    if (body_start) {
                        body_start += 4;
                        char path[64];
                        snprintf(path, sizeof(path), "/data/harmony/elite_slot%d_direct_buttons.json", slot);
                        FILE *f = fopen(path, "w");
                        if (f) {
                            fputs(body_start, f);
                            fclose(f);
                        }
                        if (is_slot_direct_mode(slot)) {
                            pthread_t push_th;
                            pthread_create(&push_th, NULL, config_push_worker, NULL);
                            pthread_detach(push_th);
                        }
                    }
                    const char *res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"saved\",\"ok\":true}";
                    write(cli, res, strlen(res));
                }
            } else if (strstr(buf, "GET /api/rf/ota-status") == buf) {
                pthread_mutex_lock(&g_ota_lock);
                char res_body[512];
                snprintf(res_body, sizeof(res_body),
                    "{\"in_progress\":%s,\"slot\":%d,\"status\":%d,\"sent_bytes\":%u,\"total_bytes\":%u,\"sent_chunks\":%u,\"total_chunks\":%u,\"error\":\"%s\"}",
                    g_rf_ota.in_progress ? "true" : "false",
                    g_rf_ota.target_slot,
                    g_rf_ota.status_code,
                    g_rf_ota.sent_bytes,
                    g_rf_ota.total_bytes,
                    g_rf_ota.sent_chunks,
                    g_rf_ota.total_chunks,
                    g_rf_ota.error_msg);
                pthread_mutex_unlock(&g_ota_lock);
                char header[256];
                int hlen = snprintf(header, sizeof(header),
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", (int)strlen(res_body));
                write(cli, header, hlen);
                write(cli, res_body, strlen(res_body));
            } else if (strstr(buf, "POST /api/rf/ota-start") == buf) {
                int slot = strstr(buf, "slot=2") ? 2 : 1;
                if (is_companion_slot((uint8_t)slot)) {
                    char res[256];
                    int rlen = snprintf(res, sizeof(res),
                        "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"unsupported\",\"slot\":%d,\"error\":\"Companion remotes do not support OTA\",\"ok\":false}", slot);
                    write(cli, res, rlen);
                } else {
                    int started = 0;
                    int busy_slot = 0;
                    pthread_mutex_lock(&g_ota_lock);
                if (!g_rf_ota.in_progress) {
                    g_rf_ota.in_progress = true;
                    g_rf_ota.target_slot = slot;
                    g_rf_ota.status_code = 1;
                    g_rf_ota.error_msg[0] = '\0';
                    pthread_t ota_th;
                    if (pthread_create(&ota_th, NULL, rf_ota_sender_worker, (void *)(uintptr_t)slot) == 0) {
                        pthread_detach(ota_th);
                        started = 1;
                    } else {
                        g_rf_ota.in_progress = false;
                        g_rf_ota.status_code = -1;
                    }
                } else {
                    busy_slot = g_rf_ota.target_slot;
                }
                pthread_mutex_unlock(&g_ota_lock);
                    if (started) {
                        char res[128];
                        int rlen = snprintf(res, sizeof(res),
                            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"started\",\"slot\":%d,\"ok\":true}", slot);
                        write(cli, res, rlen);
                    } else {
                        char res[256];
                        int rlen = snprintf(res, sizeof(res),
                            "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"already_in_progress\",\"slot\":%d,\"error\":\"RF OTA already in progress on slot %d\",\"ok\":false}", busy_slot, busy_slot);
                        write(cli, res, rlen);
                    }
                }
            } else {
                /* Status endpoint */
                char addr1_str[16] = "None";
                char addr2_str[16] = "None";
                if (g_paired_remotes[0].rf_addr[0] || g_paired_remotes[0].rf_addr[1] || g_paired_remotes[0].rf_addr[2] || g_paired_remotes[0].rf_addr[3]) {
                    snprintf(addr1_str, sizeof(addr1_str), "%02X%02X%02X%02X",
                             g_paired_remotes[0].rf_addr[0], g_paired_remotes[0].rf_addr[1], g_paired_remotes[0].rf_addr[2], g_paired_remotes[0].rf_addr[3]);
                }
                if (g_paired_remotes[1].rf_addr[0] || g_paired_remotes[1].rf_addr[1] || g_paired_remotes[1].rf_addr[2] || g_paired_remotes[1].rf_addr[3]) {
                    snprintf(addr2_str, sizeof(addr2_str), "%02X%02X%02X%02X",
                             g_paired_remotes[1].rf_addr[0], g_paired_remotes[1].rf_addr[1], g_paired_remotes[1].rf_addr[2], g_paired_remotes[1].rf_addr[3]);
                }

                bool is_comp1 = is_companion_slot(1);
                bool is_comp2 = is_companion_slot(2);
                const char *model1 = get_remote_model_name(g_paired_remotes[0].model);
                const char *model2 = get_remote_model_name(g_paired_remotes[1].model);
                if (strcmp(model1, "unknown") == 0 && g_paired_remotes[0].active) {
                    model1 = is_comp1 ? "smart_control" : "elite";
                }
                if (strcmp(model2, "unknown") == 0 && g_paired_remotes[1].active) {
                    model2 = is_comp2 ? "smart_control" : "elite";
                }

                char name1[48] = "Harmony Remote (Slot 1)";
                char name2[48] = "Harmony Elite (Slot 2)";
                if (strcmp(model1, "smart_control") == 0) {
                    snprintf(name1, sizeof(name1), "Harmony Smart Control (Slot 1)");
                } else if (strcmp(model1, "companion") == 0) {
                    snprintf(name1, sizeof(name1), "Harmony Companion (Slot 1)");
                } else if (g_paired_remotes[0].name[0]) {
                    snprintf(name1, sizeof(name1), "%s (Slot 1)", g_paired_remotes[0].name);
                }

                if (strcmp(model2, "smart_control") == 0) {
                    snprintf(name2, sizeof(name2), "Harmony Smart Control (Slot 2)");
                } else if (strcmp(model2, "companion") == 0) {
                    snprintf(name2, sizeof(name2), "Harmony Companion (Slot 2)");
                } else if (g_paired_remotes[1].name[0]) {
                    snprintf(name2, sizeof(name2), "%s (Slot 2)", g_paired_remotes[1].name);
                }

                if ((g_hub_rf_addr[0] | g_hub_rf_addr[1] | g_hub_rf_addr[2] | g_hub_rf_addr[3]) == 0) {
                    uint8_t q[7] = {0x10, 0xFF, 0x83, 0xB5, 0x03, 0x00, 0x00};
                    rf_write_cmd(q, sizeof(q));
                    usleep(30000);
                }

                char hub_rf_str[16] = "None";
                if (g_hub_rf_addr[0] || g_hub_rf_addr[1] || g_hub_rf_addr[2] || g_hub_rf_addr[3]) {
                    snprintf(hub_rf_str, sizeof(hub_rf_str), "%02X%02X%02X%02X",
                             g_hub_rf_addr[0], g_hub_rf_addr[1], g_hub_rf_addr[2], g_hub_rf_addr[3]);
                }

                bool sync_pending = (access("/data/harmony/rf_sync_pending.flag", F_OK) == 0);
                int pending_slot = 1;
                char pending_diff[256] = "Hub configuration changed. Push settings to sync.";
                FILE *pdf = fopen("/data/harmony/rf_sync_pending_details.json", "r");
                if (pdf) {
                    char dline[512] = {0};
                    if (fread(dline, 1, sizeof(dline) - 1, pdf) > 0) {
                        cJSON *pj = cJSON_Parse(dline);
                        if (pj) {
                            cJSON *ps = cJSON_GetObjectItem(pj, "slot");
                            cJSON *pd = cJSON_GetObjectItem(pj, "diff");
                            if (ps) pending_slot = ps->valueint;
                            if (pd && pd->valuestring) snprintf(pending_diff, sizeof(pending_diff), "%s", pd->valuestring);
                            cJSON_Delete(pj);
                        }
                    }
                    fclose(pdf);
                }

                if (sync_pending) {
                    if ((pending_slot == 1 && is_comp1) || (pending_slot == 2 && is_comp2)) {
                        sync_pending = false;
                    }
                    if ((!g_paired_remotes[0].active || is_comp1) && (!g_paired_remotes[1].active || is_comp2)) {
                        sync_pending = false;
                    }
                }

                bool slot1_in_sync = is_comp1 ? true : (g_remote_versions[0].config_version == g_config_version && g_paired_remotes[0].active);
                bool slot2_in_sync = is_comp2 ? true : (g_remote_versions[1].config_version == g_config_version && g_paired_remotes[1].active);

                const char *ver1_str = (is_comp1 && strcmp(g_remote_versions[0].sw_version, "unknown") == 0) ? "N/A" : g_remote_versions[0].sw_version;
                const char *ver2_str = (is_comp2 && strcmp(g_remote_versions[1].sw_version, "unknown") == 0) ? "N/A" : g_remote_versions[1].sw_version;

                pthread_mutex_lock(&g_ota_lock);
                bool ota_in_prog = g_rf_ota.in_progress;
                int ota_slot = g_rf_ota.target_slot;
                int ota_status = g_rf_ota.status_code;
                uint32_t ota_sent_b = g_rf_ota.sent_bytes;
                uint32_t ota_tot_b = g_rf_ota.total_bytes;
                uint16_t ota_sent_c = g_rf_ota.sent_chunks;
                uint16_t ota_tot_c = g_rf_ota.total_chunks;
                char ota_err[64];
                snprintf(ota_err, sizeof(ota_err), "%s", g_rf_ota.error_msg);
                pthread_mutex_unlock(&g_ota_lock);

                char body[3072];
                int blen = snprintf(body, sizeof(body),
                    "{\"running\":true,\"ok\":true,\"fw_version\":\"v%d.00\",\"paired\":%s,\"rf_address\":\"%s\",\"hub_rf_address\":\"%s\",\"pairing_active\":%s,"
                    "\"sync_pending\":%s,\"sync_pending_slot\":%d,\"sync_pending_diff\":\"%s\",\"hub_config_version\":%d,"
                    "\"ota\":{\"in_progress\":%s,\"slot\":%d,\"status\":%d,\"sent_bytes\":%u,\"total_bytes\":%u,\"sent_chunks\":%u,\"total_chunks\":%u,\"error\":\"%s\"},"
                    "\"slots\":["
                    "{\"slot\":1,\"name\":\"%s\",\"model\":\"%s\",\"has_screen\":%s,\"supports_ota\":%s,\"active\":%s,\"in_sync\":%s,\"address\":\"%s\",\"rf_address\":\"%s\",\"sw_version\":\"%s\",\"config_version\":%d,\"battery\":{\"percent\":%d,\"charging\":%s,\"voltage_mv\":%d}},"
                    "{\"slot\":2,\"name\":\"%s\",\"model\":\"%s\",\"has_screen\":%s,\"supports_ota\":%s,\"active\":%s,\"in_sync\":%s,\"address\":\"%s\",\"rf_address\":\"%s\",\"sw_version\":\"%s\",\"config_version\":%d,\"battery\":{\"percent\":%d,\"charging\":%s,\"voltage_mv\":%d}}"
                    "]}",
                    g_fw_version, (g_has_paired_device || g_paired_remotes[0].active || g_paired_remotes[1].active) ? "true" : "false",
                    hub_rf_str, hub_rf_str,
                    g_pairing_mode ? "true" : "false",
                    sync_pending ? "true" : "false",
                    pending_slot, pending_diff, g_config_version,
                    ota_in_prog ? "true" : "false", ota_slot, ota_status, ota_sent_b, ota_tot_b, ota_sent_c, ota_tot_c, ota_err,
                    name1, model1, is_comp1 ? "false" : "true", is_comp1 ? "false" : "true",
                    g_paired_remotes[0].active ? "true" : "false", slot1_in_sync ? "true" : "false", addr1_str, addr1_str,
                    ver1_str, is_comp1 ? 0 : g_remote_versions[0].config_version,
                    g_remote_batteries[0].percent, g_remote_batteries[0].charging ? "true" : "false", g_remote_batteries[0].voltage_mv,
                    name2, model2, is_comp2 ? "false" : "true", is_comp2 ? "false" : "true",
                    g_paired_remotes[1].active ? "true" : "false", slot2_in_sync ? "true" : "false", addr2_str, addr2_str,
                    ver2_str, is_comp2 ? 0 : g_remote_versions[1].config_version,
                    g_remote_batteries[1].percent, g_remote_batteries[1].charging ? "true" : "false", g_remote_batteries[1].voltage_mv);

                char header[256];
                int hlen = snprintf(header, sizeof(header),
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", blen);
                write(cli, header, hlen);
                write(cli, body, blen);
            }
        }
        close(cli);
    }
    close(srv);
    return NULL;
}
