#define _GNU_SOURCE
#include "rf_state.h"
#include "rf_spi.h"
#include "rf_keymap.h"
#include "rf_ota.h"
#include "rf_sync.h"
#include "rf_pairing.h"
#include "rf_api.h"
#include "rf_config_push.h"
#include "rf_help.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include "cJSON.h"

/* RF_MSG_DEVICE_CMD_INDEX payload: [0..9] device id, [10] command index, [11..24] name prefix.
   The index comes from the last config push; the name prefix guards against devices.json
   having changed since then. */
static void enqueue_browsed_device_cmd(const uint8_t *payload) {
    char dev_id[8] = {0};
    char prefix[22] = {0};
    memcpy(dev_id, payload, 4);
    int cmd_idx = payload[4];
    memcpy(prefix, &payload[5], 20);

    FILE *fd = fopen("/data/harmony/config/devices.json", "r");
    if (!fd) return;
    fseek(fd, 0, SEEK_END);
    long sz = ftell(fd);
    fseek(fd, 0, SEEK_SET);
    char *buf = (sz > 0 && sz < 1000000) ? malloc(sz + 1) : NULL;
    if (!buf) { fclose(fd); return; }
    size_t n = fread(buf, 1, sz, fd);
    fclose(fd);
    if (n != (size_t)sz) { free(buf); return; }
    buf[n] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;

    cJSON *devs_arr = cJSON_IsArray(root) ? root : cJSON_GetObjectItem(root, "devices");
    const char *full_id = NULL;
    const char *cmd_name = NULL;
    cJSON *dev = NULL;
    cJSON_ArrayForEach(dev, devs_arr) {
        cJSON *jid = cJSON_GetObjectItem(dev, "id");
        if (!jid || !cJSON_IsString(jid) || strncmp(jid->valuestring, dev_id, 7) != 0) continue;
        full_id = jid->valuestring;
        cJSON *cmds = cJSON_GetObjectItem(dev, "commands");
        int c = 0;
        cJSON *cmd = NULL;
        cJSON_ArrayForEach(cmd, cmds) {
            cJSON *jn = cJSON_IsString(cmd) ? cmd : cJSON_GetObjectItem(cmd, "name");
            if (jn && cJSON_IsString(jn) && strncmp(jn->valuestring, prefix, 20) == 0) {
                if (c == cmd_idx) { cmd_name = jn->valuestring; break; }
                if (!cmd_name) cmd_name = jn->valuestring; /* index moved: first name match */
            }
            c++;
        }
        break;
    }

    if (full_id && cmd_name) {
        printf("[*] Device browser command: dev=%s idx=%d -> %s\n", full_id, cmd_idx, cmd_name);
        enqueue_remote_device_cmd(full_id, cmd_name);
    } else {
        printf("[-] Device browser command not found: dev=%s idx=%d name=%s\n", dev_id, cmd_idx, prefix);
    }
    cJSON_Delete(root);
}

