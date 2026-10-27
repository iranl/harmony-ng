#include "elite_rf.h"
#include "elite_ui.h"
#include "elite_power.h"
#include "elite_config.h"

int rf_send_frame(rf_frame_t *frame) {
    if (g_rf_fd < 0 || !frame) return -1;
    uint8_t out[32];
    memset(out, 0, sizeof(out));
    uint8_t slot = (g_remote_slot >= 1 && g_remote_slot <= 15) ? g_remote_slot : 0x01;
    out[0] = 0x20; /* Remote CC2544 uplink Report 0x20 (Device mode) */
    out[1] = 0x20 | (slot & 0x0F); /* 0x20 | pipe (Hub connection) */
    out[2] = 0x12; /* Sub-ID 0x12 */
    out[3] = (slot & 0x0F); /* Host sequence/pipe */
    out[4] = frame->hdr_flag ? frame->hdr_flag : (0x50 | (frame->seq & 0x0F));
    out[5] = frame->msg_type;
    out[6] = frame->flags;
    memcpy(&out[7], frame->payload, 25);

    if (g_log_level >= LOG_LEVEL_DEBUG) {
        printf("[*] RF TX (32 bytes Report 0x20): ");
        for (int i = 0; i < 32; i++) printf("%02X ", out[i]);
        printf("\n");
        fflush(stdout);
    }

    pthread_mutex_lock(&g_rf_write_mutex);
    int ret = write(g_rf_fd, out, 32);
    pthread_mutex_unlock(&g_rf_write_mutex);
    return ret;
}

void rf_send_sync_query(void) {
    if (g_rf_fd < 0) return;
    rf_frame_t f;
    uint8_t payload[25] = {0};
    payload[0] = 0x01; /* activity only */
    rf_build_frame(&f, g_remote_slot, RF_MSG_SYNC_REQUEST, ++g_tx_seq, 0, payload, 1);
    rf_send_frame(&f);
    printf("[*] RF TX SYNC_REQUEST seq=%d slot=%d (byte1=0x%02X)\n", f.seq, f.dev_slot & 0x0F, f.dev_slot);
    fflush(stdout);
}

void rf_send_full_sync_request(void) {
    if (g_rf_fd < 0 || g_is_syncing_config || g_is_ota_updating) return;
    uint64_t now = get_monotonic_ms();
    static uint64_t last_sync_req_ms = 0;
    if (now - last_sync_req_ms < 5000) return;
    last_sync_req_ms = now;

    rf_frame_t f;
    uint8_t payload[25] = {0};
    payload[0] = 0x02; /* full config */
    rf_build_frame(&f, g_remote_slot, RF_MSG_SYNC_REQUEST, ++g_tx_seq, 0, payload, 1);
    rf_send_frame(&f);
    printf("[*] RF TX FULL_SYNC_REQUEST seq=%d slot=%d (byte1=0x%02X)\n", f.seq, f.dev_slot & 0x0F, f.dev_slot);
    fflush(stdout);
}

