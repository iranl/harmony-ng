#include "elite_input.h"
#include "elite_fb.h"
#include "elite_power.h"
#include "elite_config.h"
#include "elite_rf.h"
#include "elite_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <linux/input.h>

uint16_t g_held_rf_code = 0;
pthread_mutex_t g_repeat_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_repeat_cond = PTHREAD_COND_INITIALIZER;

uint16_t linux_to_harmony_rf_key(int code) {
    switch (code) {
        /* Activities & Power */
        case 116: return 0x01EC; /* KEY_POWER -> PowerOffActivity */

        /* Navigation */
        case 103: return 0x0052; /* KEY_UP -> DirectionUp */
        case 108: return 0x0051; /* KEY_DOWN -> DirectionDown */
        case 105: return 0x0050; /* KEY_LEFT -> DirectionLeft */
        case 106: return 0x004F; /* KEY_RIGHT -> DirectionRight */
        case 352:
        case 28:  return 0x0058; /* KEY_OK / ENTER -> Select */
        case 174: return 0x0094; /* KEY_EXIT -> Exit */
        case 158: return 0x0225; /* KEY_BACK -> Back */
        case 139:
        case 127: return 0x0065; /* KEY_MENU / COMPOSE -> Menu */
        case 358: return 0x01FF; /* KEY_INFO -> Info */
        case 395: return 0x008D; /* KEY_LIST -> Guide */
        case 389: return 0x009A; /* KEY_ORIGINAL -> Dvr */

        /* Volume & Channel */
        case 115: return 0x00E9; /* KEY_VOLUMEUP -> VolumeUp */
        case 114: return 0x00EA; /* KEY_VOLUMEDOWN -> VolumeDown */
        case 113: return 0x00E2; /* KEY_MUTE -> VolumeMute */
        case 402: return 0x009C; /* KEY_CHANNELUP -> ChannelUp */
        case 403: return 0x009D; /* KEY_CHANNELDOWN -> ChannelDown */
        case 412: return 0x0224; /* KEY_PREVIOUS -> PrevChannel */

        /* Transport / Playback */
        case 207: return 0x00B0; /* KEY_PLAY -> Play */
        case 119: return 0x00B1; /* KEY_PAUSE -> Pause */
        case 167: return 0x00B2; /* KEY_RECORD -> Record */
        case 159: return 0x00B3; /* KEY_FORWARD -> FastForward */
        case 168: return 0x00B4; /* KEY_REWIND -> Rewind */
        case 128: return 0x00B7; /* KEY_STOP -> Stop */

        /* Color Buttons */
        case 398: return 0x01F7; /* KEY_RED -> Red */
        case 399: return 0x01F6; /* KEY_GREEN -> Green */
        case 400: return 0x01F5; /* KEY_YELLOW -> Yellow */
        case 401: return 0x01F4; /* KEY_BLUE -> Blue */

        /* Home Automation */
        case 148: return 0x0FF2; /* KEY_PROG1 -> Light On */
        case 149: return 0x0FF3; /* KEY_PROG2 -> Light Off */
        case 202: return 0x0FF4; /* KEY_PROG3 -> Plug On */
        case 203: return 0x0FF5; /* KEY_PROG4 -> Plug Off */
        case 78:  return 0x0FF0; /* KEY_KPPLUS -> RockerUp */
        case 74:  return 0x0FF1; /* KEY_KPMINUS -> RockerDown */

        default:  return 0;
    }
}

void *key_repeat_thread(void *arg) {
    (void)arg;
    while (g_running) {
        pthread_mutex_lock(&g_repeat_mutex);
        while (g_held_rf_code == 0 && g_running) {
            pthread_cond_wait(&g_repeat_cond, &g_repeat_mutex);
        }
        if (!g_running) {
            pthread_mutex_unlock(&g_repeat_mutex);
            break;
        }
        uint16_t key = g_held_rf_code;
        pthread_mutex_unlock(&g_repeat_mutex);

        usleep(300000); /* 300ms initial hold delay */

        while (g_running) {
            pthread_mutex_lock(&g_repeat_mutex);
            if (g_held_rf_code != key) {
                pthread_mutex_unlock(&g_repeat_mutex);
                break;
            }
            pthread_mutex_unlock(&g_repeat_mutex);

            rf_send_button_event(key, 1);
            usleep(100000); /* 100ms repeat rate */
        }
    }
    return NULL;
}

