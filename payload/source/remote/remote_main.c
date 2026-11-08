#include "remote_state.h"
#include "remote_fb.h"
#include "remote_power.h"
#include "remote_config.h"
#include "remote_rf.h"
#include "mfg_id.h"
#include "remote_rf_sync.h"
#include "remote_rf_ota.h"
#include "remote_ui.h"
#include "remote_input.h"
#include "remote_present.h"
#include "remote_ui_scroll.h"
#include "harmony_version.h"
#include "remote_migrate.h"

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

/* Physical keys whose press only closed an overlay; their repeats and release are dropped so the
   hub never gets a button-up, or a repeat, for a press it was not sent */
static uint8_t s_swallowed_keys[(KEY_CNT + 7) / 8];

static bool key_swallowed(int code) {
    return code >= 0 && code < KEY_CNT && (s_swallowed_keys[code / 8] & (1u << (code % 8)));
}

static void key_set_swallowed(int code, bool on) {
    if (code < 0 || code >= KEY_CNT) return;
    if (on) s_swallowed_keys[code / 8] |= (uint8_t)(1u << (code % 8));
    else s_swallowed_keys[code / 8] &= (uint8_t)~(1u << (code % 8));
}

/* Touch controllers that report their own coordinate range (the Clearpad TM2176
 * on the Harmony Ultimate reports 0-782 x 0-1009) are scaled to the framebuffer.
 * Controllers whose range is already close to screen pixels are left as they are. */
static int s_touch_max_x = 0;
static int s_touch_max_y = 0;

static void touch_probe_range(int fd) {
    struct input_absinfo ax, ay;
    s_touch_max_x = 0;
    s_touch_max_y = 0;
    if (ioctl(fd, EVIOCGABS(ABS_X), &ax) != 0 || ioctl(fd, EVIOCGABS(ABS_Y), &ay) != 0) {
        printf("[*] Touch range: EVIOCGABS not available, using raw coordinates\n");
        return;
    }
    printf("[*] Touch range: x=%d..%d y=%d..%d\n", ax.minimum, ax.maximum, ay.minimum, ay.maximum);
    if (ax.maximum >= 2 * FB_WIDTH && ay.maximum >= 2 * FB_HEIGHT) {
        s_touch_max_x = ax.maximum;
        s_touch_max_y = ay.maximum;
        printf("[+] Touch range: scaling to %dx%d\n", FB_WIDTH, FB_HEIGHT);
    }
}

/* Maps 0..max onto 0..out-1 so that every pixel gets an equal share of the raw range
 * (for 0..782 onto 240 pixels, about 3.26 counts each, the last pixel included). */
static int touch_scale(int v, int max, int out) {
    if (max <= 0) return v;
    if (v < 0) v = 0;
    if (v > max) v = max;
    int scaled = v * out / (max + 1);
    if (scaled >= out) scaled = out - 1;
    return scaled;
}

