#define _GNU_SOURCE
#include "rf_sync.h"
#include "rf_spi.h"
#include "rf_keymap.h"
#include "rf_ota.h"
#include "harmony_version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SYNC_DEDUP_MS 2000
static char g_last_sync_act[32] = "";
static uint64_t g_last_sync_ms = 0;

void publish_remote_battery_mqtt(uint8_t slot, uint8_t pct, uint8_t chg, uint16_t mv) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return;
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(DAEMON_PORT);
    sin.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        close(s);
        return;
    }

    char body[512];
    snprintf(body, sizeof(body),
        "{\"action\":\"mqtt_publish\",\"topic\":\"harmony/hub/remote/%d/battery\","
        "\"payload\":\"{\\\"battery\\\":%d,\\\"charging\\\":%s,\\\"voltage\\\":%.2f}\",\"retain\":1}",
        slot, pct, chg ? "true" : "false", (float)mv / 1000.0f);

    char req[768];
    int rlen = snprintf(req, sizeof(req),
        "POST /api/action HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
        DAEMON_PORT, (int)strlen(body), body);
    write(s, req, rlen);
    close(s);
}

void record_config_diff(int slot, const char *old_json_str, cJSON *norm) {
    if (is_companion_slot((uint8_t)slot)) return;
    char diff[256] = {0};
    int diff_len = 0;
    cJSON *old_root = (old_json_str && *old_json_str) ? cJSON_Parse(old_json_str) : NULL;

    const char *keys[] = {
        "brightness", "timeout", "touch", "gyro", "nav_key", "nav_mode",
        "btn_backlight", "night_start", "night_end", "remote_mode", "usbeth", "no_screen",
        "cap_buttons"
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        cJSON *nitem = cJSON_GetObjectItem(norm, keys[i]);
        if (!nitem) continue;
        int nval = nitem->valueint;
        int oval = -999;
        if (old_root) {
            cJSON *oitem = cJSON_GetObjectItem(old_root, keys[i]);
            if (oitem) oval = oitem->valueint;
        }
        if (oval != nval && oval != -999) {
            char chunk[64];
            snprintf(chunk, sizeof(chunk), "%s%s: %d->%d", diff_len ? ", " : "", keys[i], oval, nval);
            strncat(diff, chunk, sizeof(diff) - diff_len - 1);
            diff_len = strlen(diff);
        }
    }
    if (old_root) cJSON_Delete(old_root);

    if (diff_len == 0) {
        snprintf(diff, sizeof(diff), "Remote Slot %d settings modified", slot);
    }

    FILE *df = fopen("/data/harmony/rf_sync_pending_details.json", "w");
    if (df) {
        fprintf(df, "{\"slot\":%d,\"diff\":\"%s\"}\n", slot, diff);
        fclose(df);
    }
    FILE *flag = fopen("/data/harmony/rf_sync_pending.flag", "w");
    if (flag) {
        fprintf(flag, "%ld\n", (long)time(NULL));
        fclose(flag);
    }
}