void rf_wake_and_reconnect(void) {
    if (g_rf_fd < 0) return;
    uint8_t pkt_dongle[10] = {0x12, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    pthread_mutex_lock(&g_rf_write_mutex);
    if (write(g_rf_fd, pkt_dongle, sizeof(pkt_dongle)) < 0) {}
    pthread_mutex_unlock(&g_rf_write_mutex);
    usleep(5000);
    rf_send_sync_query();
}

static void *rf_pairing_worker(void *arg) {
    (void)arg;
    int attempts = 0;
    while (g_running && g_pairing_state == 1 && attempts < 30) {
        uint8_t pkt_pair[7] = {0x10, 0xFF, 0x80, 0xB2, 0x01, 0x01, 0x00};
        pthread_mutex_lock(&g_rf_write_mutex);
        if (write(g_rf_fd, pkt_pair, sizeof(pkt_pair)) < 0) {
            perror("write pkt_pair");
        }
        pthread_mutex_unlock(&g_rf_write_mutex);

        sleep(1);
        attempts++;
        if (g_pairing_state != 1) break;

        uint8_t q_hub[7] = {0x10, 0xFF, 0x83, 0xB5, 0x20, 0x00, 0x00};
        pthread_mutex_lock(&g_rf_write_mutex);
        if (write(g_rf_fd, q_hub, sizeof(q_hub)) < 0) {}
        pthread_mutex_unlock(&g_rf_write_mutex);
    }
    if (g_pairing_state == 1) {
        printf("[-] Pairing timed out after 30s\n");
        g_pairing_state = 3;
        render_screen();
    }
    return NULL;
}

void rf_start_pairing(void) {
    if (g_rf_fd < 0) return;
    g_pairing_state = 1;
    g_rf_link_connected = false;

    printf("[*] Initiating RF Pairing to Hub...\n");
    pthread_t th;
    if (pthread_create(&th, NULL, rf_pairing_worker, NULL) == 0) {
        pthread_detach(th);
    }
    fflush(stdout);
}

void rf_stop_pairing(void) {
    if (g_rf_fd < 0) return;
    printf("[*] Stopping pairing mode on Remote...\n");
    uint8_t pkt_close[7] = {0x10, 0xFF, 0x80, 0xB2, 0x02, 0x00, 0x00};
    if (write(g_rf_fd, pkt_close, sizeof(pkt_close)) < 0) {}
    g_pairing_state = 0;
    render_screen();
}

void rf_unpair(void) {
    if (g_rf_fd < 0) return;

    rf_frame_t unpair_f;
    memset(&unpair_f, 0, sizeof(unpair_f));
    unpair_f.report_id = 0x20;
    unpair_f.sub_id = 0x12;
    unpair_f.msg_type = RF_MSG_UNPAIR_NOTIFY;
    unpair_f.hdr_flag = 0x50 | (g_tx_seq & 0x0F);
    unpair_f.seq = g_tx_seq++;
    rf_send_frame(&unpair_f);
    usleep(30000);

    g_pairing_state = 0;
    g_rf_link_connected = false;

    printf("[*] Unpairing & removing bonding: entering test mode and clearing CC2544 stored device pipes...\n");
    pthread_mutex_lock(&g_rf_write_mutex);

    uint8_t enter_test[7] = {0x10, 0xFF, 0x80, 0xD0, 0x01, 0x00, 0x00};
    if (write(g_rf_fd, enter_test, sizeof(enter_test)) < 0) {}
    usleep(100000);

    uint8_t clr_pipe20[20] = {0x11, 0xFF, 0x82, 0xB5, 0x20, 0};
    if (write(g_rf_fd, clr_pipe20, sizeof(clr_pipe20)) < 0) {}
    usleep(100000);

    uint8_t clr_pipe30[20] = {0x11, 0xFF, 0x82, 0xB5, 0x30, 0};
    if (write(g_rf_fd, clr_pipe30, sizeof(clr_pipe30)) < 0) {}
    usleep(100000);

    uint8_t exit_test[7] = {0x10, 0xFF, 0x80, 0xD0, 0x00, 0x00, 0x00};
    if (write(g_rf_fd, exit_test, sizeof(exit_test)) < 0) {}
    usleep(100000);

    pthread_mutex_unlock(&g_rf_write_mutex);

    unlink(RF_CONFIG_FILE);
    unlink(ACTIVITY_JSON_FILE);

    g_has_hub_config = false;
    g_activity_count = 0;
    g_rf_activity_count = 0;
    g_rf_device_count = 0;
    g_rf_dev_cmd_count = 0;
    g_rf_button_count = 0;
    memset(g_activities, 0, sizeof(g_activities));
    memset(g_rf_activities, 0, sizeof(g_rf_activities));
    memset(g_rf_devices, 0, sizeof(g_rf_devices));
    memset(g_rf_buttons, 0, sizeof(g_rf_buttons));
    memset(g_paired_hub_addr, 0, sizeof(g_paired_hub_addr));
    g_active_activity_id[0] = '\0';
    g_active_activity_name[0] = '\0';
    g_config_version = 0;
    g_unpair_confirm_active = false;
    g_current_view = VIEW_NOT_PAIRED;

    printf("[+] Remote unpairing & bonding removal complete. Ready to pair to new hub.\n");
    render_screen();
}

void rf_send_button_event(uint16_t key_code, int value) {
    if (key_code == 0 || g_rf_fd < 0) return;
    if (value < 0 || value > 2) return;

    const char *short_name = rf_short_button_name(key_code);
    uint8_t state = (value == 1) ? 0x01 : ((value == 2) ? 0x02 : 0x00);

    rf_frame_t f;
    uint8_t payload[26] = {0};
    payload[0] = (uint8_t)(key_code >> 8);
    payload[1] = (uint8_t)(key_code & 0xFF);
    payload[2] = state;
    strncpy((char *)&payload[3], short_name, 12);
    rf_build_frame(&f, g_remote_slot, RF_MSG_BUTTON_PRESS, ++g_tx_seq, 0, payload, 15);

    rf_send_frame(&f);

    printf("[*] RF TX BUTTON_PRESS: 0x%04X %s %s\n", key_code,
           state == 0x01 ? "press" : (state == 0x02 ? "hold" : "release"), short_name);
}

void rf_send_activity_start(const char *act_id) {
    if (g_rf_fd < 0) return;

    rf_frame_t f;
    uint8_t payload[26] = {0};
    strncpy((char *)payload, act_id, 12);
    rf_build_frame(&f, g_remote_slot, RF_MSG_ACTIVITY_REQUEST, ++g_tx_seq, 0, payload, 12);

    rf_send_frame(&f);

    printf("[*] RF TX ACTIVITY_REQUEST: %s\n", act_id);
}

void rf_send_device_command(const char *dev_id, const char *cmd) {
    if (g_rf_fd < 0) return;

    rf_frame_t f;
    uint8_t payload[26] = {0};
    strncpy((char *)payload, dev_id, 10);
    strncpy((char *)&payload[10], cmd, 16);
    rf_build_frame(&f, g_remote_slot, RF_MSG_DEVICE_CMD_REQUEST, ++g_tx_seq, 0, payload, 26);

    rf_send_frame(&f);

    printf("[*] RF TX DEVICE_CMD: dev=%s cmd=%s\n", dev_id, cmd);
}

void rf_send_screen_button_press(const char *act_id, uint8_t btn_idx, const char *label) {
    if (g_rf_fd < 0) return;
    rf_frame_t f;
    uint8_t payload[25] = {0};
    strncpy((char *)&payload[0], act_id, 10);
    payload[10] = btn_idx;
    strncpy((char *)&payload[11], label, 12);
    rf_build_frame(&f, g_remote_slot, RF_MSG_DEVICE_CMD_REQUEST, ++g_tx_seq, 0x01, payload, sizeof(payload));
    rf_send_frame(&f);
    printf("[*] RF TX SCREEN_BTN: act=%s idx=%d label=%s\n", act_id, btn_idx, label);
}

int build_combined_activity_sequence(const char *from_id, const char *to_id,
                                    rf_act_step_t *out_steps, int max_steps,
                                    uint32_t *out_total_ms) {
    int count = 0;
    uint32_t total = 0;

    if (from_id && from_id[0] && (!to_id || strcmp(from_id, to_id) != 0)) {
        int found_stop = 0;
        for (int i = 0; i < g_rf_step_count && count < max_steps; i++) {
            if (strcmp(g_rf_steps[i].act_id, from_id) == 0 && (g_rf_steps[i].step_idx & 0x80)) {
                out_steps[count] = g_rf_steps[i];
                if (strncmp(out_steps[count].name, "Power Off O", 11) == 0 ||
                    strcmp(out_steps[count].name, "Power Off Old") == 0) {
                    strncpy(out_steps[count].name, "Stop old act", sizeof(out_steps[count].name) - 1);
                } else if (strcasecmp(out_steps[count].name, "Switched") == 0) {
                    strncpy(out_steps[count].name, "Activity switched", sizeof(out_steps[count].name) - 1);
                }
                total += out_steps[count].delay_ms;
                count++;
                found_stop = 1;
            }
        }
        if (!found_stop && count < max_steps && strcmp(from_id, "-1") != 0) {
            strncpy(out_steps[count].act_id, from_id, sizeof(out_steps[count].act_id) - 1);
            out_steps[count].step_idx = 0x80;
            out_steps[count].delay_ms = 1500;
            strncpy(out_steps[count].name, "Stop old act", sizeof(out_steps[count].name) - 1);
            total += 1500;
            count++;
        }
    }

    if (to_id && to_id[0]) {
        for (int i = 0; i < g_rf_step_count && count < max_steps; i++) {
            if (strcmp(g_rf_steps[i].act_id, to_id) == 0 && !(g_rf_steps[i].step_idx & 0x80)) {
                out_steps[count] = g_rf_steps[i];
                if (strcasecmp(out_steps[count].name, "Switched") == 0) {
                    strncpy(out_steps[count].name, "Activity switched", sizeof(out_steps[count].name) - 1);
                }
                total += out_steps[count].delay_ms;
                count++;
            }
        }
    }

    if (total < 2000) total = 3500;
    if (out_total_ms) *out_total_ms = total;
    return count;
}

void send_battery_status_rf(void) {
    if (g_rf_fd < 0) return;
    read_elite_battery();
    rf_frame_t f;
    uint8_t payload[25] = {0};
    payload[0] = g_battery_info.percent;
    payload[1] = g_battery_info.charging;
    payload[2] = (uint8_t)(g_battery_info.voltage_mv & 0xFF);
    payload[3] = (uint8_t)((g_battery_info.voltage_mv >> 8) & 0xFF);
    rf_build_frame(&f, g_remote_slot, RF_MSG_BATTERY_STATUS, ++g_tx_seq, 0, payload, 4);
    rf_send_frame(&f);
    printf("[*] Sent Battery Status over RF: %d%%, chg=%d, %umV\n",
           g_battery_info.percent, g_battery_info.charging, g_battery_info.voltage_mv);
}

void send_remote_config_backup_rf(void) {
    if (g_rf_fd < 0) return;
    rf_frame_t f;
    uint8_t payload[25] = {0};
    payload[0] = (uint8_t)g_brightness_val;
    payload[1] = (uint8_t)(g_timeout_seconds & 0xFF);
    payload[2] = (uint8_t)(g_touch_enabled ? 1 : 0);
    payload[3] = (uint8_t)(g_nav_toggle_key & 0xFF);
    payload[4] = (uint8_t)((g_nav_toggle_key >> 8) & 0xFF);
    payload[5] = (uint8_t)(g_nav_toggle_mode & 0xFF);
    payload[6] = (uint8_t)(g_usbeth_enabled ? 1 : 0);
    payload[7] = (uint8_t)(g_confirm_activity ? 1 : 0);
    payload[8] = (uint8_t)g_config_version;
    payload[9] = (uint8_t)g_touch_sensitivity;
    payload[10] = (uint8_t)g_remote_mode;
    payload[11] = (uint8_t)(g_gyro_enabled ? 1 : 0);
    payload[12] = (uint8_t)g_gyro_rate_hz;
    payload[13] = (uint8_t)g_btn_backlight_mode;
    payload[14] = (uint8_t)g_night_start_hour;
    payload[15] = (uint8_t)g_night_end_hour;
    payload[16] = (uint8_t)((g_timeout_seconds >> 8) & 0xFF);
    payload[17] = (uint8_t)(g_cap_buttons_enabled ? 1 : 0);
    payload[18] = 0;
    rf_build_frame(&f, g_remote_slot, RF_MSG_REMOTE_CONFIG_BACKUP, ++g_tx_seq, 0, payload, 19);
    rf_send_frame(&f);
    printf("[*] Sent remote config backup over RF to Hub (slot %d): bright=%d touch=%d cap=%d gyro=%d rate=%d timeout=%d\n",
           g_remote_slot, g_brightness_val, g_touch_enabled, g_cap_buttons_enabled, g_gyro_enabled, g_gyro_rate_hz, g_timeout_seconds);
}

void send_version_report_rf(void) {
    if (g_rf_fd < 0) return;
    rf_frame_t f;
    uint8_t payload[25] = {0};
    unsigned int maj = 1, min = 3, pat = 0;
    sscanf(HARMONY_VERSION, "%u.%u.%u", &maj, &min, &pat);
    payload[0] = (uint8_t)maj;
    payload[1] = (uint8_t)min;
    payload[2] = (uint8_t)pat;
    payload[3] = (uint8_t)g_config_version;
    rf_build_frame(&f, g_remote_slot, RF_MSG_VERSION_REPORT, ++g_tx_seq, 0, payload, 4);
    rf_send_frame(&f);
    printf("[*] Sent 24h version report over RF: v%u.%u.%u (cfg=%u)\n", maj, min, pat, g_config_version);
}
