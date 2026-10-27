#include "elite_state.h"
#include "elite_fb.h"
#include "elite_power.h"
#include "elite_config.h"
#include "elite_rf.h"
#include "elite_rf_sync.h"
#include "elite_rf_ota.h"
#include "elite_ui.h"
#include "elite_input.h"
#include "harmony_version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/input.h>

static void crash_sig_handler(int sig, siginfo_t *info, void *ucontext) {
    (void)ucontext;
    int bfd = open("/sys/class/backlight/ap3156_backlight/brightness", O_WRONLY);
    if (bfd >= 0) {
        write(bfd, "0\n", 2);
        close(bfd);
    }
    int lfd = open("/sys/class/leds/tps6507x:white/brightness", O_WRONLY);
    if (lfd >= 0) {
        write(lfd, "0\n", 2);
        close(lfd);
    }

    FILE *cf = fopen("/data/crash.log", "a");
    if (cf) {
        time_t now = time(NULL);
        char tbuf[32] = {0};
        struct tm *tm = localtime(&now);
        if (tm) strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm);
        else snprintf(tbuf, sizeof(tbuf), "epoch=%ld", (long)now);

        const char *signame = "UNKNOWN";
        if (sig == SIGSEGV) signame = "SIGSEGV (Segmentation Fault)";
        else if (sig == SIGBUS) signame = "SIGBUS (Bus Error)";
        else if (sig == SIGABRT) signame = "SIGABRT (Abort)";
        else if (sig == SIGFPE) signame = "SIGFPE (Floating Point Exception)";
        else if (sig == SIGILL) signame = "SIGILL (Illegal Instruction)";

        void *fault_addr = info ? info->si_addr : NULL;
        int code = info ? info->si_code : 0;

        fprintf(cf, "=== CRASH %s ===\n", tbuf);
        fprintf(cf, "Signal: %d (%s), Code: %d, Fault Addr: %p\n", sig, signame, code, fault_addr);
        fprintf(cf, "State: act='%s', view=%d, screen_on=%d, no_screen=%d, rf_conn=%d, slot=%d\n",
                g_active_activity_id, g_current_view, g_screen_on ? 1 : 0, g_no_screen_mode ? 1 : 0,
                g_rf_link_connected ? 1 : 0, g_remote_slot);
        fprintf(cf, "Uptime mono: %llums\n", (unsigned long long)get_monotonic_ms());

        FILE *sf = fopen("/proc/self/status", "r");
        if (sf) {
            char sline[128];
            while (fgets(sline, sizeof(sline), sf)) {
                if (strncmp(sline, "VmPeak:", 7) == 0 ||
                    strncmp(sline, "VmSize:", 7) == 0 ||
                    strncmp(sline, "VmRSS:", 6) == 0 ||
                    strncmp(sline, "Threads:", 8) == 0) {
                    fputs(sline, cf);
                }
            }
            fclose(sf);
        }
        fprintf(cf, "========================================\n\n");
        fflush(cf);
        fclose(cf);
    }

    sync();
    _exit(128 + sig);
}

