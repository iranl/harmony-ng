#include "remote_ui.h"
#include "remote_fb.h"
#include "remote_power.h"
#include "remote_config.h"
#include "remote_rf.h"
#include "harmony_version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/reboot.h>

static void set_touch_enabled(bool enable) {
    g_touch_enabled = enable;
    if (!g_touch_enabled) show_notice(tr(STR_NOTICE_NAV_MOVED), 4000);
    save_settings_conf();
}

static void set_gyro_enabled(bool enable) {
    g_gyro_enabled = enable;
}

static void cycle_touch_sensitivity(void) {
    g_touch_sensitivity++;
    if (g_touch_sensitivity > 3) g_touch_sensitivity = 1;
    save_settings_conf();
}

static void execute_remote_reboot(void) {
    bool docked = is_remote_docked();
    show_notice(tr(docked ? STR_NOTICE_REBOOTING : STR_NOTICE_RESTARTING), 4000);
    render_screen();
    log_power_debug("[*] System reboot initiated from Settings (docked=%d)...\n", docked);
    sync();
    system("/data/remote_sync 2>/dev/null || /data/sync 2>/dev/null || true");

    if (docked) {
        if (g_rf_fd >= 0) {
            close(g_rf_fd);
            g_rf_fd = -1;
        }
        system("touch /tmp/remote_stop 2>/dev/null || true; "
               "rm -f /data/reboot_counter 2>/dev/null || true; "
               "if [ -x /data/safe_reboot.sh ]; then (/data/safe_reboot.sh >/dev/null 2>&1 &); "
               "else killall -9 init.sh remote_watchdog.sh 2>/dev/null || true; "
               "sleep 1; "
               "mount -o remount,ro /cache 2>/dev/null || true; "
               "/data/remote_sync 2>/dev/null || sync 2>/dev/null || true; "
               "sleep 1; "
               "(/sbin/reboot -f 2>/dev/null || reboot -f 2>/dev/null || (sleep 1 && echo b > /proc/sysrq-trigger)) & fi");
        exit(0);
    } else {
        /* On battery, SoC hardware reset causes PMIC power-off. Restart runtime cleanly via execv. */
        if (g_rf_fd >= 0) {
            close(g_rf_fd);
            g_rf_fd = -1;
        }
        usleep(250000);
        char *const args[] = { "/data/remote_ui", NULL };
        execv("/data/remote_ui", args);
        /* Fallback: exit with 143 so watchdog restarts remote_ui */
        exit(143);
    }
}