void save_remote_config_backup(uint8_t slot, const uint8_t *p) {
    if (slot < 1 || slot > 2) slot = 1;
    char path[64];
    snprintf(path, sizeof(path), "/data/harmony/elite_slot%d_config.json", slot);
    cJSON *root = cJSON_CreateObject();
    if (!root) return;

    int timeout = (int)p[1] | ((int)p[16] << 8);
    if (timeout <= 0) timeout = (int)p[1];
    if (timeout <= 0) timeout = 10;

    int gyro = (p[11] != 0);
    int gyro_rate = p[12] > 0 ? p[12] : 25;
    int btn_backlight = p[13];
    int night_start = p[14];
    int night_end = p[15];

    cJSON_AddNumberToObject(root, "slot", slot);
    cJSON_AddNumberToObject(root, "brightness", p[0]);
    cJSON_AddNumberToObject(root, "timeout", timeout);
    cJSON_AddBoolToObject(root, "touchEnabled", p[2] != 0);
    cJSON_AddNumberToObject(root, "touch", p[2] != 0 ? 1 : 0);
    uint16_t nk = (uint16_t)p[3] | ((uint16_t)p[4] << 8);
    cJSON_AddNumberToObject(root, "navToggleKey", nk);
    cJSON_AddNumberToObject(root, "nav_key", nk);
    cJSON_AddNumberToObject(root, "navToggleMode", p[5]);
    cJSON_AddNumberToObject(root, "nav_mode", p[5]);
    cJSON_AddBoolToObject(root, "usbEth", p[6] != 0);
    cJSON_AddNumberToObject(root, "usbeth", p[6] != 0 ? 1 : 0);
    cJSON_AddBoolToObject(root, "confirmActivity", p[7] != 0);
    cJSON_AddNumberToObject(root, "confirm_act", p[7] != 0 ? 1 : 0);
    cJSON_AddNumberToObject(root, "version", p[8]);
    int ts = (p[9] >= 1 && p[9] <= 3) ? p[9] : 2;
    cJSON_AddNumberToObject(root, "touchSensitivity", ts);
    cJSON_AddNumberToObject(root, "touch_sensitivity", ts);
    cJSON_AddNumberToObject(root, "remoteMode", 0);
    cJSON_AddNumberToObject(root, "remote_mode", 0);
    cJSON_AddBoolToObject(root, "gyroEnabled", gyro != 0);
    cJSON_AddNumberToObject(root, "gyro", gyro != 0 ? 1 : 0);
    cJSON_AddNumberToObject(root, "gyroRate", gyro_rate);
    cJSON_AddNumberToObject(root, "gyro_rate", gyro_rate);
    cJSON_AddNumberToObject(root, "btnBacklight", btn_backlight);
    cJSON_AddNumberToObject(root, "btn_backlight", btn_backlight);
    cJSON_AddNumberToObject(root, "nightStart", night_start);
    cJSON_AddNumberToObject(root, "night_start", night_start);
    cJSON_AddNumberToObject(root, "nightEnd", night_end);
    cJSON_AddNumberToObject(root, "night_end", night_end);
    int cap_buttons = (p[17] == 0) ? 0 : 1;
    cJSON_AddBoolToObject(root, "capButtonsEnabled", cap_buttons != 0);
    cJSON_AddNumberToObject(root, "cap_buttons", cap_buttons);
    int power_debug = (p[18] == 1) ? 1 : 0;
    cJSON_AddBoolToObject(root, "powerDebug", power_debug != 0);
    cJSON_AddNumberToObject(root, "power_debug", power_debug);

    int lang_id = (p[19] <= 5) ? p[19] : 0;
    const char *lang_str = "en";
    if (lang_id == 1) lang_str = "de";
    else if (lang_id == 2) lang_str = "fr";
    else if (lang_id == 3) lang_str = "nl";
    else if (lang_id == 4) lang_str = "es";
    else if (lang_id == 5) lang_str = "it";
    cJSON_AddStringToObject(root, "language", lang_str);
    cJSON_AddNumberToObject(root, "language_id", lang_id);

    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (txt) {
        int need_write = 1;
        FILE *ef = fopen(path, "r");
        if (ef) {
            fseek(ef, 0, SEEK_END);
            long sz = ftell(ef);
            fseek(ef, 0, SEEK_SET);
            if (sz == (long)strlen(txt)) {
                char *cur = (char *)malloc((size_t)sz + 1);
                if (cur) {
                    if (fread(cur, 1, (size_t)sz, ef) == (size_t)sz) {
                        cur[sz] = '\0';
                        if (strcmp(cur, txt) == 0) {
                            need_write = 0;
                        }
                    }
                    free(cur);
                }
            }
            fclose(ef);
        }
        if (need_write) {
            FILE *f = fopen(path, "w");
            if (f) {
                fputs(txt, f);
                fclose(f);
                printf("[*] Saved remote config backup to %s\n", path);
            }
        }
        free(txt);
    }
}

