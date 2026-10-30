#include "elite_rf_sync.h"
#include "elite_state.h"
#include "elite_fb.h"
#include "elite_power.h"
#include "elite_config.h"
#include "elite_rf.h"
#include "elite_rf_ota.h"
#include "elite_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>

static inline void sync_touch_chunk(void) {
    pthread_mutex_lock(&g_sync_lock);
    if (!g_is_syncing_config) {
        g_is_syncing_config = true;
        g_sync_start_ms = get_monotonic_ms();
        g_sync_chunk_count = 0;
        g_sync_total = 0;
    }
    g_sync_last_packet_ms = get_monotonic_ms();
    g_sync_chunk_count++;
    pthread_mutex_unlock(&g_sync_lock);
}

static void sanitize_ascii(char *str, size_t max_len) {
    if (!str) return;
    for (size_t i = 0; i < max_len && str[i]; i++) {
        unsigned char c = (unsigned char)str[i];
        if (c < 0x20 || c > 0x7E) str[i] = ' ';
    }
    str[max_len - 1] = '\0';
}

void handle_hub_frame_remote(const rf_frame_t *f) {
    if (f->dev_slot & 0x0F) g_remote_slot = f->dev_slot & 0x0F;

    /* Handle OTA and custom Icon streaming frames via modular handler */
    if (handle_ota_frame(f)) return;
    if (handle_icon_frame(f)) return;

    switch (f->msg_type) {
        case RF_MSG_ACTIVITY_SYNC: {
            char act_id[13] = {0};
            char act_name[24] = {0};
            if (f->payload[1] == '\0' || f->payload[2] == '\0' || f->payload[0] == '-') {
                /* Short ID format (1-2 byte ID, up to 22 byte name) */
                strncpy(act_id, (const char *)&f->payload[0], sizeof(act_id) - 1);
                strncpy(act_name, (const char *)&f->payload[3], sizeof(act_name) - 1);
            } else if (f->payload[10] >= 0x20) {
                memcpy(act_id, f->payload, 10);
                memcpy(act_name, &f->payload[10], 15);
            } else {
                memcpy(act_id, f->payload, 12);
                memcpy(act_name, &f->payload[12], 14);
            }

            uint64_t now_ms = get_monotonic_ms();

            /* While config sync or OTA update is running, do not clobber sync screen with activity view */
            if (g_is_syncing_config || g_is_ota_updating) {
                strncpy(g_active_activity_id, act_id, sizeof(g_active_activity_id) - 1);
                strncpy(g_active_activity_name, act_name, sizeof(g_active_activity_name) - 1);
                break;
            }

            /* If remote is currently displaying the switching progress bar, and Hub confirms switch */
            if (g_is_switching_activity && strcmp(act_id, g_switching_target_id) == 0) {
                strncpy(g_active_activity_id, act_id, sizeof(g_active_activity_id) - 1);
                strncpy(g_active_activity_name, act_name, sizeof(g_active_activity_name) - 1);
                printf("[*] Hub confirmed switch complete to '%s' (%s), unlocking screen\n", act_name, act_id);
                g_is_switching_activity = false;
                g_last_switch_completed_ms = now_ms;
                strncpy(g_last_switch_target_id, g_switching_target_id, sizeof(g_last_switch_target_id) - 1);
                if (strcmp(g_switching_target_id, "-1") != 0) {
                    g_current_view = VIEW_DEVICES;
                } else {
                    g_current_view = VIEW_ACTIVITIES;
                }
                g_selected_device_cmd = 0;
                if (g_screen_on) reset_idle_timer();
                g_screen_dirty = true;
                break;
            }

            if (g_initial_sync_received && strcmp(g_active_activity_id, act_id) == 0) {
                break;
            }

            uint8_t is_query_reply = f->payload[24];
            uint64_t time_since_wake = (s_last_wake_ms > 0) ? (now_ms - s_last_wake_ms) : 999999;
            uint64_t time_since_reconnect = (s_last_reconnect_ms > 0) ? (now_ms - s_last_reconnect_ms) : 999999;
            bool is_wake_or_reconnect = (time_since_wake < 6000 || time_since_reconnect < 6000);
            bool is_state_catchup = (!g_initial_sync_received || is_query_reply == 0x01 || is_wake_or_reconnect);

            if (is_state_catchup) {
                g_initial_sync_received = true;
                printf("[*] Hub Activity Sync (catch-up: wake=%llums rec=%llums qreply=%d): id=%s name=%s (already running, no switch overlay)\n",
                       (unsigned long long)time_since_wake, (unsigned long long)time_since_reconnect, is_query_reply, act_id, act_name);
                strncpy(g_active_activity_id, act_id, sizeof(g_active_activity_id) - 1);
                strncpy(g_active_activity_name, act_name, sizeof(g_active_activity_name) - 1);
                strncpy(g_last_switch_target_id, act_id, sizeof(g_last_switch_target_id) - 1);
                g_last_switch_completed_ms = now_ms;
                if (strcmp(act_id, "-1") != 0) {
                    g_current_view = VIEW_DEVICES;
                } else {
                    g_current_view = VIEW_ACTIVITIES;
                }
                g_selected_device_cmd = 0;
                if (g_screen_on) reset_idle_timer();
                g_screen_dirty = true;
                break;
            }

            printf("[*] Hub Activity Sync (live change): id=%s name=%s\n", act_id, act_name);
            bool is_recent_same_switch = (strcmp(g_last_switch_target_id, act_id) == 0 && (now_ms - g_last_switch_completed_ms < 20000));
            if (!g_is_switching_activity && !is_recent_same_switch) {
                trigger_activity_switch_external(act_id, act_name);
            }
            strncpy(g_active_activity_id, act_id, sizeof(g_active_activity_id) - 1);
            strncpy(g_active_activity_name, act_name, sizeof(g_active_activity_name) - 1);
            g_selected_device_cmd = 0;
            if (g_screen_on) reset_idle_timer();
            g_screen_dirty = true;
            break;
        }

        case RF_MSG_CONFIG_END: {
            uint8_t phase = f->payload[0];
            uint8_t version = f->payload[1];
            printf("[*] CONFIG_END: phase=%s version=%d\n", phase == 0 ? "clear" : "complete", version);
            if (phase == 0x00) {
                pthread_mutex_lock(&g_sync_lock);
                g_is_syncing_config = true;
                g_sync_start_ms = get_monotonic_ms();
                g_sync_last_packet_ms = g_sync_start_ms;
                g_sync_chunk_count = 0;
                g_sync_total = f->payload[2] | (f->payload[3] << 8); /* 0 from older Hubs */
                g_rf_activity_count = 0;
                g_rf_device_count = 0;
                g_rf_dev_cmd_count = 0;
                g_rf_button_count = 0;
                g_rf_step_count = 0;
                init_act_triggers();
                pthread_mutex_unlock(&g_sync_lock);
                g_screen_dirty = true;
            } else if (!g_is_syncing_config) {
                /* A resend after our ACK was lost is fine to ACK again. Any other "complete"
                   arrived after this remote gave up on the sync; don't stamp its version on
                   the restored config, and leave it unACKed so the Hub reports the failure. */
                if (version != g_config_version) {
                    printf("[-] Ignoring CONFIG_END complete v%d outside a sync\n", version);
                    break;
                }
            } else {
                pthread_mutex_lock(&g_sync_lock);
                g_is_syncing_config = false;
                g_config_version = version;
                g_has_hub_config = true;
                g_update_config_available = false;
                save_rf_config();
                g_activity_count = 0;
                for (int i = 0; i < g_rf_activity_count && g_activity_count < MAX_ACTIVITIES; i++) {
                    strncpy(g_activities[g_activity_count].id, g_rf_activities[i].id, sizeof(g_activities[0].id) - 1);
                    strncpy(g_activities[g_activity_count].name, g_rf_activities[i].name, sizeof(g_activities[0].name) - 1);
                    resolve_activity_icon(g_rf_activities[i].icon_id,
                                          g_rf_activities[i].icon,
                                          g_rf_activities[i].name,
                                          g_activities[g_activity_count].icon_path,
                                          sizeof(g_activities[0].icon_path));
                    g_activity_count++;
                }
                if (g_activity_count < MAX_ACTIVITIES) {
                    strncpy(g_activities[g_activity_count].id, "-1", sizeof(g_activities[0].id) - 1);
                    strncpy(g_activities[g_activity_count].name, "Power Off", sizeof(g_activities[0].name) - 1);
                    strncpy(g_activities[g_activity_count].icon_path, "/opt/luadraws/appscommon/asset/commands/116.png", sizeof(g_activities[0].icon_path) - 1);
                    g_activity_count++;
                }
                if (!has_configured_activities() && g_current_view == VIEW_ACTIVITIES) {
                    open_device_browser();
                }
                pthread_mutex_unlock(&g_sync_lock);
                g_screen_dirty = true;
            }
            rf_frame_t ack;
            rf_build_ack(&ack, g_remote_slot, f->seq, 0x00);
            rf_send_frame(&ack);
            break;
        }

        case RF_MSG_ACTIVITY_LIST_CHUNK: {
            sync_touch_chunk();
            uint8_t idx = f->payload[0];
            if (idx < MAX_RF_ACTIVITIES) {
                uint8_t itype = 0;
                uint16_t icon_id = (uint16_t)f->payload[23] | ((uint16_t)f->payload[24] << 8);
                if (f->payload[2] < 0x20) {
                    itype = f->payload[2];
                    if (f->payload[4] == '\0' || f->payload[5] == '\0' || f->payload[3] == '-') {
                        /* Short ID format (1-2 byte ID + null terminator): up to 17 bytes for activity name */
                        strncpy(g_rf_activities[idx].id, (const char *)&f->payload[3], sizeof(g_rf_activities[0].id) - 1);
                        g_rf_activities[idx].id[sizeof(g_rf_activities[0].id) - 1] = '\0';
                        char tmp_name[18] = {0};
                        memcpy(tmp_name, &f->payload[6], 17);
                        strncpy(g_rf_activities[idx].name, tmp_name, sizeof(g_rf_activities[0].name) - 1);
                        g_rf_activities[idx].name[sizeof(g_rf_activities[0].name) - 1] = '\0';
                    } else if (f->payload[11] >= 0x20) {
                        memcpy(g_rf_activities[idx].id, &f->payload[3], 8);
                        g_rf_activities[idx].id[8] = '\0';
                        char tmp_name[13] = {0};
                        memcpy(tmp_name, &f->payload[11], 12);
                        strncpy(g_rf_activities[idx].name, tmp_name, sizeof(g_rf_activities[0].name) - 1);
                        g_rf_activities[idx].name[sizeof(g_rf_activities[0].name) - 1] = '\0';
                    } else {
                        memcpy(g_rf_activities[idx].id, &f->payload[3], 10);
                        g_rf_activities[idx].id[10] = '\0';
                        char tmp_name[11] = {0};
                        memcpy(tmp_name, &f->payload[13], 10);
                        strncpy(g_rf_activities[idx].name, tmp_name, sizeof(g_rf_activities[0].name) - 1);
                        g_rf_activities[idx].name[sizeof(g_rf_activities[0].name) - 1] = '\0';
                    }
                } else {
                    memcpy(g_rf_activities[idx].id, &f->payload[2], 10);
                    g_rf_activities[idx].id[10] = '\0';
                    char tmp_name[12] = {0};
                    memcpy(tmp_name, &f->payload[12], 11);
                    strncpy(g_rf_activities[idx].name, tmp_name, sizeof(g_rf_activities[0].name) - 1);
                    g_rf_activities[idx].name[sizeof(g_rf_activities[0].name) - 1] = '\0';
                }
                sanitize_ascii(g_rf_activities[idx].id, sizeof(g_rf_activities[idx].id));
                sanitize_ascii(g_rf_activities[idx].name, sizeof(g_rf_activities[idx].name));
                g_rf_activities[idx].icon_id = icon_id;
                map_icon_type_to_str(itype, g_rf_activities[idx].name, g_rf_activities[idx].icon, sizeof(g_rf_activities[idx].icon));
                if (idx >= g_rf_activity_count) g_rf_activity_count = idx + 1;
                printf("[*] Activity chunk %d: %s (%s, icon=%s, icon_id=0x%04X)\n", idx, g_rf_activities[idx].name, g_rf_activities[idx].id, g_rf_activities[idx].icon, icon_id);
            }
            rf_frame_t ack;
            rf_build_ack(&ack, g_remote_slot, f->seq, 0x00);
            rf_send_frame(&ack);
            g_screen_dirty = true;
            break;
        }

        case RF_MSG_DEVICE_LIST_CHUNK: {
            sync_touch_chunk();
            uint8_t idx = f->payload[0];
            if (idx < MAX_RF_DEVICES) {
                uint8_t itype = 0;
                uint16_t icon_id = (uint16_t)f->payload[23] | ((uint16_t)f->payload[24] << 8);
                if (f->payload[2] < 0x20) {
                    itype = f->payload[2];
                    memcpy(g_rf_devices[idx].id, &f->payload[3], 4);
                    g_rf_devices[idx].id[4] = '\0';
                    char tmp_name[17] = {0};
                    memcpy(tmp_name, &f->payload[7], 16);
                    strncpy(g_rf_devices[idx].name, tmp_name, sizeof(g_rf_devices[0].name) - 1);
                    g_rf_devices[idx].name[sizeof(g_rf_devices[0].name) - 1] = '\0';
                } else {
                    memcpy(g_rf_devices[idx].id, &f->payload[2], 4);
                    g_rf_devices[idx].id[4] = '\0';
                    char tmp_name[18] = {0};
                    memcpy(tmp_name, &f->payload[6], 17);
                    strncpy(g_rf_devices[idx].name, tmp_name, sizeof(g_rf_devices[0].name) - 1);
                    g_rf_devices[idx].name[sizeof(g_rf_devices[0].name) - 1] = '\0';
                }
                sanitize_ascii(g_rf_devices[idx].id, sizeof(g_rf_devices[idx].id));
                sanitize_ascii(g_rf_devices[idx].name, sizeof(g_rf_devices[idx].name));
                g_rf_devices[idx].icon_id = icon_id;
                map_icon_type_to_str(itype, g_rf_devices[idx].name, g_rf_devices[idx].icon, sizeof(g_rf_devices[idx].icon));
                if (idx >= g_rf_device_count) g_rf_device_count = idx + 1;
                printf("[*] Device chunk %d: %s (%s, icon=%s, icon_id=0x%04X)\n", idx, g_rf_devices[idx].name, g_rf_devices[idx].id, g_rf_devices[idx].icon, icon_id);
            }
            rf_frame_t ack;
            rf_build_ack(&ack, g_remote_slot, f->seq, 0x00);
            rf_send_frame(&ack);
            g_screen_dirty = true;
            break;
        }

        case RF_MSG_DEVICE_CMD_CHUNK: {
            /* [0] device index, [1] command index, [2] command total, [3..24] name */
            sync_touch_chunk();
            char cname[23] = {0};
            memcpy(cname, &f->payload[3], 22);
            if (f->payload[0] < MAX_RF_DEVICES && cname[0]) {
                sanitize_ascii(cname, sizeof(cname));
                store_rf_dev_cmd(f->payload[0], f->payload[1], cname);
            }
            rf_frame_t ack;
            rf_build_ack(&ack, g_remote_slot, f->seq, 0x00);
            rf_send_frame(&ack);
            break;
        }

        case RF_MSG_BUTTON_LIST_CHUNK: {
            sync_touch_chunk();
            uint8_t idx = f->payload[0];
            if (idx < MAX_RF_BUTTONS) {
                g_rf_buttons[idx].context_type = f->payload[2];
                memcpy(g_rf_buttons[idx].context_id, &f->payload[3], 9);
                g_rf_buttons[idx].context_id[9] = '\0';
                memcpy(g_rf_buttons[idx].label, &f->payload[12], 12);
                g_rf_buttons[idx].label[12] = '\0';
                sanitize_ascii(g_rf_buttons[idx].context_id, sizeof(g_rf_buttons[idx].context_id));
                sanitize_ascii(g_rf_buttons[idx].label, sizeof(g_rf_buttons[idx].label));
                g_rf_buttons[idx].btn_index = f->payload[1];
                g_rf_buttons[idx].action_type = f->payload[24];
                if (idx >= g_rf_button_count) g_rf_button_count = idx + 1;

                /* Check if this button is mapped to an activity start or stop */
                if (f->payload[2] == 0x00 && (f->payload[24] == 0x02 || f->payload[24] == 0x03)) {
                    if (g_act_trigger_count < MAX_ACT_TRIGGERS) {
                        strncpy(g_act_triggers[g_act_trigger_count].button_name, g_rf_buttons[idx].label, sizeof(g_act_triggers[0].button_name) - 1);
                        g_act_triggers[g_act_trigger_count].atype = f->payload[24];
                        strncpy(g_act_triggers[g_act_trigger_count].target_act_id, g_rf_buttons[idx].context_id, sizeof(g_act_triggers[0].target_act_id) - 1);
                        g_act_triggers[g_act_trigger_count].rf_key_code = 0;
                        if (strcasecmp(g_act_triggers[g_act_trigger_count].button_name, "PwrOff") == 0 ||
                            strcasecmp(g_act_triggers[g_act_trigger_count].button_name, "PowerOff") == 0) {
                            g_act_triggers[g_act_trigger_count].rf_key_code = 0x01EC;
                        }
                        g_act_trigger_count++;
                    }
                }
            }
            rf_frame_t ack;
            rf_build_ack(&ack, g_remote_slot, f->seq, 0x00);
            rf_send_frame(&ack);
            g_screen_dirty = true;
            break;
        }

        case RF_MSG_STEP_LIST_CHUNK: {
            sync_touch_chunk();
            uint8_t chunk_idx = f->payload[0];
            if (chunk_idx == 0) {
                g_rf_step_count = 0;
            }
            if (g_rf_step_count < MAX_RF_STEPS) {
                g_rf_steps[g_rf_step_count].step_idx = f->payload[1];
                g_rf_steps[g_rf_step_count].total_steps = f->payload[2];
                strncpy(g_rf_steps[g_rf_step_count].act_id, (char *)&f->payload[3], 9);
                g_rf_steps[g_rf_step_count].act_id[9] = '\0';
                g_rf_steps[g_rf_step_count].delay_ms = (uint16_t)f->payload[12] | ((uint16_t)f->payload[13] << 8);
                strncpy(g_rf_steps[g_rf_step_count].name, (char *)&f->payload[14], 11);
                g_rf_steps[g_rf_step_count].name[11] = '\0';
                g_rf_step_count++;
                printf("[*] Step chunk %d: act=%s step=%d/%d del=%d name='%s'\n",
                       chunk_idx, g_rf_steps[g_rf_step_count - 1].act_id,
                       g_rf_steps[g_rf_step_count - 1].step_idx,
                       g_rf_steps[g_rf_step_count - 1].total_steps,
                       g_rf_steps[g_rf_step_count - 1].delay_ms,
                       g_rf_steps[g_rf_step_count - 1].name);
            }
            rf_frame_t ack;
            rf_build_ack(&ack, g_remote_slot, f->seq, 0x00);
            rf_send_frame(&ack);
            g_screen_dirty = true;
            break;
        }

        case RF_MSG_SETTINGS_PUSH: {
            g_brightness_val = f->payload[0];
            if (g_brightness_val < 1) g_brightness_val = 1;
            if (g_brightness_val > 5) g_brightness_val = 5;
            g_touch_enabled = (f->payload[1] != 0);
            g_gyro_enabled = (f->payload[2] != 0);
            g_timeout_seconds = ((int)f->payload[3] << 8) | f->payload[4];
            int nkey = (int)f->payload[5] | ((int)f->payload[6] << 8);
            if (nkey > 0) g_nav_toggle_key = nkey;
            g_nav_toggle_mode = f->payload[7];
            g_confirm_activity = (f->payload[8] != 0);
            if (f->payload[9] >= 1 && f->payload[9] <= 3) g_touch_sensitivity = f->payload[9];

            /* Extended settings fields */
            if (f->payload[11] > 0) g_gyro_rate_hz = f->payload[11];
            if (f->payload[12] <= 2) g_btn_backlight_mode = f->payload[12];
            if (f->payload[13] <= 23) g_night_start_hour = f->payload[13];
            if (f->payload[14] <= 23) g_night_end_hour = f->payload[14];
            g_usbeth_enabled = (f->payload[15] != 0);
            bool prev_no_screen = g_no_screen_mode;
            g_no_screen_mode = (f->payload[17] != 0);
            if (f->payload[18] == 1) g_cap_buttons_enabled = true;
            else if (f->payload[18] == 2) g_cap_buttons_enabled = false;
            if (f->payload[19] == 1) g_power_debug = true;
            else if (f->payload[19] == 2) g_power_debug = false;
            if (f->payload[20] < ELITE_LANG_COUNT) {
                g_language = (elite_lang_t)f->payload[20];
            }

            if (g_no_screen_mode) {
                set_backlight_level(0);
                g_screen_on = false;
            } else {
                g_screen_on = true;
                set_backlight_level(g_brightness_val);
            }
            save_settings_conf();
            printf("[*] Settings push: bright=%d touch=%d cap=%d gyro=%d rate=%d timeout=%d nav_key=%d nav_mode=%d confirm=%d sens=%d bl=%d night=%02d-%02d usbeth=%d no_screen=%d pwrdbg=%d lang=%d\n",
                   g_brightness_val, g_touch_enabled, g_cap_buttons_enabled, g_gyro_enabled, g_gyro_rate_hz, g_timeout_seconds,
                   g_nav_toggle_key, g_nav_toggle_mode, g_confirm_activity, g_touch_sensitivity,
                   g_btn_backlight_mode, g_night_start_hour, g_night_end_hour, g_usbeth_enabled, g_no_screen_mode, g_power_debug, (int)g_language);
            rf_frame_t ack;
            rf_build_ack(&ack, g_remote_slot, f->seq, 0x00);
            rf_send_frame(&ack);

            if (prev_no_screen && !g_no_screen_mode && !g_fb_mem) {
                /* Remote booted in headless mode; restart via watchdog to cleanly init framebuffer and touch */
                printf("[*] Screen re-enabled from WebUI. Restarting elite_ui to initialize display...\n");
                usleep(150000);
                _exit(0);
            }

            if (!g_no_screen_mode) {
                render_screen();
            }
            break;
        }

        case RF_MSG_TIME_SYNC: {
            uint32_t epoch = (uint32_t)f->payload[0] |
                            ((uint32_t)f->payload[1] << 8) |
                            ((uint32_t)f->payload[2] << 16) |
                            ((uint32_t)f->payload[3] << 24);
            uint8_t tz_len = f->payload[4];
            char tz_str[32] = {0};
            if (tz_len > 0 && tz_len < sizeof(tz_str)) {
                memcpy(tz_str, &f->payload[5], tz_len);
                tz_str[tz_len] = '\0';
            }

            printf("[*] Remote RF TIME_SYNC received: epoch=%u tz=%s\n", epoch, tz_str);
            if (epoch > 1500000000U) {
                struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
                settimeofday(&tv, NULL);
                set_msp430_rtc(epoch);
            }
            if (tz_str[0]) {
                char full_tz[48] = {0};
                expand_timezone(tz_str, full_tz, sizeof(full_tz));
                if (full_tz[0]) {
                    bool tz_changed = (strcmp(g_timezone_str, full_tz) != 0);
                    strncpy(g_timezone_str, full_tz, sizeof(g_timezone_str) - 1);
                    g_timezone_str[sizeof(g_timezone_str) - 1] = '\0';
                    setenv("TZ", g_timezone_str, 1);
                    tzset();
                    FILE *ftz = fopen("/etc/TZ", "w");
                    if (ftz) {
                        fprintf(ftz, "%s\n", full_tz);
                        fclose(ftz);
                    }
                    if (tz_changed) {
                        save_settings_conf();
                        printf("[+] Timezone updated to: %s\n", g_timezone_str);
                    }
                }
            }

            /* Hub config version & update flags embedded at bytes 19..20 */
            uint8_t uflags = f->payload[19];
            uint8_t hub_ver = f->payload[20];
            g_hub_config_version = hub_ver;
            if (hub_ver > g_config_version) {
                g_update_config_available = true;
            } else {
                g_update_config_available = false;
            }
            if ((uflags & 0x02) && g_hub_ota_version[0]) {
                g_update_ota_available = (harmony_version_compare(g_hub_ota_version, HARMONY_ELITE_VERSION) > 0);
            } else if (!(uflags & 0x02)) {
                g_update_ota_available = false;
            }
            if (hub_ver > g_config_version && !g_is_syncing_config && !g_is_ota_updating) {
                static uint64_t last_ver_check_ms = 0;
                uint64_t now_ms = get_monotonic_ms();
                if (!sync_backoff_active(now_ms) && now_ms - last_ver_check_ms > 8000) {
                    last_ver_check_ms = now_ms;
                    printf("[*] Remote detected Hub config version mismatch (hub=%d, local=%d). Requesting full sync...\n",
                           hub_ver, g_config_version);
                    rf_send_full_sync_request();
                }
            }

            /* Hub RF Address embedded in payload bytes 21..24 */
            if (tz_len <= 16 && (f->payload[21] || f->payload[22] || f->payload[23] || f->payload[24])) {
                memcpy(g_paired_hub_addr, &f->payload[21], 4);
                printf("[+] Remote learned Hub RF Address from TIME_SYNC: %02X%02X%02X%02X\n",
                       g_paired_hub_addr[0], g_paired_hub_addr[1], g_paired_hub_addr[2], g_paired_hub_addr[3]);
            }
            g_screen_dirty = true;
            break;
        }

        case RF_MSG_UPDATE_NOTIFY: {
            uint8_t uflags = f->payload[0];
            uint8_t hub_ver = f->payload[1];
            g_hub_config_version = hub_ver;
            if (uflags & 0x01) {
                g_update_config_available = (hub_ver > g_config_version);
            } else {
                g_update_config_available = false;
            }
            if (uflags & 0x02) {
                char ota_v[16];
                snprintf(ota_v, sizeof(ota_v), "%d.%d.%d",
                         f->payload[2], f->payload[3], f->payload[4]);
                if (harmony_version_compare(ota_v, HARMONY_ELITE_VERSION) > 0) {
                    g_update_ota_available = true;
                    snprintf(g_hub_ota_version, sizeof(g_hub_ota_version), "v%s", ota_v);
                } else {
                    g_update_ota_available = false;
                    g_hub_ota_version[0] = '\0';
                }
            } else {
                g_update_ota_available = false;
            }
            printf("[*] Remote received UPDATE_NOTIFY: flags=0x%02X, cfg_ver=%d, ota_ver=%s (ota_avail=%d)\n",
                   uflags, hub_ver, g_hub_ota_version, g_update_ota_available);
            rf_frame_t ack;
            rf_build_ack(&ack, g_remote_slot, f->seq, 0x00);
            rf_send_frame(&ack);
            g_screen_dirty = true;
            break;
        }

        case RF_MSG_UNPAIR_NOTIFY: {
            printf("[*] Remote: received UNPAIR_NOTIFY from Hub! Unpairing...\n");
            rf_perform_unpair_cleanup(false);
            show_notice("Hub unpaired this remote", 4000);
            break;
        }

        case RF_MSG_CONFIG_PULL_REQ: {
            printf("[*] Remote: received CONFIG_PULL_REQ from Hub, sending config backup...\n");
            send_remote_config_backup_rf();
            break;
        }

        case RF_MSG_SLEEP_REQ: {
            printf("[*] Remote: received SLEEP_REQ from Hub. Suspending to RAM...\n");
            rf_frame_t ack;
            rf_build_ack(&ack, g_remote_slot, f->seq, 0x00);
            rf_send_frame(&ack);
            usleep(50000); /* allow RF ACK TX to complete before suspend */
            fb_set_blank(true);
            read_elite_battery();
            if (strcmp(g_battery_info.powersource, "battery") == 0) {
                enter_kernel_suspend();
            }
            break;
        }

        case RF_MSG_ACK:
            break;

        case RF_MSG_PING: {
            rf_frame_t ack;
            rf_build_ack(&ack, g_remote_slot, f->seq, 0x00);
            rf_send_frame(&ack);
            break;
        }

        default:
            if (g_log_level >= LOG_LEVEL_DEBUG) {
                printf("[*] Remote: unknown Hub frame type 0x%02X\n", f->msg_type);
            }
            break;
    }
}

