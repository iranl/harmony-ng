#include "elite_ui.h"
#include "elite_fb.h"
#include "elite_power.h"
#include "elite_config.h"
#include "elite_rf.h"
#include "harmony_version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/reboot.h>

static void set_touch_enabled(bool enable) {
    g_touch_enabled = enable;
    if (!g_touch_enabled) {
        g_screen_nav_mode = true;
        show_notice("Screen nav moved to bottom buttons", 4000);
    }
    save_settings_conf();
}

static void set_gyro_enabled(bool enable) {
    g_gyro_enabled = enable;
}

static void cycle_nav_key(void) {
    if (!g_touch_enabled) {
        show_notice("Screen nav locked ON", 3000);
        return;
    }
    if (g_nav_toggle_key == 139) g_nav_toggle_key = 358;
    else if (g_nav_toggle_key == 358) g_nav_toggle_key = 395;
    else if (g_nav_toggle_key == 395) g_nav_toggle_key = 389;
    else if (g_nav_toggle_key == 389) g_nav_toggle_key = 174;
    else g_nav_toggle_key = 139;
    save_settings_conf();
}

static void cycle_nav_mode(void) {
    if (!g_touch_enabled) {
        show_notice("Screen nav locked ON", 3000);
        return;
    }
    g_nav_toggle_mode = !g_nav_toggle_mode;
    save_settings_conf();
}

static void cycle_touch_sensitivity(void) {
    g_touch_sensitivity++;
    if (g_touch_sensitivity > 3) g_touch_sensitivity = 1;
    save_settings_conf();
}

static void cycle_remote_mode(void) {
    g_remote_mode = !g_remote_mode;
    save_settings_conf();
    rf_send_sync_query();
}

static void execute_remote_reboot(void) {
    show_notice("Rebooting Remote...", 4000);
    render_screen();
    log_power_debug("[*] System reboot initiated by user from Settings menu. Syncing and rebooting (-f)...\n");
    sync();
    system("if [ -x /data/safe_reboot.sh ]; then (/data/safe_reboot.sh >/dev/null 2>&1 &); "
           "else killall -9 elite_watchdog.sh 2>/dev/null || true; "
           "/data/elite_sync 2>/dev/null || /data/sync 2>/dev/null || true; "
           "sleep 1; "
           "mount -o remount,ro /data 2>/dev/null || true; "
           "/data/elite_sync 2>/dev/null || true; "
           "(/sbin/reboot -f 2>/dev/null || reboot -f 2>/dev/null || (sleep 1 && echo b > /proc/sysrq-trigger)) & fi");
    usleep(500000);
    reboot(RB_AUTOBOOT);
    exit(1);
}

static void execute_elite_factory_reset(void) {
    show_notice("Restoring stock & rebooting...", 6000);
    render_screen();
    system("chmod +x /usr/bin/watchdog /usr/bin/usbhid /usr/bin/hal /opt/luaworks/luaworks /opt/luadraws/luadraws 2>/dev/null || true; "
           "rm -f /etc/tdeenable /etc/nowatchdog /data/reboot_counter /tmp/elite_ui.log 2>/dev/null || true; "
           "if [ -f /data/factory_reset.sh ]; then /data/factory_reset.sh --confirm; "
           "elif [ -f /data/stock_backup/rcS.local.orig ]; then cp -f /data/stock_backup/rcS.local.orig /etc/init.d/rcS.local; "
           "chmod 755 /etc/init.d/rcS.local; rm -f /data/elite_ui /data/dropbear*; /data/elite_sync 2>/dev/null || /data/sync 2>/dev/null || true; /sbin/reboot -f 2>/dev/null || reboot -f; "
           "else rm -f /etc/init.d/rcS.local /data/elite_ui /data/dropbear*; /data/elite_sync 2>/dev/null || /data/sync 2>/dev/null || true; /sbin/reboot -f 2>/dev/null || reboot -f; fi &");
    sleep(1);
    exit(1);
}