void rf_build_settings_push(rf_frame_t *f, uint8_t slot, int bright, int timeout, int touch, int gyro,
                            int nav_key, int nav_mode, int confirm_act, int touch_sens,
                            int remote_mode, int gyro_rate, int btn_backlight,
                            int night_start, int night_end, int usbeth, int no_screen,
                            int cap_buttons, int power_debug, int language) {
    uint8_t payload[26] = {0};
    payload[0] = (uint8_t)(bright < 1 ? 1 : (bright > 5 ? 5 : bright));
    payload[1] = (uint8_t)(touch ? 1 : 0);
    payload[2] = (uint8_t)(gyro ? 1 : 0);
    payload[3] = (uint8_t)((timeout >> 8) & 0xFF);
    payload[4] = (uint8_t)(timeout & 0xFF);
    payload[5] = (uint8_t)(nav_key & 0xFF);
    payload[6] = (uint8_t)((nav_key >> 8) & 0xFF);
    payload[7] = (uint8_t)nav_mode;
    payload[8] = (uint8_t)(confirm_act ? 1 : 0);
    payload[9] = (uint8_t)(touch_sens >= 1 && touch_sens <= 3 ? touch_sens : 2);
    payload[10] = 0; /* remote_mode: always 0 (activity mode) */
    payload[11] = (uint8_t)(gyro_rate > 0 ? gyro_rate : 25);
    payload[12] = (uint8_t)(btn_backlight >= 0 && btn_backlight <= 2 ? btn_backlight : 2);
    payload[13] = (uint8_t)(night_start >= 0 && night_start <= 23 ? night_start : 22);
    payload[14] = (uint8_t)(night_end >= 0 && night_end <= 23 ? night_end : 7);
    payload[15] = (uint8_t)(usbeth ? 1 : 0);
    payload[16] = (uint8_t)g_config_version;
    payload[17] = (uint8_t)(no_screen ? 1 : 0);
    payload[18] = (uint8_t)(cap_buttons ? 1 : 2);
    payload[19] = (uint8_t)(power_debug ? 1 : 2);
    payload[20] = (uint8_t)(language >= 0 && language <= 5 ? language : 0);
    rf_build_frame(f, slot, RF_MSG_SETTINGS_PUSH, ++g_tx_seq, RF_FLAG_ACK_REQ, payload, 21);
}

void rf_send_settings_push_slot(uint8_t slot, int bright, int timeout, int touch, int gyro,
                               int nav_key, int nav_mode, int confirm_act, int touch_sens,
                               int remote_mode, int gyro_rate, int btn_backlight,
                               int night_start, int night_end, int usbeth, int no_screen,
                               int cap_buttons, int power_debug, int language) {
    if (g_rf_fd < 0 || !g_has_paired_device) return;
    rf_frame_t f;
    rf_build_settings_push(&f, slot, bright, timeout, touch, gyro, nav_key, nav_mode, confirm_act, touch_sens,
                           remote_mode, gyro_rate, btn_backlight, night_start, night_end, usbeth, no_screen,
                           cap_buttons, power_debug, language);
    rf_send_frame_acked(&f, 300);
}

void rf_send_hub_activity_sync_slot(const char *act_id, uint8_t seq, uint8_t slot, uint8_t is_query_reply) {
    if (g_rf_fd < 0 || !g_has_paired_device) return;
    if (is_companion_slot(slot)) return;

    char act_name[24] = {0};
    get_activity_name_by_id(act_id, act_name, sizeof(act_name));

    rf_frame_t f;
    uint8_t payload[25] = {0};
    if (strlen(act_id) <= 2 || strcmp(act_id, "-1") == 0) {
        strncpy((char *)&payload[0], act_id, 3);
        strncpy((char *)&payload[3], act_name, 21);
    } else {
        strncpy((char *)&payload[0], act_id, 10);
        strncpy((char *)&payload[10], act_name, 14);
    }
    payload[24] = is_query_reply; /* 1 = reply to SYNC_REQUEST (steady-state catchup), 0 = live transition */

    rf_build_frame(&f, slot ? slot : 0x01, RF_MSG_ACTIVITY_SYNC, seq, 0, payload, 25);
    rf_send_frame(&f);
}