void *rf_rx_worker(void *arg) {
    (void)arg;
    uint8_t pkt[64];

    while (g_running) {
        if (g_rf_fd < 0) { usleep(100000); continue; }
        ssize_t n = read(g_rf_fd, pkt, sizeof(pkt));
        if (n <= 0) {
            if (n < 0 && errno != EINTR) usleep(50000);
            continue;
        }

        if (n == 7 && pkt[0] == 0x10 && pkt[2] == 0x41) {
            bool cc2544_lost = (pkt[4] & 0x40) != 0;
            static uint64_t s_last_link_ok_ms = 0;
            if (!cc2544_lost) {
                s_last_link_ok_ms = get_monotonic_ms();
                if (!g_rf_link_connected) {
                    g_rf_link_connected = true;
                    g_screen_dirty = true;
                }
                if (g_pairing_state == 1) {
                    g_pairing_state = 2;
                    if (g_current_view == VIEW_PAIRING) {
                        if (has_configured_activities()) {
                            g_current_view = VIEW_ACTIVITIES;
                        } else {
                            open_device_browser();
                        }
                        render_screen();
                    }
                } else if (g_pairing_state == 0) {
                    g_pairing_state = 2;
                }
                if (g_activity_count <= 1 && !g_has_hub_config && !sync_backoff_active(get_monotonic_ms())) {
                    rf_send_full_sync_request();
                    render_screen();
                }
            } else if (g_screen_on) {
                if (get_monotonic_ms() - s_last_link_ok_ms > 6000) {
                    if (g_rf_link_connected) {
                        g_rf_link_connected = false;
                        g_screen_dirty = true;
                    }
                }
            }
            static bool s_prev_cc2544_lost = true;
            if (cc2544_lost != s_prev_cc2544_lost || g_log_level >= LOG_LEVEL_DEBUG) {
                s_prev_cc2544_lost = cc2544_lost;
                printf("[*] Remote RF CC2544 Link Status: %s (byte4=0x%02X byte5=0x%02X byte6=0x%02X, connected=%d)\n",
                       cc2544_lost ? "LOST" : "ESTABLISHED", pkt[4], pkt[5], pkt[6], g_rf_link_connected);
                fflush(stdout);
            }
        }

        /* Pipe Device Info replies (11 FF 83 B5 <pipe> <data...>) */
        if (n >= 16 && pkt[0] == 0x11 && pkt[2] == 0x83 && pkt[3] == 0xB5) {
            uint8_t pipe = pkt[4];
            if (pipe == 0x03) {
                memcpy(g_remote_own_addr, &pkt[5], 4);
                printf("[+] Remote Own RF Address (pipe 0x03): %02X%02X%02X%02X\n",
                       g_remote_own_addr[0], g_remote_own_addr[1], g_remote_own_addr[2], g_remote_own_addr[3]);
            } else if (pipe == 0x20 || pipe == 0x30) {
                if (pkt[12] != 0 || pkt[13] != 0 || pkt[14] != 0 || pkt[15] != 0) {
                    memcpy(g_paired_hub_addr, &pkt[12], 4);
                    printf("[+] Remote Paired Hub Address (pipe 0x%02X): %02X%02X%02X%02X\n",
                           pipe, g_paired_hub_addr[0], g_paired_hub_addr[1], g_paired_hub_addr[2], g_paired_hub_addr[3]);
                }
            }
            if (g_current_view == VIEW_PAIRING) render_screen();
            continue;
        }

        if (n >= 5 && pkt[0] == 0x10 && pkt[2] == 0x4A) {
            uint8_t pair_status = pkt[4];
            printf("[*] Remote RF CC2544 Pairing Event: status=0x%02X slot=%d\n", pair_status, pkt[1]);
            if (pkt[1] >= 1 && pkt[1] <= 15) {
                g_remote_slot = pkt[1];
            }
            if (pair_status == 0x01 || pair_status == 0x00) {
                printf("[+] Remote RF Pairing SUCCESS! Paired to Hub on Slot %d\n", g_remote_slot);
                g_rf_link_connected = true;
                g_pairing_state = 2;
                if (g_current_view == VIEW_PAIRING || g_current_view == VIEW_NOT_PAIRED) g_current_view = VIEW_ACTIVITIES;
                rf_send_full_sync_request();
            } else {
                printf("[-] Remote RF Pairing failed / closed (status=0x%02X)\n", pair_status);
                g_pairing_state = 3;
            }
            render_screen();
            continue;
        }

        if (rf_is_valid_frame(pkt, (size_t)n)) {
            g_last_hub_frame_ms = get_monotonic_ms();
            if (!g_rf_link_connected) {
                g_rf_link_connected = true;
                g_screen_dirty = true;
            }
            if (g_pairing_state == 1) {
                g_pairing_state = 2;
                if (g_current_view == VIEW_PAIRING || g_current_view == VIEW_NOT_PAIRED) {
                    g_current_view = VIEW_ACTIVITIES;
                    g_screen_dirty = true;
                }
            } else if (g_pairing_state == 0) {
                g_pairing_state = 2;
            }
            rf_frame_t f;
            memset(&f, 0, sizeof(f));
            if (pkt[0] == 0x20 && n >= 32) {
                if ((pkt[1] & 0x0F) >= 1 && (pkt[1] & 0x0F) <= 15) {
                    g_remote_slot = pkt[1] & 0x0F;
                }
                if ((pkt[4] & 0xE0) == 0x40 || (pkt[4] & 0xE0) == 0x60) {
                    f.report_id = 0x20;
                    f.dev_slot = pkt[1] & 0x0F;
                    f.sub_id = 0x12;
                    f.seq = (pkt[4] & 0x0F);
                    f.hdr_flag = pkt[4];
                    f.msg_type = pkt[5];
                    f.flags = pkt[6];
                    memcpy(f.payload, &pkt[7], sizeof(f.payload));
                } else {
                    memcpy(&f, pkt, sizeof(f));
                }
            } else if (pkt[0] == 0x12 && n >= 30) {
                if (pkt[1] >= 1 && pkt[1] <= 15) {
                    g_remote_slot = pkt[1];
                }
                if ((pkt[2] & 0xE0) == 0x40 || (pkt[2] & 0xE0) == 0x60) {
                    f.report_id = 0x20;
                    f.dev_slot = g_remote_slot;
                    f.sub_id = 0x12;
                    f.seq = (pkt[2] & 0x0F);
                    f.hdr_flag = pkt[2];
                    f.msg_type = pkt[3];
                    f.flags = pkt[4];
                    memcpy(f.payload, &pkt[5], sizeof(f.payload));
                } else {
                    f.report_id = 0x20;
                    f.dev_slot = g_remote_slot;
                    f.sub_id = 0x12;
                    f.seq = pkt[1];
                    f.hdr_flag = 0x40;
                    f.msg_type = pkt[2];
                    f.flags = pkt[3];
                    memcpy(f.payload, &pkt[4], sizeof(f.payload));
                }
            }
            printf("[*] RF RX -> Hub frame type=0x%02X seq=%d\n", f.msg_type, f.seq);
            fflush(stdout);
            handle_hub_frame_remote(&f);
        } else {
            /* Not a standard frame — check if it's a 0x42 container with embedded data */
            if (n >= 32 && pkt[0] == 0x20 && pkt[2] == 0x42) {
                printf("[*] RF RX eQuad container (0x42): parsing payload at offset 3\n");
                rf_frame_t f;
                memset(&f, 0, sizeof(f));
                f.report_id = 0x20;
                f.dev_slot = pkt[1];
                f.sub_id = 0x12;
                if ((pkt[4] & 0xE0) == 0x40 || (pkt[4] & 0xE0) == 0x60) {
                    f.seq = pkt[3];
                    f.hdr_flag = pkt[4];
                    f.msg_type = pkt[5];
                    f.flags = pkt[6];
                    memcpy(f.payload, &pkt[7], sizeof(f.payload));
                } else {
                    f.seq = pkt[3];
                    f.msg_type = pkt[4];
                    f.flags = pkt[5];
                    memcpy(f.payload, &pkt[6], sizeof(f.payload));
                }
                if ((f.msg_type >= RF_MSG_ACTIVITY_SYNC && f.msg_type <= RF_MSG_STEP_LIST_CHUNK) ||
                    (f.msg_type >= RF_MSG_BUTTON_PRESS && f.msg_type <= RF_MSG_SLEEP_REQ) ||
                    (f.msg_type >= RF_MSG_OTA_START && f.msg_type <= RF_MSG_ICON_END) ||
                    f.msg_type == RF_MSG_ACK || f.msg_type == RF_MSG_PING) {
                    g_last_hub_frame_ms = get_monotonic_ms();
                    if (!g_rf_link_connected) {
                        g_rf_link_connected = true;
                        g_screen_dirty = true;
                    }
                    printf("[*] RF RX 0x42 -> Hub frame type=0x%02X seq=%d\n", f.msg_type, f.seq);
                    fflush(stdout);
                    handle_hub_frame_remote(&f);
                }
            }
            if (pkt[0] != 0x20 || pkt[2] != 0x42) {
                printf("[*] Remote RF RX legacy (%zd bytes): ", n);
                for (ssize_t i = 0; i < n; i++) printf("%02X ", pkt[i]);
                printf("\n");
                fflush(stdout);
            }
        }
    }
    return NULL;
}