int find_touchscreen_fd(void) {
    const char *candidates[] = {
        "/dev/input/event2",
        "/dev/input/event3",
        "/dev/input/touchscreen0",
        "/dev/input/event1",
        "/dev/input/event0"
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        int fd = open(candidates[i], O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            char name[128] = {0};
            if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0) {
                if (strstr(name, "Clearpad") || strstr(name, "clearpad") ||
                    strstr(name, "touch") || strstr(name, "Touch")) {
                    printf("[+] Discovered touchscreen at %s: %s (fd=%d)\n", candidates[i], name, fd);
                    return fd;
                }
            }
            close(fd);
        }
    }
    return open(TOUCH_EVENT_DEV, O_RDONLY | O_NONBLOCK);
}

void handle_screen_touch_tap(int x, int y) {
    reset_idle_timer();
    if (!g_screen_on) {
        fb_set_blank(false);
        render_screen();
        return;
    }
    if (!g_touch_enabled) return;
    if (g_is_switching_activity || g_is_syncing_config || g_is_ota_updating) return;

    printf("[*] Touch tap: screen x=%d, y=%d (down at %d, %d)\n", x, y, g_touch_down_x, g_touch_down_y);

    int h_thresh = (g_touch_sensitivity == 1) ? 48 : ((g_touch_sensitivity == 3) ? 28 : 38);
    int v_thresh = (g_touch_sensitivity == 1) ? 36 : ((g_touch_sensitivity == 3) ? 22 : 28);
    int tap_slop = (g_touch_sensitivity == 1) ? 16 : ((g_touch_sensitivity == 3) ? 25 : 20);

    int dx = x - g_touch_down_x;
    int dy = y - g_touch_down_y;
    bool is_tap = (abs(dx) <= tap_slop && abs(dy) <= tap_slop);

    if (is_tap && y < 38) {
        if (x >= 106 && x <= 138) {
            g_screen_nav_mode = !g_screen_nav_mode;
            printf("[*] Mode toggled by top header icon: %s\n", g_screen_nav_mode ? "NAV" : "DEV");
            show_notice(g_screen_nav_mode ? "Screen Nav: ON" : "Screen Nav: OFF", 1500);
            render_screen();
            return;
        }
        bool ota_avail = g_update_ota_available && harmony_version_compare(g_hub_ota_version, HARMONY_ELITE_VERSION) > 0;
        if ((g_update_config_available || ota_avail) && x >= 70 && x <= 104) {
            printf("[*] Touch tap on update badge -> jump to UPDATE & SYNC\n");
            g_current_view = VIEW_SETTINGS;
            g_settings_subview = SETTINGS_VIEW_UPDATE;
            g_selected_sub_item = 0;
            render_screen();
        } else if (g_current_view == VIEW_DEVICE_BROWSER && x < 90) {
            device_browser_back();
            render_screen();
        }
        return;
    }

    if (!is_tap && abs(dx) >= h_thresh && abs(dx) >= (abs(dy) * 3 / 2) && y < 280) {
        if (!is_remote_paired()) {
            if (g_current_view == VIEW_NOT_PAIRED) {
                g_current_view = VIEW_SETTINGS;
                g_settings_subview = SETTINGS_VIEW_CATEGORIES;
                g_selected_sub_item = 0;
            } else if (g_current_view == VIEW_SETTINGS) {
                g_current_view = VIEW_NOT_PAIRED;
            }
            printf("[*] Swipe (dx=%d) [unpaired lock] -> view %d\n", dx, g_current_view);
            render_screen();
            return;
        }
        if (g_current_view == VIEW_DEVICE_BROWSER) {
            if (dx > 0 && g_browse_dev >= 0) {
                device_browser_back();
                render_screen();
                return;
            }
        }
        if (dx > 0) {
            if (g_current_view == VIEW_ACTIVITIES) g_current_view = VIEW_SETTINGS;
            else if (g_current_view == VIEW_DEVICES) g_current_view = VIEW_ACTIVITIES;
            else if (g_current_view == VIEW_DEVICE_BROWSER) g_current_view = VIEW_DEVICES;
            else if (g_current_view == VIEW_SETTINGS || g_current_view == VIEW_PAIRING) open_device_browser();
            g_settings_subview = SETTINGS_VIEW_CATEGORIES;
            g_selected_sub_item = 0;
            g_factory_reset_stage = 0;
            printf("[*] Swipe right (dx=%d) -> view %d\n", dx, g_current_view);
            render_screen();
            return;
        } else {
            if (g_current_view == VIEW_ACTIVITIES) g_current_view = VIEW_DEVICES;
            else if (g_current_view == VIEW_DEVICES) open_device_browser();
            else if (g_current_view == VIEW_DEVICE_BROWSER) g_current_view = VIEW_SETTINGS;
            else if (g_current_view == VIEW_SETTINGS || g_current_view == VIEW_PAIRING) g_current_view = VIEW_ACTIVITIES;
            g_settings_subview = SETTINGS_VIEW_CATEGORIES;
            g_selected_sub_item = 0;
            g_factory_reset_stage = 0;
            printf("[*] Swipe left (dx=%d) -> view %d\n", dx, g_current_view);
            render_screen();
            return;
        }
    }

    if (!is_tap && abs(dy) >= v_thresh && abs(dy) >= (abs(dx) * 3 / 2)) {
        if (dy < 0) {
            printf("[*] Swipe Up (dy=%d) -> next page\n", dy);
            if (g_current_view == VIEW_ACTIVITIES) {
                int cur_page = g_selected_activity / 4;
                if ((cur_page + 1) * 4 < activity_list_count()) g_selected_activity = (cur_page + 1) * 4;
                else if (activity_list_count() > 0) g_selected_activity = activity_list_count() - 1;
            } else if (g_current_view == VIEW_DEVICE_BROWSER) {
                device_browser_page(1);
            } else if (g_current_view == VIEW_DEVICES) {
                int tot = get_active_dev_cmd_count();
                int cur_page = g_selected_device_cmd / 6;
                if ((cur_page + 1) * 6 < tot) g_selected_device_cmd = (cur_page + 1) * 6;
                else if (tot > 0) g_selected_device_cmd = tot - 1;
            } else if (g_current_view == VIEW_SETTINGS) {
                if (g_settings_subview == SETTINGS_VIEW_CATEGORIES) {
                    g_selected_category = (g_selected_category + 1) % 6;
                } else {
                    int count = get_settings_sub_count();
                    if (count > 0) g_selected_sub_item = (g_selected_sub_item + 1) % count;
                }
            } else if (g_current_view == VIEW_PAIRING) {
                g_selected_pair_opt = (g_selected_pair_opt + 1) % PAIR_OPT_COUNT;
            }
        } else {
            printf("[*] Swipe Down (dy=%d) -> prev page\n", dy);
            if (g_current_view == VIEW_ACTIVITIES) {
                int cur_page = g_selected_activity / 4;
                if (cur_page > 0) g_selected_activity = (cur_page - 1) * 4;
                else g_selected_activity = 0;
            } else if (g_current_view == VIEW_DEVICE_BROWSER) {
                device_browser_page(-1);
            } else if (g_current_view == VIEW_DEVICES) {
                int cur_page = g_selected_device_cmd / 6;
                if (cur_page > 0) g_selected_device_cmd = (cur_page - 1) * 6;
                else g_selected_device_cmd = 0;
            } else if (g_current_view == VIEW_SETTINGS) {
                if (g_settings_subview == SETTINGS_VIEW_CATEGORIES) {
                    g_selected_category = (g_selected_category + 5) % 6;
                } else {
                    int count = get_settings_sub_count();
                    if (count > 0) g_selected_sub_item = (g_selected_sub_item + count - 1) % count;
                }
            } else if (g_current_view == VIEW_PAIRING) {
                g_selected_pair_opt = (g_selected_pair_opt + PAIR_OPT_COUNT - 1) % PAIR_OPT_COUNT;
            }
        }
        render_screen();
        return;
    }

    if (y >= 280 && y < 320) {
        if (g_confirm_pending_idx >= 0) {
            g_confirm_pending_idx = -1;
        }
        g_confirm_pending_rf_code = 0;
        ui_view_t prev_view = g_current_view;
        bool has_acts = has_configured_activities();
        if (has_acts) {
            if (x < 60) {
                if (!is_remote_paired()) {
                    show_notice("Remote not paired - pair first", 2500);
                    return;
                }
                g_current_view = VIEW_ACTIVITIES;
                g_settings_subview = SETTINGS_VIEW_CATEGORIES;
                g_selected_sub_item = 0;
                g_factory_reset_stage = 0;
            } else if (x < 120) {
                if (!is_remote_paired()) {
                    show_notice("Remote not paired - pair first", 2500);
                    return;
                }
                g_current_view = VIEW_DEVICES;
                g_settings_subview = SETTINGS_VIEW_CATEGORIES;
                g_selected_sub_item = 0;
                g_factory_reset_stage = 0;
            } else if (x < 180) {
                if (!is_remote_paired()) {
                    show_notice("Remote not paired - pair first", 2500);
                    return;
                }
                open_device_browser();
                g_settings_subview = SETTINGS_VIEW_CATEGORIES;
                g_selected_sub_item = 0;
                g_factory_reset_stage = 0;
            } else {
                g_current_view = VIEW_SETTINGS;
                g_settings_subview = SETTINGS_VIEW_CATEGORIES;
                g_selected_sub_item = 0;
                g_factory_reset_stage = 0;
            }
        } else {
            /* 2 tabs when no activities: [ DEVICES ] [ SETTINGS ] */
            if (x < 120) {
                if (!is_remote_paired()) {
                    show_notice("Remote not paired - pair first", 2500);
                    return;
                }
                open_device_browser();
                g_settings_subview = SETTINGS_VIEW_CATEGORIES;
                g_selected_sub_item = 0;
                g_factory_reset_stage = 0;
            } else {
                g_current_view = VIEW_SETTINGS;
                g_settings_subview = SETTINGS_VIEW_CATEGORIES;
                g_selected_sub_item = 0;
                g_factory_reset_stage = 0;
            }
        }
        if ((prev_view == VIEW_SETTINGS || prev_view == VIEW_PAIRING) && g_current_view != prev_view && g_settings_dirty) {
            g_settings_dirty = false;
        }
        render_screen();
        return;
    }

    if (y >= 320) {
        g_confirm_pending_rf_code = 0;
        if (!is_remote_paired()) {
            show_notice("Remote not paired - pair first", 2500);
            return;
        }
        bool has_acts = has_configured_activities();
        if (x < 120) {
            printf("[*] Capacitive tap: Activities (x=%d, y=%d)\n", x, y);
            if (has_acts) {
                g_current_view = VIEW_ACTIVITIES;
            } else {
                open_device_browser();
            }
        } else {
            printf("[*] Capacitive tap: Devices (x=%d, y=%d)\n", x, y);
            if (has_acts) {
                g_current_view = VIEW_DEVICES;
            } else {
                open_device_browser();
            }
        }
        render_screen();
        return;
    }

    if (!is_tap) return;

    int start_y = 38;
    if (g_current_view == VIEW_ACTIVITIES) {
        int card_h = 56, spacing = 4, page_size = 4;
        int page_start = (g_selected_activity / page_size) * page_size;
        for (int i = 0; i < page_size && (page_start + i) < activity_list_count(); i++) {
            int cy = start_y + i * (card_h + spacing);
            if (y >= cy && y < cy + card_h + spacing) {
                int idx = page_start + i;
                g_selected_activity = idx;
                printf("[*] Touch selected Activity: %s (id=%s)\n", g_activities[idx].name, g_activities[idx].id);
                request_activity_switch_with_confirm(idx);
                return;
            }
        }
    } else if (g_current_view == VIEW_DEVICE_BROWSER) {
        int page_start = (g_browse_sel / 6) * 6;
        for (int i = 0; i < 6 && (page_start + i) < browser_item_count(); i++) {
            int cy = start_y + i * (34 + 5);
            if (y >= cy && y < cy + 34 + 5) {
                device_browser_activate(page_start + i);
                render_screen();
                return;
            }
        }
    } else if (g_current_view == VIEW_DEVICES) {
        int card_h = 34, spacing = 5, page_size = 6;
        int page_start = (g_selected_device_cmd / page_size) * page_size;
        int total = get_active_dev_cmd_count();
        for (int i = 0; i < page_size && (page_start + i) < total; i++) {
            int cy = start_y + i * (card_h + spacing);
            if (y >= cy && y < cy + card_h + spacing) {
                g_selected_device_cmd = page_start + i;
                execute_active_dev_cmd(g_selected_device_cmd);
                render_screen();
                return;
            }
        }
    } else if (g_current_view == VIEW_SETTINGS) {
        if (g_settings_subview == SETTINGS_VIEW_CATEGORIES) {
            int card_h = 35, spacing = 4;
            for (int i = 0; i < 6; i++) {
                int cy = start_y + i * (card_h + spacing);
                if (y >= cy && y < cy + card_h) {
                    g_selected_category = i;
                    execute_settings_action(i);
                    render_screen();
                    return;
                }
            }
        } else if (g_settings_subview == SETTINGS_VIEW_FACTORY_RESET || g_settings_subview == SETTINGS_VIEW_REBOOT || g_settings_subview == SETTINGS_VIEW_SUSPEND) {
            int card_y = 38, card_h = 130;
            int btn_y = card_y + card_h + 10, btn_h = 38, spacing = 8;
            for (int i = 0; i < 2; i++) {
                int cy = btn_y + i * (btn_h + spacing);
                if (y >= cy && y < cy + btn_h) {
                    g_selected_sub_item = i;
                    execute_settings_action(i);
                    render_screen();
                    return;
                }
            }
        } else if (g_settings_subview == SETTINGS_VIEW_UPDATE) {
            int item_y_start = 118, item_h = 30, spacing = 3;
            for (int i = 0; i < 5; i++) {
                int cy = item_y_start + i * (item_h + spacing);
                if (y >= cy && y < cy + item_h) {
                    g_selected_sub_item = i;
                    execute_settings_action(i);
                    render_screen();
                    return;
                }
            }
        } else {
            int start_y = 40;
            int count = get_settings_sub_count();
            int card_h = (count >= 7) ? 30 : ((count >= 6) ? 34 : 36);
            int spacing = (count >= 7) ? 3 : ((count >= 6) ? 4 : 5);
            for (int i = 0; i < count; i++) {
                int cy = start_y + i * (card_h + spacing);
                if (y >= cy && y < cy + card_h) {
                    g_selected_sub_item = i;
                    execute_settings_action(i);
                    render_screen();
                    return;
                }
            }
        }
    } else if (g_current_view == VIEW_PAIRING) {
        int opt_y = 42 + 114, opt_h = 40, spacing = 10;
        for (int i = 0; i < PAIR_OPT_COUNT; i++) {
            int cy = opt_y + i * (opt_h + spacing);
            if (y >= cy && y < cy + opt_h) {
                g_selected_pair_opt = i;
                execute_pairing_menu_opt(i);
                return;
            }
        }
    } else if (g_current_view == VIEW_NOT_PAIRED) {
        if (y >= 190 && y <= 265 && x >= 16 && x <= FB_WIDTH - 16) {
            printf("[*] Touch tap 'PAIR TO HUB' -> switch to VIEW_PAIRING\n");
            g_current_view = VIEW_PAIRING;
            g_selected_pair_opt = 0;
            render_screen();
            return;
        }
    }
}