static void sig_handler(int sig) {
    (void)sig;
    g_running = false;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    int lock_fd = open("/var/run/elite_ui.lock", O_CREAT | O_RDWR, 0644);
    if (lock_fd >= 0) {
        if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                fprintf(stderr, "[-] FATAL: Another elite_ui instance is already running! Exiting.\n");
                return 1;
            }
            fprintf(stderr, "[-] Warning: flock failed: %s (ignoring non-contention error)\n", strerror(errno));
        }
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGUSR1, sig_wake_handler);
    signal(SIGUSR2, sig_deep_sleep_handler);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_sig_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);

    printf("[+] Starting Harmony Elite Custom Interface (elite_ui v%s)...\n", HARMONY_ELITE_VERSION);

    load_settings_conf();

    g_usb_connected = is_usb_hardware_connected();
    log_power_debug("[*] elite_ui INIT: usbeth_enabled=%d, usb_cable_connected=%d\n",
                    g_usbeth_enabled, g_usb_connected);
    if (!g_usb_connected || !g_usbeth_enabled) {
        stop_usbeth_and_dropbear();
    } else {
        start_usbeth_and_dropbear();
    }

    sync_time_from_rtc();

    bool do_pairing = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-touch") == 0) {
            g_touch_enabled = false;
            g_screen_nav_mode = true;
        }
        if (strcmp(argv[i], "--no-gyro") == 0)   g_gyro_enabled = false;
        if (strcmp(argv[i], "--no-cap") == 0)    g_cap_buttons_enabled = false;
        if (strcmp(argv[i], "--blank") == 0)     g_screen_on = false;
        if (strcmp(argv[i], "--pair") == 0)      do_pairing = true;
        if (strcmp(argv[i], "--no-screen") == 0) g_no_screen_mode = true;
    }

    if (g_no_screen_mode) {
        printf("[+] NO-SCREEN MODE ACTIVE: Headless physical button remote, screen & touch disabled\n");
        g_screen_on = false;
        g_touch_enabled = false;
        g_gyro_enabled = false;
        int bfd = open("/sys/class/backlight/ap3156_backlight/brightness", O_WRONLY);
        if (bfd >= 0) { write(bfd, "0\n", 2); close(bfd); }
    } else if (!g_touch_enabled) {
        g_screen_nav_mode = true;
        show_notice("Screen nav moved to bottom buttons", 4000);
    }

    init_act_triggers();
    load_rf_config();
    if (!g_has_hub_config) {
        load_activities();
    }
    printf("[+] Loaded %d activities (has_hub_config=%d)\n", g_activity_count, g_has_hub_config);

    if (!g_no_screen_mode) {
        g_fb_fd = open(FB_DEV, O_RDWR);
        if (g_fb_fd >= 0) {
            struct fb_fix_screeninfo finfo;
            struct fb_var_screeninfo vinfo;
            if (ioctl(g_fb_fd, FBIOGET_FSCREENINFO, &finfo) == 0 &&
                ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &vinfo) == 0) {
                g_fb_size = finfo.smem_len;
                g_fb_mem = (uint16_t *)mmap(NULL, g_fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fb_fd, 0);
                if (g_fb_mem == MAP_FAILED) g_fb_mem = NULL;

                vinfo.xoffset = 0;
                vinfo.yoffset = 0;
                ioctl(g_fb_fd, FBIOPAN_DISPLAY, &vinfo);
            } else {
                g_fb_mem = (uint16_t *)mmap(NULL, FB_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, g_fb_fd, 0);
                if (g_fb_mem == MAP_FAILED) g_fb_mem = NULL;
            }
        }

        fb_set_blank(!g_screen_on);
        reset_idle_timer();
        load_wallpaper_asset();

        clearpad_hw_init();
        g_touch_fd = find_touchscreen_fd();
        if (g_touch_fd < 0) {
            perror("[-] Failed to open touchscreen node");
        } else {
            printf("[+] Opened touchscreen fd=%d\n", g_touch_fd);
        }
        if (g_touch_fd >= 0 && !g_touch_enabled) {
            set_backlight_level(g_brightness_val);
        }

        g_gyro_fd = open(GYRO_EVENT_DEV, O_RDONLY | O_NONBLOCK);
        if (g_gyro_fd >= 0) {
            init_gyro_interrupt();
        }
    }

    g_rf_fd = open(RFSPI_DEV, O_RDWR);
    if (g_rf_fd >= 0) {
        printf("[+] Opened %s for RF TX/RX\n", RFSPI_DEV);
        uint8_t pkt_init[7] = {0x10, 0xFF, 0x80, 0x00, 0x00, 0x01, 0x00};
        if (write(g_rf_fd, pkt_init, sizeof(pkt_init)) < 0) {}
        usleep(30000);

        uint8_t pkt_ext[7] = {0x10, 0xFF, 0x80, 0xFD, 0x01, 0x00, 0x00};
        if (write(g_rf_fd, pkt_ext, sizeof(pkt_ext)) < 0) {}
        usleep(30000);

        uint8_t pkt_pipe[7] = {0x10, 0xFF, 0x83, 0xB5, 0x03, 0x00, 0x00};
        if (write(g_rf_fd, pkt_pipe, sizeof(pkt_pipe)) < 0) {}
        usleep(20000);
        uint8_t pkt_hub[7] = {0x10, 0xFF, 0x83, 0xB5, 0x20, 0x00, 0x00};
        if (write(g_rf_fd, pkt_hub, sizeof(pkt_hub)) < 0) {}
        usleep(20000);

        uint8_t pkt_power[7] = {0x20, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00};
        if (write(g_rf_fd, pkt_power, sizeof(pkt_power)) < 0) {}
        usleep(30000);

        pthread_t th_rf_rx;
        pthread_create(&th_rf_rx, NULL, rf_rx_worker, NULL);
        usleep(30000);

        if (do_pairing) {
            rf_start_pairing();
        } else {
            uint8_t pkt_dongle0[10] = {0x12, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
            uint8_t pkt_dongle1[10] = {0x12, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
            printf("[*] Writing Dongle Active Sync (10 bytes)...\n");
            if (write(g_rf_fd, pkt_dongle0, sizeof(pkt_dongle0)) < 0) { perror("write dongle sync 0"); }
            if (write(g_rf_fd, pkt_dongle1, sizeof(pkt_dongle1)) < 0) { perror("write dongle sync 1"); }
            fflush(stdout);

            for (int w = 0; w < 10 && !g_rf_link_connected; w++) {
                usleep(50000);
            }

            load_rf_config();
            if (!g_has_hub_config) {
                load_activities();
                rf_send_full_sync_request();
            } else {
                rf_send_sync_query();
            }
            usleep(50000);
        }
    } else {
        load_rf_config();
        if (!g_has_hub_config) {
            load_activities();
        }
    }

    g_btn_fd = open(BTN_EVENT_DEV, O_RDONLY | O_NONBLOCK);
    if (g_btn_fd < 0) {
        perror("open " BTN_EVENT_DEV);
        return 1;
    }
    printf("[+] Listening on %s for physical buttons...\n", BTN_EVENT_DEV);

    pthread_t th_power, th_repeat;
    pthread_create(&th_power, NULL, power_manager, NULL);
    pthread_create(&th_repeat, NULL, key_repeat_thread, NULL);

    if (!is_remote_paired() && g_current_view != VIEW_PAIRING) {
        g_current_view = VIEW_NOT_PAIRED;
    }
    render_screen();

    uint64_t last_sync_query_ms = 0;
    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int max_fd = -1;

        if (g_btn_fd >= 0) {
            FD_SET(g_btn_fd, &rfds);
            if (g_btn_fd > max_fd) max_fd = g_btn_fd;
        }
        if (g_touch_fd >= 0) {
            FD_SET(g_touch_fd, &rfds);
            if (g_touch_fd > max_fd) max_fd = g_touch_fd;
        }
        uint64_t standby_s = (s_last_blank_time_ms > 0) ? (get_monotonic_ms() - s_last_blank_time_ms) / 1000 : 0;
        bool is_docked = is_elite_docked();
        bool allow_gyro_wake = !is_docked;
        if (g_gyro_fd >= 0 && !g_screen_on && g_gyro_enabled && allow_gyro_wake) {
            FD_SET(g_gyro_fd, &rfds);
            if (g_gyro_fd > max_fd) max_fd = g_gyro_fd;
        }

        struct timeval tv;
        if (g_screen_on) {
            tv.tv_sec = 0;
            tv.tv_usec = 100000;
        } else {
            tv.tv_sec = (!is_docked && standby_s >= 1800) ? 30 : 10;
            tv.tv_usec = 0;
        }
        int sel = select(max_fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            break;
        }

        uint64_t now_ms = get_monotonic_ms();
        bool has_paired_hub = (g_paired_hub_addr[0] != 0 || g_paired_hub_addr[1] != 0 ||
                               g_paired_hub_addr[2] != 0 || g_paired_hub_addr[3] != 0);
        if (g_screen_on && g_rf_fd >= 0 && g_pairing_state != 1 && has_paired_hub && (now_ms - last_sync_query_ms >= 800)) {
            rf_send_sync_query();
            last_sync_query_ms = now_ms;
        }

        static uint64_t last_reconnect_pulse_ms = 0;
        if (!g_rf_link_connected && g_screen_on && g_rf_fd >= 0 && g_pairing_state != 1 && has_paired_hub && (now_ms - last_reconnect_pulse_ms >= 2000)) {
            rf_wake_and_reconnect();
            last_reconnect_pulse_ms = now_ms;
        }

        if (g_touch_enabled && g_nav_toggle_mode == 1 && g_toggle_key_down_ms > 0 && !g_toggle_key_triggered) {
            if (now_ms - g_toggle_key_down_ms >= 500) {
                g_screen_nav_mode = !g_screen_nav_mode;
                g_toggle_key_triggered = true;
                printf("[*] Mode toggled by LONG press hold (key %d): %s\n", g_nav_toggle_key, g_screen_nav_mode ? "NAV" : "DEV");
                g_screen_dirty = true;
            }
        }

        static uint64_t last_progress_render_ms = 0;
        if (g_is_switching_activity) {
            if (now_ms - g_switching_start_ms > (uint64_t)g_switching_total_ms + 400) {
                printf("[*] Activity switch complete (elapsed %llums), unlocking screen\n", (unsigned long long)(now_ms - g_switching_start_ms));
                g_is_switching_activity = false;
                g_last_switch_completed_ms = now_ms;
                strncpy(g_last_switch_target_id, g_switching_target_id, sizeof(g_last_switch_target_id) - 1);
                if (strcmp(g_switching_target_id, "-1") != 0) {
                    g_current_view = VIEW_DEVICES;
                } else {
                    g_current_view = VIEW_ACTIVITIES;
                }
                g_screen_dirty = true;
            } else if (now_ms - last_progress_render_ms >= 100) {
                g_screen_dirty = true;
                last_progress_render_ms = now_ms;
            }
        }

        static uint64_t last_usb_check_ms = 0;
        uint32_t usb_check_interval = g_usb_connected ? 1000 : 3000;
        if ((g_usbeth_enabled || (g_screen_on && g_current_view == VIEW_SETTINGS)) &&
            (now_ms - last_usb_check_ms >= usb_check_interval)) {
            last_usb_check_ms = now_ms;
            bool usb_now = is_usb_hardware_connected();
            if (usb_now != g_usb_connected) {
                g_usb_connected = usb_now;
                if (g_usb_connected) {
                    if (g_usbeth_enabled) {
                        log_power_debug("[+] USB cable plugged: starting USB Ethernet and Dropbear...\n");
                        start_usbeth_and_dropbear();
                    } else {
                        log_power_debug("[+] USB cable plugged (Ethernet mode is disabled in settings).\n");
                    }
                } else {
                    log_power_debug("[-] USB cable disconnected: stopping Dropbear and USB Ethernet...\n");
                    stop_usbeth_and_dropbear();
                }
                g_screen_dirty = true;
            }
        }

        static uint64_t last_battery_report_ms = 0;
        static uint8_t last_charging_state = 0xFF;
        if (g_screen_on && g_rf_fd >= 0 && g_rf_link_connected && !g_is_ota_updating) {
            read_elite_battery();
            if (now_ms - last_battery_report_ms >= 3600000 || g_battery_info.charging != last_charging_state) {
                last_charging_state = g_battery_info.charging;
                last_battery_report_ms = now_ms;
                send_battery_status_rf();
            }
        }

        static uint64_t last_version_report_ms = 0;
        if (g_screen_on && g_rf_fd >= 0 && g_rf_link_connected && !g_is_ota_updating) {
            if (last_version_report_ms == 0 || (now_ms - last_version_report_ms >= 86400000ULL)) {
                last_version_report_ms = now_ms;
                send_version_report_rf();
            }
        }

        if (g_confirm_pending_idx >= 0 && now_ms >= g_confirm_pending_deadline_ms) {
            g_confirm_pending_idx = -1;
            g_screen_dirty = true;
        }
        if (g_confirm_pending_rf_code != 0 && now_ms >= g_confirm_pending_rf_deadline_ms) {
            g_confirm_pending_rf_code = 0;
            g_confirm_pending_rf_deadline_ms = 0;
            g_screen_dirty = true;
        }

        static int last_rendered_minute = -1;
        time_t cur_time = time(NULL);
        struct tm *tm_cur = localtime(&cur_time);
        int cur_min = tm_cur ? tm_cur->tm_min : 0;
        if (cur_min != last_rendered_minute && g_screen_on) {
            last_rendered_minute = cur_min;
            g_screen_dirty = true;
        }

        if (g_is_syncing_config && (now_ms - g_sync_last_packet_ms >= 45000)) {
            printf("[!] Config sync timed out (45s without packets). Unlocking UI.\n");
            g_is_syncing_config = false;
            g_screen_dirty = true;
        }

        if (g_is_ota_updating && (now_ms - g_ota_last_packet_ms >= 20000)) {
            printf("[!] OTA update timed out (20s without packets). Unlocking UI.\n");
            g_is_ota_updating = false;
            if (g_ota_file) {
                fclose(g_ota_file);
                g_ota_file = NULL;
            }
            g_screen_dirty = true;
        }

        static bool s_notice_active = false;
        if (g_notice_until_ms > 0) {
            if (now_ms < g_notice_until_ms) {
                s_notice_active = true;
            } else if (s_notice_active) {
                s_notice_active = false;
                g_notice_until_ms = 0;
                g_screen_dirty = true;
            }
        }

        if (g_screen_on && g_current_view == VIEW_SETTINGS && g_settings_subview == SETTINGS_VIEW_SUSPEND && g_suspend_countdown_start_ms > 0) {
            uint64_t elapsed_s = (now_ms - g_suspend_countdown_start_ms) / 1000;
            if (elapsed_s >= 10) {
                g_suspend_countdown_start_ms = 0;
                trigger_manual_suspend_to_ram();
                g_settings_subview = SETTINGS_VIEW_SYSTEM;
                g_selected_sub_item = 4;
                g_screen_dirty = true;
            } else {
                static uint64_t s_last_cd_render = 0;
                if (now_ms - s_last_cd_render >= 200) {
                    g_screen_dirty = true;
                    s_last_cd_render = now_ms;
                }
            }
        }

        if (g_screen_dirty && g_screen_on) {
            render_screen();
        }

        if (access("/tmp/trigger_rf_pair", F_OK) == 0) {
            unlink("/tmp/trigger_rf_pair");
            printf("[*] External trigger: starting RF pairing...\n");
            rf_start_pairing();
            g_screen_dirty = true;
        }
        if (access("/tmp/trigger_rf_unpair", F_OK) == 0) {
            unlink("/tmp/trigger_rf_unpair");
            printf("[*] External trigger: unpairing Remote & wiping bonding...\n");
            rf_unpair();
            g_screen_dirty = true;
        }

        /* 1. Physical Buttons (event0) */
        if (g_btn_fd >= 0 && FD_ISSET(g_btn_fd, &rfds)) {
            struct input_event ev;
            while (read(g_btn_fd, &ev, sizeof(ev)) == sizeof(ev)) {
                if (ev.type == EV_KEY) {
                    reset_idle_timer();
                    if (!g_screen_on) {
                        fb_set_blank(false);
                        render_screen();
                        continue;
                    }

                    if (g_is_switching_activity || g_is_syncing_config || g_is_ota_updating) continue;

                    /* Long-press toggle key latching */
                    if (g_touch_enabled && g_nav_toggle_mode == 1 && ev.code == g_nav_toggle_key) {
                        if (ev.value == 1) {
                            g_toggle_key_down_ms = get_monotonic_ms();
                            g_toggle_key_triggered = false;
                        } else if (ev.value == 0) {
                            g_toggle_key_down_ms = 0;
                            if (g_toggle_key_triggered) {
                                g_toggle_key_triggered = false;
                                continue;
                            }
                        }
                    }

                    /* Short-press toggle key */
                    if (g_touch_enabled && g_nav_toggle_mode == 0 && ev.value == 1 && ev.code == g_nav_toggle_key) {
                        g_screen_nav_mode = !g_screen_nav_mode;
                        printf("[*] Mode toggled by SHORT press (key %d): %s\n", g_nav_toggle_key, g_screen_nav_mode ? "NAV" : "DEV");
                        render_screen();
                        continue;
                    }

                    uint16_t rf_code = linux_to_harmony_rf_key(ev.code);

                    /* If in Screen Navigation Mode, handle navigation buttons locally */
                    if (g_screen_nav_mode || !g_touch_enabled || g_current_view == VIEW_SETTINGS || g_current_view == VIEW_PAIRING) {
                        if (ev.code == 78 || ev.code == 74 || ev.code == 149 || ev.code == 203 ||
                            ev.code == 148 || ev.code == 202 || ev.code == 158 || ev.code == 102 || ev.code == 364) {
                            handle_navigation_key(ev.code, ev.value);
                            continue;
                        }
                    }

                    if (rf_code != 0) {
                        activity_trigger_btn_t *matched_trig = NULL;
                        for (int t = 0; t < g_act_trigger_count; t++) {
                            if ((g_act_triggers[t].rf_key_code != 0 && g_act_triggers[t].rf_key_code == rf_code) ||
                                strcasecmp(g_act_triggers[t].button_name, rf_short_button_name(rf_code)) == 0) {
                                matched_trig = &g_act_triggers[t];
                                break;
                            }
                        }

                        if (matched_trig && g_remote_mode == 0) {
                            if (ev.value == 1) {
                                const char *target_id = matched_trig->target_act_id;
                                if (matched_trig->atype == 0x03) target_id = "-1";

                                if (strcmp(g_active_activity_id, target_id) == 0) {
                                    if (strcmp(target_id, "-1") == 0) {
                                        show_notice("Already Powered Off", 2000);
                                    } else {
                                        show_notice("Activity already active", 2000);
                                    }
                                    g_screen_dirty = true;
                                    continue;
                                }

                                uint64_t now = get_monotonic_ms();
                                if (g_confirm_activity) {
                                    if (g_confirm_pending_rf_code == rf_code && now < g_confirm_pending_rf_deadline_ms) {
                                        g_confirm_pending_rf_code = 0;
                                        g_confirm_pending_rf_deadline_ms = 0;
                                    } else {
                                        g_confirm_pending_rf_code = rf_code;
                                        g_confirm_pending_rf_deadline_ms = now + 4000;
                                        show_notice("Press again or OK to confirm", 3000);
                                        g_screen_dirty = true;
                                        continue;
                                    }
                                }

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
                            }
                            continue;
                        }

                        if (ev.value == 1) {
                            rf_send_button_event(rf_code, 1);
                            pthread_mutex_lock(&g_repeat_mutex);
                            g_held_rf_code = rf_code;
                            pthread_cond_signal(&g_repeat_cond);
                            pthread_mutex_unlock(&g_repeat_mutex);
                        } else if (ev.value == 0) {
                            pthread_mutex_lock(&g_repeat_mutex);
                            if (g_held_rf_code == rf_code) g_held_rf_code = 0;
                            pthread_mutex_unlock(&g_repeat_mutex);
                            rf_send_button_event(rf_code, 0);
                        }
                    }
                }
            }
        }

        /* 2. Touchscreen & Capacitive Buttons (event2) */
        if (g_touch_fd >= 0 && FD_ISSET(g_touch_fd, &rfds)) {
            struct input_event ev;
            while (read(g_touch_fd, &ev, sizeof(ev)) == sizeof(ev)) {
                if (!g_screen_on) {
                    reset_idle_timer();
                    fb_set_blank(false);
                    render_screen();
                    continue;
                }
                if (ev.type == EV_KEY) {
                    if (ev.code == 102 || ev.code == 364) {
                        if (!g_cap_buttons_enabled) continue;
                        printf("[*] Capacitive key pressed (event2): code=%d val=%d\n", ev.code, ev.value);
                        reset_idle_timer();
                        handle_navigation_key(ev.code, ev.value);
                        continue;
                    }
                    if (!g_touch_enabled) continue;
                    if (ev.code == 330) {
                        printf("[*] Touch BTN_TOUCH: val=%d at (%d, %d)\n", ev.value, g_touch_cur_x, g_touch_cur_y);
                        if (ev.value == 0) {
                            if (g_touch_is_down) {
                                g_touch_is_down = false;
                                g_touch_need_down_latch = false;
                                handle_screen_touch_tap(g_touch_cur_x, g_touch_cur_y);
                            }
                        } else {
                            reset_idle_timer();
                            g_touch_is_down = true;
                            g_touch_need_down_latch = true;
                        }
                    }
                } else if (ev.type == EV_ABS) {
                    if (!g_touch_enabled) continue;
                    if (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X) {
                        g_touch_cur_x = ev.value;
                    } else if (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y) {
                        g_touch_cur_y = ev.value;
                    } else if (ev.code == ABS_PRESSURE) {
                        if (ev.value > 0) {
                            reset_idle_timer();
                            if (!g_touch_is_down) {
                                g_touch_is_down = true;
                                g_touch_need_down_latch = true;
                            }
                        } else {
                            if (g_touch_is_down) {
                                g_touch_is_down = false;
                                g_touch_need_down_latch = false;
                                handle_screen_touch_tap(g_touch_cur_x, g_touch_cur_y);
                            }
                        }
                    }
                } else if (ev.type == EV_SYN) {
                    if (g_touch_need_down_latch && g_touch_is_down) {
                        g_touch_down_x = g_touch_cur_x;
                        g_touch_down_y = g_touch_cur_y;
                        g_touch_down_ms = get_monotonic_ms();
                        g_touch_need_down_latch = false;
                        printf("[*] Touch DOWN latched on SYN at (%d, %d)\n", g_touch_down_x, g_touch_down_y);
                    }
                }
            }
        }

        /* 3. Accelerometer Hardware Motion Wake (event1 / INT1) */
        if (g_gyro_fd >= 0 && FD_ISSET(g_gyro_fd, &rfds)) {
            struct input_event ev[16];
            ssize_t n = read(g_gyro_fd, ev, sizeof(ev));
            bool is_docked = is_elite_docked();
            bool allow_gyro_wake = !is_docked;
            if (n > 0 && !g_screen_on && g_gyro_enabled && allow_gyro_wake) {
                log_power_debug("[*] Accelerometer INT1 motion interrupt, waking display\n");
                reset_idle_timer();
                fb_set_blank(false);
                render_screen();
            }
        }
    }

    printf("[+] Cleaning up hardware resources...\n");
    if (g_touch_fd >= 0) { ioctl(g_touch_fd, EVIOCGRAB, 0); close(g_touch_fd); }
    if (g_gyro_fd >= 0)  { ioctl(g_gyro_fd, EVIOCGRAB, 0);  close(g_gyro_fd); }
    if (g_rf_fd >= 0)    close(g_rf_fd);
    if (g_btn_fd >= 0)   close(g_btn_fd);
    if (g_fb_mem)        munmap(g_fb_mem, g_fb_size);
    if (g_fb_fd >= 0)    close(g_fb_fd);

    return 0;
}