static void execute_remote_factory_reset(void) {
    show_notice("Restoring stock & rebooting...", 6000);
    render_screen();
    system("if [ -x /data/init.sh ]; then /data/init.sh stop 2>/dev/null || true; fi; "
           "killall -9 init.sh remote_watchdog.sh 2>/dev/null || true; "
           "if [ -f /data/factory_reset.sh ]; then /data/factory_reset.sh --confirm; "
           "else "
           "chmod +x /usr/bin/watchdog /usr/bin/usbhid /usr/bin/hal /opt/luaworks/luaworks /opt/luadraws/luadraws /usr/sbin/bluetoothd /usr/bin/dbus-daemon 2>/dev/null || true; "
           "rm -f /etc/nowatchdog /etc/tdeenable /data/reboot_counter /tmp/remote_ui.log /tmp/remote-init.log /tmp/remote_stop /tmp/remote_supervisors.pid 2>/dev/null || true; "
           "rm -f /usr/bin/sync /usr/sbin/dropbear /usr/sbin/dropbearkey /usr/sbin/dbclient /usr/sbin/scp 2>/dev/null || true; "
           "rm -f /usr/bin/head /usr/bin/tail /usr/bin/which /usr/bin/find /usr/bin/tar /usr/bin/xargs /usr/bin/xz /usr/bin/bzcat /usr/bin/bunzip2 /usr/bin/lzma /usr/bin/unlzma /usr/bin/diff /usr/bin/patch /usr/bin/awk /usr/bin/sed 2>/dev/null || true; "
           "if [ -f /data/stock_backup/rcS.local.orig ] && ! grep -q 'usbeth' /data/stock_backup/rcS.local.orig 2>/dev/null && ! grep -q 'dropbear' /data/stock_backup/rcS.local.orig 2>/dev/null && ! grep -q 'exit 0' /data/stock_backup/rcS.local.orig 2>/dev/null; then "
           "cp -f /data/stock_backup/rcS.local.orig /etc/init.d/rcS.local; chmod 755 /etc/init.d/rcS.local; "
           "else "
           "printf '#!/bin/sh\\n/usr/bin/flashuboot /lib/firmware/u-boot.bin 2>/dev/null || true\\n' > /etc/init.d/rcS.local; chmod 755 /etc/init.d/rcS.local; fi; "
           "rm -f /data/remote_ui /data/remote_ui.new /data/elite_ui /data/elite_ui.new /data/init.sh /data/safe_reboot.sh /data/remote_watchdog.sh /data/elite_watchdog.sh /data/dropbear* /data/mknod /data/scp /data/busybox 2>/dev/null || true; "
           "rm -rf /data/stock_backup /data/assets /data/themes 2>/dev/null || true; "
           "/data/remote_sync 2>/dev/null || /data/sync 2>/dev/null || /bin/busybox sync 2>/dev/null || sync 2>/dev/null || true; "
           "rm -f /data/remote_sync /data/elite_sync /data/sync 2>/dev/null || true; "
           "/bin/busybox sync 2>/dev/null || sync 2>/dev/null || true; sleep 1; "
           "/sbin/reboot -f 2>/dev/null || reboot -f; fi &");
    sleep(1);
    exit(0);
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
                remote_lang_cycle();
                save_settings_conf();
                show_notice(tr(STR_NOTICE_LANG_CHANGED), 2000);
                break;
            case 2:
                if (!g_rf_link_connected) {
                    show_notice(tr(STR_NOTICE_HUB_NOT_CONNECTED), 2500);
                } else {
                    rf_send_full_sync_request();
                    show_notice(tr(STR_NOTICE_SYNCING), 3000);
                }
                break;
            case 3:
                g_activity_order = g_activity_order == ACTIVITY_ORDER_MOST_USED ? ACTIVITY_ORDER_YOURS : ACTIVITY_ORDER_MOST_USED;
                pthread_mutex_lock(&g_render_mutex);
                if (g_has_hub_config) build_activity_list();
                g_selected_activity = 0;
                pthread_mutex_unlock(&g_render_mutex);
                save_settings_conf();
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
            case 3:   /* night start and end: one hour per tap, wrapping at midnight */
                g_night_start_hour = (g_night_start_hour + 1) % 24;
                fb_set_blank(!g_screen_on);   /* the key light follows the new window at once */
                save_settings_conf();
                break;
            case 4:
                g_night_end_hour = (g_night_end_hour + 1) % 24;
                fb_set_blank(!g_screen_on);
                save_settings_conf();
                break;
            case 5: {
                char next[32];
                theme_next_name(next, sizeof(next));
                apply_theme(next);
                save_settings_conf();
                break;
            }
            case 6:
                if (g_physical_nav_enabled) {
                    show_notice("Layout locked to List by Physical Nav", 2500);
                    break;
                }
                g_home_layout = (g_home_layout + 1) % HOME_LAYOUT_COUNT;
                g_selected_activity = 0;
                home_select_running();
                save_settings_conf();
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
                g_physical_nav_enabled = !g_physical_nav_enabled;
                if (g_physical_nav_enabled) {
                    g_home_layout = HOME_LAYOUT_LIST;
                    g_selected_activity = 0;
                    home_select_running();
                }
                save_settings_conf();
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
            case 1: {
                g_gyro_rate_hz = (g_gyro_rate_hz == 10) ? 25 : ((g_gyro_rate_hz == 25) ? 50 : ((g_gyro_rate_hz == 50) ? 100 : 10));
                if (g_gyro_fd >= 0 && g_gyro_enabled) {
                    init_gyro_interrupt();
                }
                save_settings_conf();
                break;
            }
            case 2:
                if (g_deep_standby_min < 0) g_deep_standby_min = 15;
                else if (g_deep_standby_min == 15) g_deep_standby_min = 30;
                else if (g_deep_standby_min == 30) g_deep_standby_min = 60;
                else if (g_deep_standby_min == 60) g_deep_standby_min = 120;
                else if (g_deep_standby_min == 120) g_deep_standby_min = 240;
                else if (g_deep_standby_min == 240) g_deep_standby_min = 0;
                else g_deep_standby_min = -1;
                save_settings_conf();
                break;
            case 3: {
                read_remote_battery();
                char bat_msg[64];
                snprintf(bat_msg, sizeof(bat_msg), "Battery: %d%% (%dmV) %s",
                         g_battery_info.percent, g_battery_info.voltage_mv,
                         g_battery_info.charging ? "Chg" : "Dis");
                show_notice(bat_msg, 2000);
                break;
            }
            case 4:
                if (g_debug_logging) {
                    log_remote_debug("[*] Debug logging disabled from Settings\n");
                    g_debug_logging = false;
                    save_settings_conf();
                    show_notice("Debug Log: DISABLED", 2000);
                } else {
                    g_debug_logging = true;
                    save_settings_conf();
                    log_remote_debug("[*] Debug logging enabled from Settings\n");
                    show_notice("Debug Log: ENABLED", 2000);
                }
                break;
            case 5:
                g_settings_subview = SETTINGS_VIEW_SUSPEND;
                g_suspend_countdown_start_ms = get_monotonic_ms();
                g_selected_sub_item = 0;
                break;
            default: break;
        }
        return;
    }

    if (g_settings_subview == SETTINGS_VIEW_SYSTEM) {
        switch (idx) {
            case 0:
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
            case 1:
                g_settings_subview = SETTINGS_VIEW_REBOOT;
                g_selected_sub_item = 0;
                break;
            case 2:
                g_settings_subview = SETTINGS_VIEW_FACTORY_RESET;
                g_factory_reset_stage = 0;
                g_selected_sub_item = 0;
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
                    read_remote_battery();
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
            default: break;
        }
        return;
    }

    if (g_settings_subview == SETTINGS_VIEW_SUSPEND) {
        if (idx == 0) {
            g_suspend_countdown_start_ms = 0;
            g_settings_subview = SETTINGS_VIEW_POWER;
            g_selected_sub_item = 4;
        } else if (idx == 1) {
            g_suspend_countdown_start_ms = 0;
            trigger_manual_suspend_to_ram();
            g_settings_subview = SETTINGS_VIEW_POWER;
            g_selected_sub_item = 4;
        }
        return;
    }

    if (g_settings_subview == SETTINGS_VIEW_REBOOT) {
        if (idx == 0) {
            g_settings_subview = SETTINGS_VIEW_SYSTEM;
            g_selected_sub_item = 1;
        } else if (idx == 1) {
            execute_remote_reboot();
        }
        return;
    }

    if (g_settings_subview == SETTINGS_VIEW_FACTORY_RESET) {
        if (idx == 0) {
            g_settings_subview = SETTINGS_VIEW_SYSTEM;
            g_factory_reset_stage = 0;
            g_selected_sub_item = 2;
        } else if (idx == 1) {
            if (g_factory_reset_stage == 0) {
                g_factory_reset_stage = 1;
                g_selected_sub_item = 0;
            } else {
                execute_remote_factory_reset();
            }
        }
        return;
    }
}