void handle_rf_frame(const rf_frame_t *f) {
    if (f->dev_slot == 1 || f->dev_slot == 2) {
        mark_remote_slot_active(f->dev_slot);
        if (g_paired_remotes[f->dev_slot - 1].model != REMOTE_MODEL_COMPANION) {
            g_paired_remotes[f->dev_slot - 1].model = REMOTE_MODEL_ELITE;
        }
    }
    switch (f->msg_type) {
        case RF_MSG_ACK: {
            ota_note_chunk_ack(f->dev_slot, f->payload);
            rf_signal_ack(f->payload[0], f->payload[1], f->payload);
            break;
        }

        case RF_MSG_BUTTON_PRESS: {
            uint8_t slot = (f->dev_slot & 0x0F);
            if (slot < 1 || slot > 2) slot = 1;
            uint16_t key_code = ((uint16_t)f->payload[0] << 8) | f->payload[1];
            uint8_t state = f->payload[2];
            char btn_name[18] = {0};
            memcpy(btn_name, &f->payload[3], 16);
            char dev_id[8] = {0};
            memcpy(dev_id, &f->payload[20], 5);

            const char *action = (state == 0x01) ? "press" : ((state == 0x02) ? "hold" : "release");
            const char *name = map_button_code(key_code >> 8, key_code & 0xFF);
            if (!name) name = btn_name[0] ? btn_name : "Unknown";

            printf("[*] BUTTON_PRESS: slot=%d code=0x%04X state=%s name=%s dev=%s\n",
                   slot, key_code, action, name, dev_id[0] ? dev_id : "none");
            enqueue_remote_button(slot, name, action, dev_id);

            static uint32_t s_last_sync_ms = 0;
            uint32_t now_ms = (uint32_t)get_monotonic_ms();
            if (!is_ota_busy() && (now_ms - s_last_sync_ms > 5000)) {
                s_last_sync_ms = now_ms;
                char cur[32] = "-1";
                get_current_activity(cur, sizeof(cur));
                rf_send_hub_activity_sync(cur);
            }
            break;
        }

        case RF_MSG_ACTIVITY_REQUEST: {
            char act_id[13] = {0};
            memcpy(act_id, f->payload, 12);
            printf("[*] ACTIVITY_REQUEST: %s\n", act_id);
            enqueue_remote_activity(act_id);
            break;
        }

        case RF_MSG_DEVICE_CMD_INDEX:
            enqueue_browsed_device_cmd(f->payload);
            break;

        case RF_MSG_HELP_REQUEST:
            rf_help_request(f);
            break;

        case RF_MSG_HELP_ANSWER:
            rf_help_answer(f);
            break;

        case RF_MSG_DEVICE_CMD_REQUEST: {
            uint8_t slot = (f->dev_slot & 0x0F);
            if (slot < 1 || slot > 2) slot = 1;
            char act_or_dev[13] = {0};
            memcpy(act_or_dev, f->payload, 10);
            uint8_t btn_idx = f->payload[10];

            bool handled = false;
            if (is_slot_direct_mode(slot)) {
                cJSON *dmap = get_remote_direct_map(slot);
                if (dmap) {
                    const char *page_key = (strcmp(act_or_dev, "page2") == 0) ? "page2" : "page1";
                    cJSON *sbtns = cJSON_GetObjectItem(dmap, page_key);
                    if (sbtns && cJSON_IsArray(sbtns)) {
                        cJSON *item = cJSON_GetArrayItem(sbtns, btn_idx);
                        if (item && cJSON_IsObject(item)) {
                            cJSON *jdev = cJSON_GetObjectItem(item, "deviceId");
                            cJSON *jcmd = cJSON_GetObjectItem(item, "command");
                            if (jdev && jcmd && jdev->valuestring && jcmd->valuestring) {
                                printf("[*] Direct Screen button dispatch: slot=%d page=%s idx=%d -> dev=%s cmd=%s\n",
                                       slot, page_key, btn_idx, jdev->valuestring, jcmd->valuestring);
                                enqueue_remote_device_cmd(jdev->valuestring, jcmd->valuestring);
                                handled = true;
                            }
                        }
                    }
                    cJSON_Delete(dmap);
                }
            } else {
                char dev_str[64] = {0};
                char cmd_str[64] = {0};
                if (get_remote_screen_button(slot, act_or_dev, btn_idx, dev_str, sizeof(dev_str), cmd_str, sizeof(cmd_str))) {
                    printf("[*] Screen button dispatch: act=%s idx=%d -> dev=%s cmd=%s\n",
                           act_or_dev, btn_idx, dev_str, cmd_str);
                    enqueue_remote_device_cmd(dev_str, cmd_str);
                    handled = true;
                }
            }

            if (!handled) {
                char dev_id[13] = {0};
                char cmd[17] = {0};
                memcpy(dev_id, f->payload, 12);
                memcpy(cmd, &f->payload[12], 16);
                printf("[*] DEVICE_CMD direct: dev=%s cmd=%s\n", dev_id, cmd);
                enqueue_remote_device_cmd(dev_id, cmd);
            }
            break;
        }

        case RF_MSG_SYNC_REQUEST: {
            if (is_ota_busy()) {
                printf("[*] RF OTA active - suppressing SYNC_REQUEST response\n");
                break;
            }
            uint8_t req_type = f->payload[0];
            uint8_t slot = f->dev_slot ? f->dev_slot : (g_remote_slot ? g_remote_slot : 0x01);
            printf("[*] SYNC_REQUEST: type=%d seq=%d slot=%d\n", req_type, f->seq, slot);

            char cur[32] = "-1";
            get_current_activity(cur, sizeof(cur));
            rf_send_hub_activity_sync_slot(cur, f->seq, slot, 0x01);

            if (req_type == 0x02) {
                rf_sync_cache_invalidate();
                usleep(50000);
                pthread_t push_th;
                pthread_create(&push_th, NULL, config_push_worker, NULL);
                pthread_detach(push_th);
            } else {
                usleep(10000);
                rf_send_hub_time_sync_slot_if_due(slot);
            }
            break;
        }

        case RF_MSG_BATTERY_STATUS: {
            uint8_t slot = f->dev_slot & 0x0F;
            if (slot < 1 || slot > 2) slot = 1;
            int idx = slot - 1;
            g_remote_batteries[idx].percent = f->payload[0];
            g_remote_batteries[idx].charging = f->payload[1];
            g_remote_batteries[idx].voltage_mv = (uint16_t)f->payload[2] | ((uint16_t)f->payload[3] << 8);
            g_remote_batteries[idx].last_update_ms = get_monotonic_ms();
            printf("[*] BATTERY_STATUS: Slot %d: %d%% (chg=%d, %umV)\n",
                   slot, g_remote_batteries[idx].percent, g_remote_batteries[idx].charging, g_remote_batteries[idx].voltage_mv);
            /* Kept in memory only: the remote reports every few minutes and MQTT retains the
             * last value; writing it to flash cost hundreds of writes a day */
            publish_remote_battery_mqtt(slot, g_remote_batteries[idx].percent, g_remote_batteries[idx].charging, g_remote_batteries[idx].voltage_mv);
            if (!is_ota_busy()) {
                rf_frame_t ack;
                rf_build_ack(&ack, slot, f->seq, 0x00);
                rf_send_frame(&ack);
            }
            break;
        }

        case RF_MSG_REMOTE_CONFIG_BACKUP: {
            uint8_t slot = f->dev_slot & 0x0F;
            if (slot < 1 || slot > 2) slot = 1;
            printf("[*] REMOTE_CONFIG_BACKUP: Slot %d\n", slot);
            save_remote_config_backup(slot, f->payload);
            if (!is_ota_busy()) {
                rf_frame_t ack;
                rf_build_ack(&ack, slot, f->seq, 0x00);
                rf_send_frame(&ack);
            }
            break;
        }

        case RF_MSG_VERSION_REPORT: {
            uint8_t slot = f->dev_slot & 0x0F;
            if (slot < 1 || slot > 2) slot = 1;
            uint8_t maj = f->payload[0];
            uint8_t min = f->payload[1];
            uint8_t pat = f->payload[2];
            uint8_t cfg_ver = f->payload[3];
            uint8_t caps = f->payload[4];
            int idx = slot - 1;
            char new_ver[sizeof(g_remote_versions[idx].sw_version)];
            snprintf(new_ver, sizeof(new_ver), "%u.%u.%u", maj, min, pat);
            /* The file is read back at start; the remote reports every 24 h and on every link,
             * so rewrite it only when something in it changed */
            bool ver_changed = strcmp(g_remote_versions[idx].sw_version, new_ver) != 0 ||
                               g_remote_versions[idx].config_version != cfg_ver ||
                               g_remote_versions[idx].caps != caps;
            snprintf(g_remote_versions[idx].sw_version, sizeof(g_remote_versions[idx].sw_version), "%s", new_ver);
            g_remote_versions[idx].config_version = cfg_ver;
            g_remote_versions[idx].caps = caps;
            g_remote_versions[idx].last_report_ms = get_monotonic_ms();

            char vpath[64];
            snprintf(vpath, sizeof(vpath), "/data/harmony/remote_slot%d_version.json", slot);
            if (ver_changed || access(vpath, F_OK) != 0) {
                FILE *vf = fopen(vpath, "w");
                if (vf) {
                    fprintf(vf, "{\"slot\":%d,\"version\":\"%s\",\"config_version\":%d,\"caps\":%d,\"timestamp\":%ld}\n",
                            slot, g_remote_versions[idx].sw_version, cfg_ver, caps, (long)time(NULL));
                    fclose(vf);
                }
            }
            printf("[*] VERSION_REPORT: Slot %d -> SW v%s, Config v%d, caps 0x%02X\n", slot, g_remote_versions[idx].sw_version, cfg_ver, caps);
            rf_frame_t ack;
            rf_build_ack(&ack, slot, f->seq, 0x00);
            rf_send_frame(&ack);

            uint8_t uflags = rf_check_update_flags(slot);
            if (uflags) {
                usleep(25000);
                rf_notify_update_available(slot);
            }
            break;
        }

        case RF_MSG_OTA_REQUEST: {
            uint8_t slot = f->dev_slot & 0x0F;
            if (slot < 1 || slot > 2) slot = g_remote_slot ? g_remote_slot : 1;
            printf("[*] Remote in slot %d requested RF OTA pull\n", slot);
            pthread_mutex_lock(&g_ota_lock);
            if (!g_rf_ota.in_progress) {
                g_rf_ota.in_progress = true;
                g_rf_ota.target_slot = slot;
                g_rf_ota.status_code = 1;
                g_rf_ota.error_msg[0] = '\0';
                pthread_t ota_th;
                if (pthread_create(&ota_th, NULL, rf_ota_sender_worker, (void *)(uintptr_t)slot) == 0) {
                    pthread_detach(ota_th);
                    rf_frame_t ack;
                    rf_build_ack(&ack, slot, f->seq, 0x00);
                    rf_send_frame(&ack);
                } else {
                    g_rf_ota.in_progress = false;
                    g_rf_ota.status_code = -1;
                    rf_frame_t ack;
                    rf_build_ack(&ack, slot, f->seq, 0x01);
                    rf_send_frame(&ack);
                }
            } else {
                rf_frame_t ack;
                rf_build_ack(&ack, slot, f->seq, 0x02); /* Busy */
                rf_send_frame(&ack);
            }
            pthread_mutex_unlock(&g_ota_lock);
            break;
        }

        case RF_MSG_UNPAIR_NOTIFY: {
            uint8_t slot = f->dev_slot & 0x0F;
            if (slot < 1 || slot > 2) slot = 1;
            RF_LOG(LOG_LEVEL_INFO, "[*] UNPAIR_NOTIFY received from Remote Slot %d\n", slot);
            uint8_t s = (slot == 2) ? 0x02 : 0x01;
            uint8_t unpair[7] = {0x10, 0xFF, 0x80, 0xB2, 0x03, s, 0x00};
            rf_write_cmd(unpair, sizeof(unpair));
            g_paired_remotes[slot - 1].active = false;
            memset(g_paired_remotes[slot - 1].rf_addr, 0, 4);
            if (!g_paired_remotes[0].active && !g_paired_remotes[1].active) {
                g_has_paired_device = false;
                memset(g_paired_addr, 0, sizeof(g_paired_addr));
            }
            rf_frame_t ack;
            rf_build_ack(&ack, slot, f->seq, 0x00);
            rf_send_frame(&ack);
            break;
        }

        case RF_MSG_PING: {
            rf_frame_t ack;
            rf_build_ack(&ack, f->dev_slot ? f->dev_slot : (g_remote_slot ? g_remote_slot : 0x01), f->seq, 0x00);
            rf_send_frame(&ack);
            break;
        }

        default:
            RF_LOG(LOG_LEVEL_DEBUG, "[*] RF unknown type 0x%02X\n", f->msg_type);
            break;
    }
}