void handle_navigation_key(int code, int value) {
    if (value != 1 && value != 2) return;
    reset_idle_timer();

    if (g_confirm_pending_idx >= 0 || g_confirm_pending_rf_code != 0 || g_unpair_confirm_active) {
        if (code == 158 || code == 202 || code == 149 || code == 203 || (g_cap_buttons_enabled && (code == 364 || code == 102))) {
            printf("[*] Confirm cancelled by back/nav key (code=%d)\n", code);
            g_confirm_pending_idx = -1;
            g_confirm_pending_deadline_ms = 0;
            g_confirm_pending_rf_code = 0;
            g_confirm_pending_rf_deadline_ms = 0;
            g_unpair_confirm_active = false;
            show_notice(NULL, 0);
            render_screen();
            return;
        }
    }

    if (code == 364) {
        if (!g_cap_buttons_enabled) return;
        if (g_is_switching_activity || g_is_syncing_config || g_is_ota_updating) return;
        if (!is_remote_paired()) {
            show_notice("Remote not paired - pair first", 2500);
            return;
        }
        if (g_current_view == VIEW_ACTIVITIES) return;
        printf("[*] Capacitive key pressed: Activities (code=364)\n");
        g_current_view = VIEW_ACTIVITIES;
        render_screen();
        return;
    }
    if (code == 102) {
        if (!g_cap_buttons_enabled) return;
        if (g_is_switching_activity || g_is_syncing_config || g_is_ota_updating) return;
        if (!is_remote_paired()) {
            show_notice("Remote not paired - pair first", 2500);
            return;
        }
        if (g_current_view == VIEW_DEVICES) return;
        printf("[*] Capacitive key pressed: Devices (code=102)\n");
        g_current_view = VIEW_DEVICES;
        render_screen();
        return;
    }

    if (g_is_switching_activity || g_is_syncing_config || g_is_ota_updating) return;

    if (!g_screen_nav_mode && g_touch_enabled && g_current_view != VIEW_SETTINGS && g_current_view != VIEW_PAIRING) {
        return;
    }

    switch (code) {
        case 78:  /* KEY_KPPLUS (RockerUp) */
            if (g_current_view == VIEW_DEVICES) {
                int count = get_active_dev_cmd_count();
                if (count > 0) g_selected_device_cmd = (g_selected_device_cmd > 0) ? g_selected_device_cmd - 1 : count - 1;
            } else if (g_current_view == VIEW_ACTIVITIES) {
                int count = activity_list_count();
                if (count > 0) g_selected_activity = (g_selected_activity > 0) ? g_selected_activity - 1 : count - 1;
            } else if (g_current_view == VIEW_DEVICE_BROWSER) {
                device_browser_move(-1);
            } else if (g_current_view == VIEW_SETTINGS) {
                if (g_settings_subview == SETTINGS_VIEW_CATEGORIES) {
                    g_selected_category = (g_selected_category + 5) % 6;
                } else {
                    int count = get_settings_sub_count();
                    if (count > 0) g_selected_sub_item = (g_selected_sub_item + count - 1) % count;
                }
            } else if (g_current_view == VIEW_PAIRING) {
                if (g_selected_pair_opt > 0) g_selected_pair_opt--;
                else g_selected_pair_opt = PAIR_OPT_COUNT - 1;
            }
            render_screen();
            break;

        case 74:  /* KEY_KPMINUS (RockerDown) */
            if (g_current_view == VIEW_DEVICES) {
                int count = get_active_dev_cmd_count();
                if (count > 0) g_selected_device_cmd = (g_selected_device_cmd < count - 1) ? g_selected_device_cmd + 1 : 0;
            } else if (g_current_view == VIEW_ACTIVITIES) {
                int count = activity_list_count();
                if (count > 0) g_selected_activity = (g_selected_activity < count - 1) ? g_selected_activity + 1 : 0;
            } else if (g_current_view == VIEW_DEVICE_BROWSER) {
                device_browser_move(1);
            } else if (g_current_view == VIEW_SETTINGS) {
                if (g_settings_subview == SETTINGS_VIEW_CATEGORIES) {
                    g_selected_category = (g_selected_category + 1) % 6;
                } else {
                    int count = get_settings_sub_count();
                    if (count > 0) g_selected_sub_item = (g_selected_sub_item + 1) % count;
                }
            } else if (g_current_view == VIEW_PAIRING) {
                if (g_selected_pair_opt < PAIR_OPT_COUNT - 1) g_selected_pair_opt++;
                else g_selected_pair_opt = 0;
            }
            render_screen();
            break;

        case 149: /* KEY_PROG2 (Prev Tab) */
            if ((g_current_view == VIEW_SETTINGS || g_current_view == VIEW_PAIRING) && g_settings_dirty) {
                g_settings_dirty = false;
            }
            if (!is_remote_paired()) {
                if (g_current_view == VIEW_PAIRING) {
                    g_current_view = VIEW_SETTINGS;
                    g_settings_subview = SETTINGS_VIEW_SYSTEM;
                    g_selected_sub_item = 1;
                } else if (g_current_view == VIEW_SETTINGS) {
                    if (g_settings_subview == SETTINGS_VIEW_REBOOT || g_settings_subview == SETTINGS_VIEW_FACTORY_RESET || g_settings_subview == SETTINGS_VIEW_SUSPEND) {
                        bool was_reboot = (g_settings_subview == SETTINGS_VIEW_REBOOT);
                        bool was_suspend = (g_settings_subview == SETTINGS_VIEW_SUSPEND);
                        g_settings_subview = was_suspend ? SETTINGS_VIEW_POWER : SETTINGS_VIEW_SYSTEM;
                        g_factory_reset_stage = 0;
                        g_suspend_countdown_start_ms = 0;
                        g_selected_sub_item = was_suspend ? 4 : (was_reboot ? 2 : 3);
                    } else if (g_settings_subview != SETTINGS_VIEW_CATEGORIES) {
                        g_settings_subview = SETTINGS_VIEW_CATEGORIES;
                        g_factory_reset_stage = 0;
                        g_suspend_countdown_start_ms = 0;
                    } else {
                        g_current_view = VIEW_NOT_PAIRED;
                    }
                } else if (g_current_view == VIEW_NOT_PAIRED) {
                    g_current_view = VIEW_SETTINGS;
                    g_settings_subview = SETTINGS_VIEW_CATEGORIES;
                }
                render_screen();
                break;
            }
            if (g_current_view == VIEW_DEVICE_BROWSER) {
                if (g_browse_dev >= 0) {
                    device_browser_back();
                } else {
                    g_current_view = VIEW_DEVICES;
                }
            } else if (g_current_view == VIEW_PAIRING) {
                g_current_view = VIEW_SETTINGS;
                g_settings_subview = SETTINGS_VIEW_SYSTEM;
                g_selected_sub_item = 1;
            } else if (g_current_view == VIEW_SETTINGS) {
                if (g_settings_subview == SETTINGS_VIEW_REBOOT || g_settings_subview == SETTINGS_VIEW_FACTORY_RESET || g_settings_subview == SETTINGS_VIEW_SUSPEND) {
                    bool was_reboot = (g_settings_subview == SETTINGS_VIEW_REBOOT);
                    bool was_suspend = (g_settings_subview == SETTINGS_VIEW_SUSPEND);
                    g_settings_subview = was_suspend ? SETTINGS_VIEW_POWER : SETTINGS_VIEW_SYSTEM;
                    g_factory_reset_stage = 0;
                    g_suspend_countdown_start_ms = 0;
                    g_selected_sub_item = was_suspend ? 4 : (was_reboot ? 2 : 3);
                } else if (g_settings_subview != SETTINGS_VIEW_CATEGORIES) {
                    g_settings_subview = SETTINGS_VIEW_CATEGORIES;
                    g_factory_reset_stage = 0;
                    g_suspend_countdown_start_ms = 0;
                } else {
                    open_device_browser();
                }
            } else if (g_current_view == VIEW_DEVICES) {
                g_current_view = VIEW_ACTIVITIES;
            } else if (g_current_view == VIEW_ACTIVITIES) {
                g_current_view = VIEW_SETTINGS;
                g_settings_subview = SETTINGS_VIEW_CATEGORIES;
            }
            render_screen();
            break;

        case 203: /* KEY_PROG4 (Next Tab) */
            if ((g_current_view == VIEW_SETTINGS || g_current_view == VIEW_PAIRING) && g_settings_dirty) {
                g_settings_dirty = false;
            }
            if (!is_remote_paired()) {
                if (g_current_view == VIEW_PAIRING) {
                    g_current_view = VIEW_SETTINGS;
                    g_settings_subview = SETTINGS_VIEW_SYSTEM;
                    g_selected_sub_item = 3;
                } else if (g_current_view == VIEW_NOT_PAIRED) {
                    g_current_view = VIEW_SETTINGS;
                    g_settings_subview = SETTINGS_VIEW_CATEGORIES;
                } else if (g_current_view == VIEW_SETTINGS) {
                    if (g_settings_subview != SETTINGS_VIEW_CATEGORIES) {
                        g_settings_subview = SETTINGS_VIEW_CATEGORIES;
                        g_factory_reset_stage = 0;
                        g_suspend_countdown_start_ms = 0;
                    } else {
                        g_current_view = VIEW_NOT_PAIRED;
                    }
                }
                render_screen();
                break;
            }
            if (g_current_view == VIEW_PAIRING) {
                g_current_view = VIEW_SETTINGS;
                g_settings_subview = SETTINGS_VIEW_SYSTEM;
                g_selected_sub_item = 3;
            } else if (g_current_view == VIEW_ACTIVITIES) {
                g_current_view = VIEW_DEVICES;
            } else if (g_current_view == VIEW_DEVICES) {
                open_device_browser();
            } else if (g_current_view == VIEW_DEVICE_BROWSER) {
                g_current_view = VIEW_SETTINGS;
                g_settings_subview = SETTINGS_VIEW_CATEGORIES;
            } else if (g_current_view == VIEW_SETTINGS) {
                if (g_settings_subview != SETTINGS_VIEW_CATEGORIES) {
                    g_settings_subview = SETTINGS_VIEW_CATEGORIES;
                    g_factory_reset_stage = 0;
                    g_suspend_countdown_start_ms = 0;
                } else {
                    g_current_view = VIEW_ACTIVITIES;
                }
            }
            render_screen();
            break;

        case 148: /* KEY_PROG1 (Select / OK) */
            if (g_confirm_pending_rf_code != 0 && get_monotonic_ms() < g_confirm_pending_rf_deadline_ms) {
                int pending_rf = g_confirm_pending_rf_code;
                g_confirm_pending_rf_code = 0;
                g_confirm_pending_rf_deadline_ms = 0;
                show_notice(NULL, 0);
                activity_trigger_btn_t *matched_trig = NULL;
                for (int t = 0; t < g_act_trigger_count; t++) {
                    if ((g_act_triggers[t].rf_key_code != 0 && g_act_triggers[t].rf_key_code == pending_rf) ||
                        strcasecmp(g_act_triggers[t].button_name, rf_short_button_name(pending_rf)) == 0) {
                        matched_trig = &g_act_triggers[t];
                        break;
                    }
                }
                if (matched_trig) {
                    const char *target_id = matched_trig->target_act_id;
                    if (matched_trig->atype == 0x03) target_id = "-1";
                    const char *target_name = "PowerOff";
                    if (strcmp(target_id, "-1") != 0) {
                        for (int a = 0; a < g_activity_count; a++) {
                            if (strcmp(g_activities[a].id, target_id) == 0) {
                                target_name = g_activities[a].name;
                                break;
                            }
                        }
                    }
                    trigger_activity_switch(target_id, target_name);
                    render_screen();
                    break;
                }
            }
            if (g_confirm_pending_idx >= 0 && get_monotonic_ms() < g_confirm_pending_deadline_ms) {
                int idx = g_confirm_pending_idx;
                g_confirm_pending_idx = -1;
                g_confirm_pending_deadline_ms = 0;
                show_notice(NULL, 0);
                trigger_activity_switch(g_activities[idx].id, g_activities[idx].name);
                render_screen();
                break;
            }
            if (g_current_view == VIEW_ACTIVITIES) {
                if (g_selected_activity >= 0 && g_selected_activity < g_activity_count) {
                    request_activity_switch_with_confirm(g_selected_activity);
                }
            } else if (g_current_view == VIEW_DEVICE_BROWSER) {
                device_browser_activate(g_browse_sel);
                render_screen();
            } else if (g_current_view == VIEW_DEVICES) {
                execute_active_dev_cmd(g_selected_device_cmd);
                render_screen();
            } else if (g_current_view == VIEW_SETTINGS) {
                if (g_settings_subview == SETTINGS_VIEW_CATEGORIES) {
                    execute_settings_action(g_selected_category);
                } else {
                    execute_settings_action(g_selected_sub_item);
                }
                render_screen();
            } else if (g_current_view == VIEW_PAIRING) {
                execute_pairing_menu_opt(g_selected_pair_opt);
            } else if (g_current_view == VIEW_NOT_PAIRED) {
                g_current_view = VIEW_PAIRING;
                g_selected_pair_opt = 0;
                render_screen();
            }
            break;

        case 158: case 202: /* KEY_BACK / KEY_PROG3 */
            if (g_current_view == VIEW_DEVICE_BROWSER) {
                device_browser_back();
            } else if (g_current_view == VIEW_PAIRING) {
                if (g_unpair_confirm_active) {
                    g_unpair_confirm_active = false;
                } else {
                    if (!is_remote_paired()) {
                        g_current_view = VIEW_NOT_PAIRED;
                    } else {
                        g_current_view = VIEW_SETTINGS;
                        g_settings_subview = SETTINGS_VIEW_SYSTEM;
                        g_selected_sub_item = 1;
                    }
                }
            } else if (g_current_view == VIEW_SETTINGS) {
                if (g_settings_subview == SETTINGS_VIEW_REBOOT || g_settings_subview == SETTINGS_VIEW_FACTORY_RESET || g_settings_subview == SETTINGS_VIEW_SUSPEND) {
                    bool was_reboot = (g_settings_subview == SETTINGS_VIEW_REBOOT);
                    bool was_suspend = (g_settings_subview == SETTINGS_VIEW_SUSPEND);
                    g_settings_subview = was_suspend ? SETTINGS_VIEW_POWER : SETTINGS_VIEW_SYSTEM;
                    g_factory_reset_stage = 0;
                    g_suspend_countdown_start_ms = 0;
                    g_selected_sub_item = was_suspend ? 4 : (was_reboot ? 2 : 3);
                } else if (g_settings_subview != SETTINGS_VIEW_CATEGORIES) {
                    g_settings_subview = SETTINGS_VIEW_CATEGORIES;
                    g_factory_reset_stage = 0;
                    g_suspend_countdown_start_ms = 0;
                } else {
                    if (is_remote_paired()) open_device_browser(); else g_current_view = VIEW_NOT_PAIRED;
                }
            } else if (g_current_view == VIEW_NOT_PAIRED) {
                break;
            } else if (g_current_view == VIEW_DEVICES) {
                g_current_view = VIEW_ACTIVITIES;
            } else if (g_current_view == VIEW_ACTIVITIES) {
                g_current_view = VIEW_SETTINGS;
                g_settings_subview = SETTINGS_VIEW_CATEGORIES;
            }
            render_screen();
            break;
        default:
            break;
    }
}