void execute_settings_action(int idx) {
    if (g_settings_subview == SETTINGS_VIEW_CATEGORIES) {
        if (idx >= 0 && idx < 6) {
            g_selected_category = idx;
            g_settings_subview = (settings_subview_t)(SETTINGS_VIEW_ACTIVITIES + idx);
            g_selected_sub_item = 0;
        }
        return;
    }

    if (g_settings_subview == SETTINGS_VIEW_ACTIVITIES) {
        switch (idx) {
            case 0:
                g_confirm_activity = !g_confirm_activity;
                save_settings_conf();
                break;
            case 1:
                cycle_remote_mode();
                save_settings_conf();
                break;
            case 2:
                if (!g_rf_link_connected) {
                    show_notice("Hub not connected", 2500);
                } else {
                    rf_send_full_sync_request();
                    show_notice("Syncing config from Hub...", 3000);
                }
                break;
            case 3:
                g_settings_subview = SETTINGS_VIEW_CATEGORIES;
                break;
            default: break;
        }
        return;
    }

    if (g_settings_subview == SETTINGS_VIEW_DISPLAY) {
        switch (idx) {
            case 0:
                g_brightness_val = (g_brightness_val % 5) + 1;
                set_backlight_level(g_brightness_val);
                save_settings_conf();
                break;
            case 1:
                g_timeout_seconds = (g_timeout_seconds == 10) ? 15 :
                                    ((g_timeout_seconds == 15) ? 30 :
                                     ((g_timeout_seconds == 30) ? 60 :
                                      ((g_timeout_seconds == 60) ? 120 : 10)));
                save_settings_conf();
                break;
            case 2:
                g_btn_backlight_mode = (g_btn_backlight_mode + 1) % 3;
                fb_set_blank(!g_screen_on);
                save_settings_conf();
                break;
            case 3:
                if (g_night_start_hour == 22) { g_night_start_hour = 23; g_night_end_hour = 6; }
                else if (g_night_start_hour == 23) { g_night_start_hour = 21; g_night_end_hour = 8; }
                else { g_night_start_hour = 22; g_night_end_hour = 7; }
                save_settings_conf();
                break;
            case 4:
                g_settings_subview = SETTINGS_VIEW_CATEGORIES;
                break;
            default: break;
        }
        return;
    }

    if (g_settings_subview == SETTINGS_VIEW_TOUCH) {
        switch (idx) {
            case 0:
                set_touch_enabled(!g_touch_enabled);
                save_settings_conf();
                break;
            case 1:
                g_cap_buttons_enabled = !g_cap_buttons_enabled;
                save_settings_conf();
                break;
            case 2:
                cycle_touch_sensitivity();
                save_settings_conf();
                break;
            case 3:
                cycle_nav_key();
                save_settings_conf();
                break;
            case 4:
                cycle_nav_mode();
                save_settings_conf();
                break;
            case 5:
                g_settings_subview = SETTINGS_VIEW_CATEGORIES;
                break;
            default: break;
        }
        return;
    }

    if (g_settings_subview == SETTINGS_VIEW_POWER) {
        switch (idx) {
            case 0:
                set_gyro_enabled(!g_gyro_enabled);
                save_settings_conf();
                break;
            case 1:
                g_gyro_rate_hz = (g_gyro_rate_hz == 10) ? 25 : ((g_gyro_rate_hz == 25) ? 50 : 10);
                save_settings_conf();
                break;
            case 2: {
                read_elite_battery();
                char bat_msg[64];
                snprintf(bat_msg, sizeof(bat_msg), "Battery: %d%% (%dmV) %s",
                         g_battery_info.percent, g_battery_info.voltage_mv,
                         g_battery_info.charging ? "Chg" : "Dis");
                show_notice(bat_msg, 2000);
                break;
            }
            case 3:
                g_settings_subview = SETTINGS_VIEW_CATEGORIES;
                break;
            default: break;
        }
        return;
    }

    if (g_settings_subview == SETTINGS_VIEW_SYSTEM) {
        switch (idx) {
            case 0: {
                char vmsg[64];
                snprintf(vmsg, sizeof(vmsg), "Elite v%s (cfg=%d)", HARMONY_ELITE_VERSION, g_config_version);
                show_notice(vmsg, 2500);
                break;
            }
            case 1:
                toggle_usbeth();
                save_settings_conf();
                break;
            case 2:
                if (g_power_debug) {
                    log_power_debug("[*] Power debugging disabled from System menu\n");
                    g_power_debug = false;
                    save_settings_conf();
                    show_notice("Power Debug: DISABLED", 2000);
                } else {
                    g_power_debug = true;
                    save_settings_conf();
                    log_power_debug("[*] Power debugging enabled from System menu\n");
                    show_notice("Power Debug: ENABLED", 2000);
                }
                break;
            case 3:
                g_current_view = VIEW_PAIRING;
                g_selected_pair_opt = 0;
                g_unpair_confirm_active = false;
                if (g_rf_fd >= 0) {
                    uint8_t q_own[7] = {0x10, 0xFF, 0x83, 0xB5, 0x03, 0x00, 0x00};
                    if (write(g_rf_fd, q_own, sizeof(q_own)) < 0) {}
                    usleep(10000);
                    uint8_t q_hub[7] = {0x10, 0xFF, 0x83, 0xB5, 0x20, 0x00, 0x00};
                    if (write(g_rf_fd, q_hub, sizeof(q_hub)) < 0) {}
                }
                break;
            case 4:
                g_settings_subview = SETTINGS_VIEW_SUSPEND;
                g_suspend_countdown_start_ms = get_monotonic_ms();
                g_selected_sub_item = 0;
                break;
            case 5:
                g_settings_subview = SETTINGS_VIEW_REBOOT;
                g_selected_sub_item = 0;
                break;
            case 6:
                g_settings_subview = SETTINGS_VIEW_FACTORY_RESET;
                g_factory_reset_stage = 0;
                g_selected_sub_item = 0;
                break;
            case 7:
                g_settings_subview = SETTINGS_VIEW_CATEGORIES;
                break;
            default: break;
        }
        return;
    }

    if (g_settings_subview == SETTINGS_VIEW_UPDATE) {
        switch (idx) {
            case 0:
                if (!g_rf_link_connected) {
                    show_notice("Hub not connected", 2500);
                } else {
                    read_elite_battery();
                    if (g_battery_info.percent < 40 && !g_battery_info.charging) {
                        show_notice("Low battery (<40%) - connect charger", 3500);
                    } else {
                        rf_frame_t f;
                        uint8_t payload[25] = {0};
                        rf_build_frame(&f, g_remote_slot, RF_MSG_OTA_REQUEST, ++g_tx_seq, 0, payload, 4);
                        rf_send_frame(&f);
                        show_notice("Requesting OTA from Hub...", 3500);
                    }
                }
                break;
            case 1:
                if (!g_rf_link_connected) {
                    show_notice("Hub not connected", 2500);
                } else {
                    send_version_report_rf();
                    rf_send_sync_query();
                    show_notice("Checking for updates...", 2000);
                }
                break;
            case 2:
                g_settings_subview = SETTINGS_VIEW_CATEGORIES;
                g_selected_category = 5;
                break;
            default: break;
        }
        return;
    }

    if (g_settings_subview == SETTINGS_VIEW_SUSPEND) {
        if (idx == 0) {
            g_suspend_countdown_start_ms = 0;
            g_settings_subview = SETTINGS_VIEW_SYSTEM;
            g_selected_sub_item = 4;
        } else if (idx == 1) {
            g_suspend_countdown_start_ms = 0;
            trigger_manual_suspend_to_ram();
            g_settings_subview = SETTINGS_VIEW_SYSTEM;
            g_selected_sub_item = 4;
        }
        return;
    }

    if (g_settings_subview == SETTINGS_VIEW_REBOOT) {
        if (idx == 0) {
            g_settings_subview = SETTINGS_VIEW_SYSTEM;
            g_selected_sub_item = 5;
        } else if (idx == 1) {
            execute_remote_reboot();
        }
        return;
    }

    if (g_settings_subview == SETTINGS_VIEW_FACTORY_RESET) {
        if (idx == 0) {
            g_settings_subview = SETTINGS_VIEW_SYSTEM;
            g_factory_reset_stage = 0;
            g_selected_sub_item = 6;
        } else if (idx == 1) {
            if (g_factory_reset_stage == 0) {
                g_factory_reset_stage = 1;
                g_selected_sub_item = 0;
            } else {
                execute_elite_factory_reset();
            }
        }
        return;
    }
}