void rf_send_hub_activity_sync_seq(const char *act_id, uint8_t seq) {
    if (g_rf_fd < 0 || !g_has_paired_device) return;

    for (int i = 0; i < MAX_PAIRED_REMOTES; i++) {
        if (g_paired_remotes[i].active && !g_paired_remotes[i].link_lost) {
            if (is_companion_slot(g_paired_remotes[i].slot)) continue;
            rf_send_hub_activity_sync_slot(act_id, seq, g_paired_remotes[i].slot, 0x00);
            usleep(10000);
        }
    }
}

void rf_send_hub_activity_sync(const char *act_id) {
    if (g_rf_fd < 0 || !g_has_paired_device) return;

    /* Dedup */
    uint64_t now = get_monotonic_ms();
    if (strcmp(act_id, g_last_sync_act) == 0 && (now - g_last_sync_ms) < SYNC_DEDUP_MS) return;
    strncpy(g_last_sync_act, act_id, sizeof(g_last_sync_act) - 1);
    g_last_sync_act[sizeof(g_last_sync_act) - 1] = '\0';
    g_last_sync_ms = now;

    rf_send_hub_activity_sync_seq(act_id, ++g_tx_seq);
}

uint8_t rf_check_update_flags(uint8_t slot) {
    uint8_t flags = 0;
    int idx = (slot == 2) ? 1 : 0;

    /* 1. Config update check: Hub config_version is newer than remote reported config_version */
    if (g_config_version > 0 && g_remote_versions[idx].config_version > 0 &&
        g_config_version > g_remote_versions[idx].config_version) {
        flags |= 0x01;
    }

    /* 2. OTA update check: Hub has OTA file in /cache with higher version than remote */
    const char *ota_path = "/cache/elite_ui.ota.gz";
    if (access(ota_path, R_OK) == 0) {
        FILE *vf = fopen("/cache/elite_ui.version", "r");
        if (vf) {
            char ota_ver[32] = {0};
            if (fgets(ota_ver, sizeof(ota_ver), vf)) {
                char *nl = strchr(ota_ver, '\r'); if (nl) *nl = '\0';
                nl = strchr(ota_ver, '\n'); if (nl) *nl = '\0';
                if (harmony_version_compare(ota_ver, g_remote_versions[idx].sw_version) > 0) {
                    flags |= 0x02;
                }
            }
            fclose(vf);
        }
    }
    return flags;
}

void rf_notify_update_available(uint8_t target_slot) {
    if (g_rf_fd < 0 || !g_has_paired_device) return;
    uint8_t flags = rf_check_update_flags(target_slot);
    if (!flags) return;

    rf_frame_t f;
    uint8_t payload[25] = {0};
    payload[0] = flags;
    payload[1] = g_config_version;
    FILE *vf = fopen("/cache/elite_ui.version", "r");
    if (vf) {
        unsigned int maj = 0, min = 0, pat = 0;
        if (fscanf(vf, "%u.%u.%u", &maj, &min, &pat) >= 2) {
            payload[2] = (uint8_t)maj;
            payload[3] = (uint8_t)min;
            payload[4] = (uint8_t)pat;
        }
        fclose(vf);
    }
    rf_build_frame(&f, target_slot ? target_slot : 0x01, RF_MSG_UPDATE_NOTIFY, ++g_tx_seq, 0, payload, 8);
    rf_send_frame(&f);
}