/* The end of a touch: a drag of the home pages or a list settles, anything else is a tap or swipe */
static void touch_released(void) {
    uint64_t ms = get_monotonic_ms();
    if (home_scroll_touch_up(g_touch_cur_x, g_touch_cur_y, ms) ||
        list_scroll_touch_up(g_touch_cur_x, g_touch_cur_y, ms)) {
        reset_idle_timer();
        g_screen_dirty = true;
        return;
    }
    handle_screen_touch_tap(g_touch_cur_x, g_touch_cur_y);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* Builds before the elite -> remote rename kept /data/elite_* files, and an older installer
     * may have started this binary as /data/elite_ui: rename them before any config is read */
    remote_migrate_legacy("/data");

    int lock_fd = open("/var/run/remote_ui.lock", O_CREAT | O_RDWR, 0644);
    if (lock_fd >= 0) {
        if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                fprintf(stderr, "[-] FATAL: Another remote_ui instance is already running! Exiting.\n");
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

    printf("[+] Starting Harmony remote custom interface (remote_ui v%s)...\n", HARMONY_REMOTE_VERSION);
    if (mfg_read_unitid(getenv("HARMONY_MFG_FILE"), g_remote_unit_id, sizeof(g_remote_unit_id)) == 0) {
        printf("[+] Unit id %s (mfg partition)\n", g_remote_unit_id);
    } else {
        g_remote_unit_id[0] = '\0';
        printf("[!] No unit id in the mfg partition: none will be reported to the Hub\n");
    }

    load_settings_conf();
    load_activity_usage();

    g_usb_connected = is_usb_hardware_connected();
    log_power_debug("[*] remote_ui INIT: usb_cable_connected=%d\n", g_usb_connected);
    if (!g_usb_connected) {
        stop_usbeth_and_dropbear();
    } else {
        start_usbeth_and_dropbear();
    }

    sync_time_from_rtc();

    bool do_pairing = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-touch") == 0) {
            g_touch_enabled = false;
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
        show_notice("Screen nav moved to bottom buttons", 4000);
    }

    init_act_triggers();
    load_rf_config();
    if (!g_has_hub_config) {
        load_activities();
    }
    printf("[+] Loaded %d activities (has_hub_config=%d)\n", g_activity_count, g_has_hub_config);
    if (!has_configured_activities() && g_current_view == VIEW_ACTIVITIES) {
        open_device_browser();
    }

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
            } else {
                g_fb_mem = (uint16_t *)mmap(NULL, FB_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, g_fb_fd, 0);
                if (g_fb_mem == MAP_FAILED) g_fb_mem = NULL;
            }
        }

        fb_present_init();
        fb_set_blank(!g_screen_on);
        reset_idle_timer();
        load_wallpaper_asset();

        clearpad_hw_init();
        g_touch_fd = find_touchscreen_fd();
        if (g_touch_fd < 0) {
            perror("[-] Failed to open touchscreen node");
        } else {
            printf("[+] Opened touchscreen fd=%d\n", g_touch_fd);
            touch_probe_range(g_touch_fd);
        }
        if (g_touch_fd >= 0 && !g_touch_enabled) {
            set_backlight_level(g_brightness_val);
        }

        g_gyro_fd = open(GYRO_EVENT_DEV, O_RDONLY | O_NONBLOCK);
        if (g_gyro_fd >= 0) {
            init_gyro_interrupt();
        }
    }

    /*
     * EXCLUSIVE SPI ACCESS REQUIREMENT:
     * cc2544_remote.ko driver does NOT support poll(), select(), or O_NONBLOCK.
     * read() calls block indefinitely until hardware GPIO IRQ. Concurrent opens
     * across processes corrupt CC2544 SPI protocol state and deadlock the kernel
     * thread. remote_ui must be the sole process accessing RFSPI_DEV; supervisor
     * scripts must killall -9 any previous instances prior to starting this process.
     */
    g_rf_fd = open(RFSPI_DEV, O_RDWR);
    if (g_rf_fd >= 0) {
        printf("[+] Opened %s for RF TX/RX\n", RFSPI_DEV);
        uint8_t pkt_init[7] = {0x10, 0xFF, 0x80, 0x00, 0x00, 0x01, 0x00};
        if (write(g_rf_fd, pkt_init, sizeof(pkt_init)) < 0) {}
        usleep(30000);

        /* No 10 FF 80 FD 01 here: in Logitech's firmware that write only comes from the factory RF
         * test menu, and each one steps the CC2544 to its next test mode (reply byte = mode:
         * RX sweep, unmodulated carriers, modulated carriers, ...). Sent at every start, it left
         * the remote at "No Hub" from the fifth start since boot until a reboot. */
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

            bool force_sync = (access(FORCE_SYNC_FILE, F_OK) == 0);
            if (force_sync) {
                unlink(FORCE_SYNC_FILE);
                printf("[*] Found %s: forcing full config sync from Hub...\n", FORCE_SYNC_FILE);
            }

            load_rf_config();
            if (force_sync) {
                g_config_version = 0;
            }
            if (!g_has_hub_config || force_sync) {
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

    for (int retry = 0; retry < 30 && g_btn_fd < 0; retry++) {
        g_btn_fd = open(BTN_EVENT_DEV, O_RDONLY | O_NONBLOCK);
        if (g_btn_fd < 0) {
            usleep(100000);
        }
    }
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

    remote_wdt_init();

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
        bool is_docked = is_remote_docked();
        bool allow_gyro_wake = !is_docked;
        if (g_gyro_fd >= 0 && !g_screen_on && g_gyro_enabled && allow_gyro_wake) {
            FD_SET(g_gyro_fd, &rfds);
            if (g_gyro_fd > max_fd) max_fd = g_gyro_fd;
        }

        struct timeval tv;
        if (g_screen_on && (ui_scroll_settling() || g_screen_dirty)) {
            tv.tv_sec = 0;
            tv.tv_usec = 0; /* draw now; settle frames are paced by fb_present() */
        } else if (g_screen_on) {
            tv.tv_sec = 0;
            tv.tv_usec = 100000;
        } else {
            bool in_deep = (g_deep_standby_min > 0 && standby_s >= (uint64_t)g_deep_standby_min * 60);
            tv.tv_sec = (!is_docked && in_deep) ? 30 : 2;
            tv.tv_usec = 0;
        }
        int sel = select(max_fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            break;
        }

        uint64_t now_ms = get_monotonic_ms();
        remote_wdt_ping();
        bool has_paired_hub = (g_paired_hub_addr[0] != 0 || g_paired_hub_addr[1] != 0 ||
                               g_paired_hub_addr[2] != 0 || g_paired_hub_addr[3] != 0);
        if (g_screen_on && g_rf_fd >= 0 && g_pairing_state != 1 && has_paired_hub && !g_is_syncing_config && !g_is_ota_updating && (now_ms - last_sync_query_ms >= 800)) {
            rf_send_sync_query();
            last_sync_query_ms = now_ms;
        }

        static uint64_t last_reconnect_pulse_ms = 0;
        if (!g_rf_link_connected && g_screen_on && g_rf_fd >= 0 && g_pairing_state != 1 && has_paired_hub && (now_ms - last_reconnect_pulse_ms >= 2000)) {
            rf_wake_and_reconnect();
            last_reconnect_pulse_ms = now_ms;
        }


        if (g_act_key_down_ms > 0 && !g_act_key_long_fired && now_ms - g_act_key_down_ms >= ACT_KEY_LONG_MS) {
            g_act_key_long_fired = true;
            printf("[*] Activities key held: opening the menu\n");
            ui_menu_open();
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
        uint32_t usb_check_interval = g_usb_connected ? 1000 : 2500;
        if (now_ms - last_usb_check_ms >= usb_check_interval) {
            last_usb_check_ms = now_ms;
            bool usb_now = is_usb_hardware_connected();
            if (usb_now != g_usb_connected) {
                g_usb_connected = usb_now;
                if (g_usb_connected) {
                    log_power_debug("[+] USB cable plugged: starting USB Ethernet and Dropbear...\n");
                    start_usbeth_and_dropbear();
                } else {
                    log_power_debug("[-] USB cable disconnected: stopping Dropbear and USB Ethernet...\n");
                    stop_usbeth_and_dropbear();
                }
                g_screen_dirty = true;
            }
        }

        static uint64_t last_battery_report_ms = 0;
        static uint8_t last_charging_state = 0xFF;
        static uint8_t last_reported_percent = 0xFF;
        /* The battery read takes about 4 ms, so it waits until a drag or settle ends */
        if (g_rf_fd >= 0 && g_rf_link_connected && !g_is_ota_updating && !ui_scroll_active()) {
            read_remote_battery();
            bool pct_changed = (last_reported_percent == 0xFF || abs((int)g_battery_info.percent - (int)last_reported_percent) >= 2);
            bool chg_changed = (g_battery_info.charging != last_charging_state);
            bool interval_elapsed = (last_battery_report_ms == 0 || now_ms - last_battery_report_ms >= 300000);
            if (interval_elapsed || chg_changed || (g_screen_on && pct_changed)) {
                last_charging_state = g_battery_info.charging;
                last_reported_percent = g_battery_info.percent;
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

        ui_help_tick(now_ms);

        if (g_confirm_pending_idx >= 0 && now_ms >= g_confirm_pending_deadline_ms) {
            g_confirm_pending_idx = -1;
            g_screen_dirty = true;
        }
        if (g_confirm_pending_rf_code != 0 && now_ms >= g_confirm_pending_rf_deadline_ms) {
            g_confirm_pending_rf_code = 0;
            g_confirm_pending_rf_deadline_ms = 0;
            g_screen_dirty = true;
        }

        static uint64_t last_link_check_ms = 0;
        static link_quality_t last_link = { LINK_NO_HUB, 0, -1, -1 };
        if (g_screen_on && now_ms - last_link_check_ms >= (g_link_panel_open ? 500 : 1000)) {
            last_link_check_ms = now_ms;
            link_quality_t q;
            link_quality(now_ms, &q);
            if (g_link_panel_open || q.state != last_link.state || q.bars != last_link.bars) g_screen_dirty = true;
            last_link = q;
        }

        static int last_rendered_minute = -1;
        time_t cur_time = time(NULL);
        struct tm *tm_cur = localtime(&cur_time);
        int cur_min = tm_cur ? tm_cur->tm_min : 0;
        if (cur_min != last_rendered_minute && g_screen_on) {
            last_rendered_minute = cur_min;
            g_screen_dirty = true;
        }

        /* Config sync watchdog: if a sync is active and no packets arrived for 15s (with total) or 30s (without total) */
        pthread_mutex_lock(&g_sync_lock);
        uint64_t last_sync_packet_ms = g_sync_last_packet_ms;
        uint64_t silent_ms = (now_ms > last_sync_packet_ms) ? (now_ms - last_sync_packet_ms) : 0;
        bool restore_config = false;
        if (g_is_syncing_config && silent_ms >= (g_sync_total > 0 ? 15000 : 30000)) {
            printf("[!] Config sync stalled (%llus without packets). Unlocking UI.\n",
                   (unsigned long long)(silent_ms / 1000));
            g_is_syncing_config = false;
            g_sync_failed_ms = now_ms;
            restore_config = true;
        }
        pthread_mutex_unlock(&g_sync_lock);
        if (restore_config) {
            if (g_has_hub_config) {
                init_act_triggers();
                load_rf_config();
            }
            g_screen_dirty = true;
        }

        /* Keep sync overlay animated while input is locked */
        static uint64_t last_sync_render_ms = 0;
        if (g_is_syncing_config && now_ms - last_sync_render_ms >= 500) {
            last_sync_render_ms = now_ms;
            g_screen_dirty = true;
        }

        static uint64_t last_ota_stall_sync_ms = 0;
        if (g_is_ota_updating) {
            /* The receiver thread stamps each chunk; a chunk that arrived after now_ms was read
             * is newer than now_ms, and an unsigned now_ms - stamp wrapped to a "timeout" that
             * dropped the remote out of OTA mode mid-transfer */
            uint64_t ota_quiet_ms = ms_since(now_ms, g_ota_last_packet_ms);
            if (ota_quiet_ms >= 5000 && now_ms - last_ota_stall_sync_ms >= 10000) {
                last_ota_stall_sync_ms = now_ms;
                rf_wake_and_reconnect();
            }
            if (ota_quiet_ms >= 60000) {
                printf("[!] OTA update timed out (60s without packets). UI unlocked (progress saved for resume).\n");
                ota_set_updating(false);
                if (g_ota_file) {
                    fflush(g_ota_file);
                    fclose(g_ota_file);
                    g_ota_file = NULL;
                }
                g_screen_dirty = true;
            }
        }

        /* Redraw once when the charging page times out */
        if (g_charging_page_until_ms != 0 && !ui_charging_page_active()) {
            g_charging_page_until_ms = 0;
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
                g_selected_sub_item = 0;
                g_screen_dirty = true;
            } else {
                static uint64_t s_last_cd_render = 0;
                if (now_ms - s_last_cd_render >= 200) {
                    g_screen_dirty = true;
                    s_last_cd_render = now_ms;
                }
            }
        }

        if (home_scroll_tick(get_monotonic_ms())) g_screen_dirty = true;
        if (list_scroll_tick(get_monotonic_ms())) g_screen_dirty = true;
        if (g_screen_dirty && g_screen_on) {
            render_screen();
        }

        frame_timing_poll();
        if (access("/tmp/trigger_rf_pair", F_OK) == 0) {
            unlink("/tmp/trigger_rf_pair");
            printf("[*] External trigger: starting RF pairing...\n");
            rf_start_pairing();
            g_screen_dirty = true;
        }
        if (access("/tmp/trigger_wake", F_OK) == 0) {
            unlink("/tmp/trigger_wake");
            printf("[*] External trigger: waking the screen\n");
            reset_idle_timer();
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

                    /* A new press means the release was missed; it acts as usual */
                    if (key_swallowed(ev.code)) {
                        key_set_swallowed(ev.code, ev.value == 2);
                        if (ev.value != 1) continue;
                    }

                    if (g_is_switching_activity || g_is_syncing_config || g_is_ota_updating) continue;

                    if (ev.value == 1 && (ui_charging_page_dismiss() || ui_key_hint_dismiss())) {
                        key_set_swallowed(ev.code, true);
                        continue;
                    }
                    if (ev.value == 1) ui_link_panel_dismiss(); /* the key still acts */
                    /* The open menu takes the D-pad, OK and Back; releases pass through */
                    if (g_menu_open && ev.value != 0 && ui_menu_handle_key(ev.code)) continue;
                    /* Help takes the D-pad, OK and Back; other keys, such as volume, still act */
                    if (!g_menu_open && ui_help_active() && ev.value != 0 && ui_help_handle_key(ev.code)) continue;

                    uint16_t rf_code = linux_to_harmony_rf_key(ev.code);

                    /* With physical navigation enabled, the bottom keys drive the screen on all views (KEY_BACK passes through to RF) */
                    if (g_physical_nav_enabled) {
                        if (ev.code == 78 || ev.code == 74 || ev.code == 149 || ev.code == 203 ||
                            ev.code == 148 || ev.code == 202 || ev.code == 102 || ev.code == 364) {
                            handle_navigation_key(ev.code, ev.value);
                            continue;
                        }
                    }

                    /* Without touch, and in Settings and Pairing, the bottom keys and Back drive the screen */
                    if (!g_touch_enabled || g_current_view == VIEW_SETTINGS || g_current_view == VIEW_PAIRING) {
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

                        if (matched_trig) {
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
                    bool should_wake = false;
                    if (ev.type == EV_KEY && ev.value == 1) {
                        if (g_cap_buttons_enabled && (ev.code == 102 || ev.code == 364)) {
                            should_wake = true;
                        } else if (g_touch_enabled && ev.code == 330) {
                            should_wake = true;
                        }
                    } else if (g_touch_enabled && ev.type == EV_ABS) {
                        if (ev.code == ABS_PRESSURE && ev.value > 0) {
                            should_wake = true;
                        } else if (ev.code == ABS_X || ev.code == ABS_Y) {
                            should_wake = true;
                        }
                    }
                    if (should_wake) {
                        log_power_debug("[*] Touch / Cap button wake event: type=%d, code=%d, val=%d\n",
                                        ev.type, ev.code, ev.value);
                        reset_idle_timer();
                    }
                    continue;
                }
                if (ev.type == EV_KEY) {
                    if (ev.code == 102 || ev.code == 364) {
                        if (!g_cap_buttons_enabled) continue;
                        printf("[*] Capacitive key pressed (event2): code=%d val=%d\n", ev.code, ev.value);
                        reset_idle_timer();
                        if (ev.value == 1) ui_link_panel_dismiss();
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
                                touch_released();
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
                        g_touch_cur_x = touch_scale(ev.value, s_touch_max_x, FB_WIDTH);
                    } else if (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y) {
                        g_touch_cur_y = touch_scale(ev.value, s_touch_max_y, FB_HEIGHT);
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
                                touch_released();
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
                        home_scroll_touch_down(g_touch_down_x, g_touch_down_y, g_touch_down_ms);
                        list_scroll_touch_down(g_touch_down_x, g_touch_down_y, g_touch_down_ms);
                    } else if (g_touch_is_down) {
                        uint64_t ms = get_monotonic_ms();
                        if (home_scroll_touch_move(g_touch_cur_x, g_touch_cur_y, ms)) g_screen_dirty = true;
                        if (list_scroll_touch_move(g_touch_cur_x, g_touch_cur_y, ms)) g_screen_dirty = true;
                    }
                }
            }
        }

        /* 3. Accelerometer Hardware Motion Wake (event1 / INT1) */
        if (g_gyro_fd >= 0 && FD_ISSET(g_gyro_fd, &rfds)) {
            struct input_event ev[16];
            ssize_t n = read(g_gyro_fd, ev, sizeof(ev));
            bool is_docked = is_remote_docked();
            bool allow_gyro_wake = !is_docked;
            if (n > 0 && !g_screen_on && g_gyro_enabled && allow_gyro_wake) {
                log_power_debug("[*] Accelerometer INT1 motion interrupt, waking display\n");
                reset_idle_timer(); /* wakes and redraws; a second fb_set_blank(false) would reset the touch controller again */
            }
        }
    }

    flush_activity_usage();  /* init.sh stop sends SIGTERM first, which ends the main loop */
    printf("[+] Cleaning up hardware resources...\n");
    remote_wdt_close();
    if (g_touch_fd >= 0) { ioctl(g_touch_fd, EVIOCGRAB, 0); close(g_touch_fd); }
    if (g_gyro_fd >= 0)  { ioctl(g_gyro_fd, EVIOCGRAB, 0);  close(g_gyro_fd); }
    if (g_rf_fd >= 0)    close(g_rf_fd);
    if (g_btn_fd >= 0)   close(g_btn_fd);
    if (g_fb_mem)        munmap(g_fb_mem, g_fb_size);
    if (g_fb_fd >= 0)    close(g_fb_fd);

    return 0;
}