static void sig_handler(int sig) {
    (void)sig;
    g_running = false;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* Enforce single instance across the entire OS to protect AR9344 SPI arbiter */
    int lock_fd = open("/var/run/hub_rf.lock", O_CREAT | O_RDWR, 0644);
    if (lock_fd >= 0) {
        if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                fprintf(stderr, "[-] FATAL: Another hub_rf instance is already running! Exiting immediately to protect SPI arbiter.\n");
                return 1;
            }
            fprintf(stderr, "[-] Warning: flock failed: %s (ignoring non-contention error)\n", strerror(errno));
        }
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Load debug logging flag from persistent config */
    if (access("/data/harmony/debug_logging.conf", F_OK) == 0) {
        g_debug_logging = 1;
        g_log_level = LOG_LEVEL_DEBUG;
    }

    printf("[+] Starting Harmony Hub RF Daemon (hub_rf)...\n");

    /* Open SPI driver */
    if (rf_spi_open() < 0) {
        fprintf(stderr, "[-] Warning: cannot open %s, attempting CC2544 flash...\n", RFSPI_DEV);
        flash_cc2544();
        if (rf_spi_open() < 0) {
            fprintf(stderr, "[-] Fatal: cannot open %s: %s\n", RFSPI_DEV, strerror(errno));
            return 1;
        }
    }
    printf("[+] Opened %s (fd=%d)\n", RFSPI_DEV, g_rf_fd);

    FILE *vf = fopen("/data/harmony/config_version", "r");
    if (vf) {
        int v = 1;
        if (fscanf(vf, "%d", &v) == 1 && v > 0) g_config_version = (uint8_t)v;
        fclose(vf);
    } else {
        g_config_version = 1;
        FILE *wf = fopen("/data/harmony/config_version", "w");
        if (wf) { fprintf(wf, "1\n"); fclose(wf); }
    }
    printf("[+] Hub RF config version: %d\n", g_config_version);

    /* Files of builds before the elite -> remote rename: per-slot version and config, and an
     * update image staged under the old names (an older deploy tool or update.sh) */
    rf_migrate_legacy_names();

    /* Load last known remote software and config versions from disk */
    for (int slot = 1; slot <= 2; slot++) {
        char vpath[64];
        snprintf(vpath, sizeof(vpath), "/data/harmony/remote_slot%d_version.json", slot);
        FILE *fv = fopen(vpath, "r");
        if (fv) {
            char vbuf[256] = {0};
            if (fread(vbuf, 1, sizeof(vbuf) - 1, fv) > 0) {
                cJSON *vroot = cJSON_Parse(vbuf);
                if (vroot) {
                    cJSON *jv = cJSON_GetObjectItem(vroot, "version");
                    cJSON *jcv = cJSON_GetObjectItem(vroot, "config_version");
                    cJSON *jcaps = cJSON_GetObjectItem(vroot, "caps");
                    if (cJSON_IsNumber(jcaps)) g_remote_versions[slot - 1].caps = (uint8_t)jcaps->valueint;
                    if (jv && jv->valuestring) {
                        snprintf(g_remote_versions[slot - 1].sw_version, sizeof(g_remote_versions[slot - 1].sw_version), "%s", jv->valuestring);
                    }
                    if (jcv) {
                        g_remote_versions[slot - 1].config_version = (uint8_t)jcv->valueint;
                    }
                    cJSON_Delete(vroot);
                }
            }
            fclose(fv);
        }

        /* The battery state is no longer kept on flash: remove a file left by older builds */
        char bpath[64];
        snprintf(bpath, sizeof(bpath), "/data/harmony/elite_slot%d_battery.json", slot);
        unlink(bpath);
    }

    /* 1. Initialize CC2544 message engine */
    uint8_t pkt_init[7] = {0x10, 0xFF, 0x80, 0x00, 0x00, 0x01, 0x00};
    rf_write_cmd(pkt_init, sizeof(pkt_init));
    usleep(50000);

    /* Close pairing lock to force normal operation with already-paired devices */
    uint8_t pkt_close_lock[7] = {0x10, 0xFF, 0x80, 0xB2, 0x02, 0x00, 0x00};
    rf_write_cmd(pkt_close_lock, sizeof(pkt_close_lock));
    usleep(50000);

    /* Set CC2544 active power mode */
    uint8_t pkt_power[7] = {0x20, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00};
    rf_write_cmd(pkt_power, sizeof(pkt_power));
    usleep(50000);

    pthread_t rx_th;
    if (pthread_create(&rx_th, NULL, rx_worker, NULL) != 0) {
        perror("pthread_create rx_worker");
        rf_spi_close();
        return 1;
    }

    pthread_t disp_th;
    if (pthread_create(&disp_th, NULL, dispatch_worker, NULL) != 0) {
        perror("pthread_create dispatch_worker");
        rf_spi_close();
        return 1;
    }

    pthread_t sync_th;
    if (pthread_create(&sync_th, NULL, activity_sync_worker, NULL) != 0) {
        perror("pthread_create activity_sync_worker");
        rf_spi_close();
        return 1;
    }

    /* Query initial state */
    uint8_t pkt_ver[7] = {0x10, 0xFF, 0x81, 0xF1, 0x01, 0x00, 0x00};
    rf_write_cmd(pkt_ver, sizeof(pkt_ver));
    usleep(50000);

    query_paired_devices();

    /* Start local REST API thread and Hub button listener */
    pthread_t api_th;
    if (pthread_create(&api_th, NULL, api_server_worker, NULL) != 0) {
        perror("pthread_create api_server_worker");
    } else {
        printf("[+] RF API listening on http://127.0.0.1:%d/api/rf/...\n", RF_API_PORT);
    }

    pthread_t btn_th;
    if (pthread_create(&btn_th, NULL, hub_button_worker, NULL) != 0) {
        perror("pthread_create hub_button_worker");
    }

    hub_led_restore();

    /* Optional CLI flags */
    if (argc > 1) {
        if (strcmp(argv[1], "--pair") == 0) {
            int slot = (argc > 2) ? atoi(argv[2]) : 0;
            start_pairing_mode(slot);
        }
        else if (strcmp(argv[1], "--unpair") == 0) {
            int slot = (argc > 2) ? atoi(argv[2]) : 0;
            unpair_device(slot);
        }
    }

    printf("[+] hub_rf ready and operational.\n");

    /* Main loop wait */
    while (g_running) {
        sleep(1);
    }

    printf("[+] Shutting down hub_rf...\n");
    rf_spi_close();
    return 0;
}