void rf_send_hub_time_sync_slot(uint8_t slot) {
    if (g_rf_fd < 0 || !g_has_paired_device) return;
    if (is_companion_slot(slot)) return;

    rf_frame_t f;
    uint8_t payload[26] = {0};
    uint32_t now_epoch = (uint32_t)time(NULL);

    /* 4 bytes epoch seconds (little-endian) */
    payload[0] = (uint8_t)(now_epoch & 0xFF);
    payload[1] = (uint8_t)((now_epoch >> 8) & 0xFF);
    payload[2] = (uint8_t)((now_epoch >> 16) & 0xFF);
    payload[3] = (uint8_t)((now_epoch >> 24) & 0xFF);

    /* Read /etc/TZ */
    FILE *ftz = fopen("/etc/TZ", "r");
    char tz[64] = "CET-1CEST,M3.5.0,M10.5.0/3";
    if (ftz) {
        if (fgets(tz, sizeof(tz), ftz)) {
            char *nl = strchr(tz, '\n');
            if (nl) *nl = '\0';
            char *cr = strchr(tz, '\r');
            if (cr) *cr = '\0';
        }
        fclose(ftz);
    }
    if (strlen(tz) > 15) {
        char *comma = strchr(tz, ',');
        if (comma) *comma = '\0';
    }
    uint8_t tz_len = (uint8_t)strlen(tz);
    if (tz_len > 15) tz_len = 15;
    payload[4] = tz_len;
    memcpy(&payload[5], tz, tz_len);

    /* Byte 19: Update notification flags */
    payload[19] = rf_check_update_flags(slot ? slot : 0x01);

    /* Config version embedded at byte 20 */
    payload[20] = g_config_version;

    /* Embed Hub RF address in bytes 21..24 */
    if (g_hub_rf_addr[0] || g_hub_rf_addr[1] || g_hub_rf_addr[2] || g_hub_rf_addr[3]) {
        memcpy(&payload[21], g_hub_rf_addr, 4);
    } else if (g_paired_addr[0] || g_paired_addr[1] || g_paired_addr[2] || g_paired_addr[3]) {
        memcpy(&payload[21], g_paired_addr, 4);
    }

    rf_build_frame(&f, slot ? slot : 0x01, RF_MSG_TIME_SYNC, ++g_tx_seq, 0, payload, 26);
    rf_send_frame(&f);
}

void rf_send_hub_time_sync(void) {
    if (g_rf_fd < 0 || !g_has_paired_device) return;

    for (int i = 0; i < MAX_PAIRED_REMOTES; i++) {
        if (g_paired_remotes[i].active && !g_paired_remotes[i].link_lost) {
            if (is_companion_slot(g_paired_remotes[i].slot)) continue;
            rf_send_hub_time_sync_slot(g_paired_remotes[i].slot);
            usleep(10000);
        }
    }
}

int rf_push_custom_icon_file(int target_slot, const char *file_path, const char *id_str) {
    if (target_slot < 1 || target_slot > 2) target_slot = 1;
    if (!id_str || !id_str[0] || !file_path || !file_path[0]) return -1;
    if (access(file_path, R_OK) != 0) {
        printf("[-] Icon push: file not found %s\n", file_path);
        return -1;
    }

    FILE *f = fopen(file_path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsz <= 0 || fsz > 16384) {
        fclose(f);
        printf("[-] Icon push: invalid file size %ld for %s\n", fsz, file_path);
        return -1;
    }

    uint8_t *img_buf = (uint8_t *)malloc((size_t)fsz);
    if (!img_buf) { fclose(f); return -1; }
    if (fread(img_buf, 1, (size_t)fsz, f) != (size_t)fsz) {
        free(img_buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    uint16_t crc = rf_crc16(img_buf, (size_t)fsz);
    uint32_t tot_bytes = (uint32_t)fsz;
    uint16_t tot_chunks = (uint16_t)((tot_bytes + 21) / 22);

    printf("[*] Starting RF Custom Icon push to slot %d: %s (%u bytes, %u chunks, CRC16=0x%04X)\n",
           target_slot, id_str, tot_bytes, tot_chunks, crc);

    /* 1. RF_MSG_ICON_START */
    rf_frame_t frame;
    uint8_t start_payload[25] = {0};
    uint8_t type_code = 0;
    start_payload[0] = type_code;
    start_payload[1] = (uint8_t)(tot_bytes & 0xFF);
    start_payload[2] = (uint8_t)((tot_bytes >> 8) & 0xFF);
    start_payload[3] = (uint8_t)(tot_chunks & 0xFF);
    start_payload[4] = (uint8_t)((tot_chunks >> 8) & 0xFF);
    strncpy((char *)&start_payload[5], id_str, 19);
    start_payload[4] = (uint8_t)((tot_chunks >> 8) & 0xFF);
    strncpy((char *)&start_payload[5], id_str, 19);

    int ack_res = -1;
    for (int attempt = 1; attempt <= 3 && g_running; attempt++) {
        rf_build_frame(&frame, (uint8_t)target_slot, RF_MSG_ICON_START, ++g_tx_seq, RF_FLAG_ACK_REQ, start_payload, 25);
        ack_res = rf_send_frame_acked(&frame, 800);
        if (ack_res == 0) break;
        usleep(100000);
    }

    if (ack_res != 0) {
        free(img_buf);
        printf("[-] Remote in slot %d did not ACK ICON_START (sleeping?)\n", target_slot);
        return -1;
    }

    /* 2. Stream chunks */
    uint16_t c_idx = 0;
    uint32_t sent = 0;
    while (c_idx < tot_chunks && g_running) {
        uint32_t rem = tot_bytes - sent;
        uint8_t chunk_len = (rem > 22) ? 22 : (uint8_t)rem;

        uint8_t chunk_payload[25] = {0};
        chunk_payload[0] = (uint8_t)(c_idx & 0xFF);
        chunk_payload[1] = (uint8_t)((c_idx >> 8) & 0xFF);
        chunk_payload[2] = chunk_len;
        memcpy(&chunk_payload[3], img_buf + sent, chunk_len);

        bool need_ack = ((c_idx + 1 == tot_chunks) || (c_idx % 4 == 0));
        uint8_t flags = need_ack ? RF_FLAG_ACK_REQ : 0;
        rf_build_frame(&frame, (uint8_t)target_slot, RF_MSG_ICON_CHUNK, ++g_tx_seq, flags, chunk_payload, (uint8_t)(3 + chunk_len));

        if (need_ack) {
            int retries = 0, ack_ok = -1;
            while (retries < 3) {
                if (rf_send_frame_acked(&frame, 800) == 0 && g_ack_status == 0x00) {
                    ack_ok = 0;
                    break;
                }
                retries++;
                usleep(25000);
            }
            if (ack_ok != 0) {
                free(img_buf);
                printf("[-] Icon chunk %u ACK timeout\n", c_idx);
                return -1;
            }
        } else {
            rf_send_frame(&frame);
        }

        sent += chunk_len;
        c_idx++;
        usleep(15000); /* 15 ms pace */
    }
    free(img_buf);

    /* 3. RF_MSG_ICON_END */
    uint8_t end_payload[25] = {0};
    end_payload[0] = (uint8_t)(crc & 0xFF);
    end_payload[1] = (uint8_t)((crc >> 8) & 0xFF);
    rf_build_frame(&frame, (uint8_t)target_slot, RF_MSG_ICON_END, ++g_tx_seq, RF_FLAG_ACK_REQ, end_payload, 2);

    if (rf_send_frame_acked(&frame, 2000) != 0 || g_ack_status != 0x00) {
        printf("[-] ICON_END verification failed on remote slot %d (status=0x%02X)\n", target_slot, g_ack_status);
        return -1;
    }

    printf("[+] RF Custom Icon %s transferred successfully to slot %d!\n", id_str, target_slot);
    return 0;
}

int rf_push_custom_icon(int target_slot, const char *type_str, const char *id_str) {
    if (!type_str || !type_str[0]) type_str = "activity";
    char file_path[256];
    snprintf(file_path, sizeof(file_path), "/data/harmony/custom_icons/%s_%s.png", type_str, id_str ? id_str : "");
    return rf_push_custom_icon_file(target_slot, file_path, id_str);
}

void *activity_sync_worker(void *arg) {
    (void)arg;
    char last_act[32] = "";

    /* Wait for pairing response from CC2544 */
    for (int i = 0; i < 20 && g_running && !g_has_paired_device; i++) {
        usleep(100000);
    }

    /* Seed last_act on startup to prevent unsolicited TX into empty air */
    get_current_activity(last_act, sizeof(last_act));

    while (g_running) {
        char cur_act[32] = "-1";
        get_current_activity(cur_act, sizeof(cur_act));
        if (strcmp(cur_act, last_act) != 0) {
            if (!is_ota_busy()) {
                strncpy(last_act, cur_act, sizeof(last_act) - 1);
                last_act[sizeof(last_act) - 1] = '\0';
                rf_send_hub_activity_sync_seq(cur_act, ++g_tx_seq);
            }
        }
        sleep(1);
    }
    return NULL;
}
