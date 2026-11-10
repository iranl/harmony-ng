#include "remote_power.h"
#include "remote_transition.h"
#include "remote_fb.h"
#include "remote_rf.h"
#include "remote_config.h"
#include "remote_ui.h"
#include "timezone_db.h"
#include "remote_battery.h"
#include "remote_flightlog.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/watchdog.h>

volatile sig_atomic_t g_wake_requested = 0;
char g_timezone_str[64] = TZ_DEFAULT_POSIX;

void expand_timezone(const char *in, char *out, size_t out_sz) {
    timezone_expand_prefix(in, out, out_sz);
}

void set_msp430_rtc(uint32_t epoch_sec) {
    FILE *f = fopen("/sys/bus/i2c/devices/1-0034/time", "w");
    if (!f) return;
    fprintf(f, "%u\n", epoch_sec);
    fclose(f);
}

void sync_time_from_rtc(void) {
    FILE *ftz = fopen("/etc/TZ", "r");
    if (ftz) {
        char tz[64] = {0};
        if (fgets(tz, sizeof(tz), ftz)) {
            char *nl = strchr(tz, '\n'); if (nl) *nl = '\0';
            char *cr = strchr(tz, '\r'); if (cr) *cr = '\0';
            if (tz[0]) {
                expand_timezone(tz, g_timezone_str, sizeof(g_timezone_str));
            }
        }
        fclose(ftz);
    }
    char tz_tmp[64] = {0};
    expand_timezone(g_timezone_str, tz_tmp, sizeof(tz_tmp));
    strncpy(g_timezone_str, tz_tmp, sizeof(g_timezone_str) - 1);
    g_timezone_str[sizeof(g_timezone_str) - 1] = '\0';
    setenv("TZ", g_timezone_str, 1);
    tzset();

    FILE *f = fopen("/sys/bus/i2c/devices/1-0034/time", "r");
    if (!f) return;
    uint32_t epoch_sec = 0;
    if (fscanf(f, "%u", &epoch_sec) == 1 && epoch_sec > 1577836800) {
        struct timeval tv = { .tv_sec = (time_t)epoch_sec, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        printf("[+] Synchronized system time from MSP430 RTC: %u (TZ=%s)\n", epoch_sec, g_timezone_str);
    }
    fclose(f);
}

void set_backlight_level(int level) {
    int fd = open(BACKLIGHT_SYSFS, O_WRONLY);
    if (fd >= 0) {
        char buf[16];
        int val = level * 6; /* 1..5 maps to 6..30 */
        if (val > 31) val = 31;
        snprintf(buf, sizeof(buf), "%d\n", val);
        write(fd, buf, strlen(buf));
        close(fd);
    }
}

void clearpad_wake(bool full_reset) {
    (void)full_reset;
    int fd = open("/dev/i2c-1", O_RDWR);
    if (fd >= 0) {
        if (ioctl(fd, I2C_SLAVE_FORCE, 0x20) >= 0) {
            union i2c_smbus_data data;
            struct i2c_smbus_ioctl_data args;

            data.byte = 0x00;
            args.read_write = I2C_SMBUS_WRITE;
            args.command = 0x35;
            args.size = I2C_SMBUS_BYTE_DATA;
            args.data = &data;
            ioctl(fd, I2C_SMBUS, &args);

            data.byte = 0x07;
            args.command = 0x36;
            ioctl(fd, I2C_SMBUS, &args);

            args.read_write = I2C_SMBUS_READ;
            args.command = 0x14;
            ioctl(fd, I2C_SMBUS, &args);
        }
        close(fd);
    }
}

void clearpad_hw_init(void) {
    clearpad_wake(false);
    printf("[+] Synaptics Clearpad initialized via I2C\n");
}

void clearpad_doze(void) {
    int fd = open("/dev/i2c-1", O_RDWR);
    if (fd >= 0) {
        if (ioctl(fd, I2C_SLAVE_FORCE, 0x20) >= 0) {
            union i2c_smbus_data data;
            struct i2c_smbus_ioctl_data args;

            data.byte = 0x00; /* Operating Mode: automatic hardware doze when untouched */
            args.read_write = I2C_SMBUS_WRITE;
            args.command = 0x35;
            args.size = I2C_SMBUS_BYTE_DATA;
            args.data = &data;
            ioctl(fd, I2C_SMBUS, &args);

            data.byte = 0x07;
            args.command = 0x36;
            ioctl(fd, I2C_SMBUS, &args);

            args.read_write = I2C_SMBUS_READ;
            args.command = 0x14;
            ioctl(fd, I2C_SMBUS, &args);
        }
        close(fd);
    }
}

void clearpad_sleep(void) {
    int fd = open("/dev/i2c-1", O_RDWR);
    if (fd >= 0) {
        if (ioctl(fd, I2C_SLAVE_FORCE, 0x20) >= 0) {
            union i2c_smbus_data data;
            struct i2c_smbus_ioctl_data args;

            data.byte = 0x01;
            args.read_write = I2C_SMBUS_WRITE;
            args.command = 0x35;
            args.size = I2C_SMBUS_BYTE_DATA;
            args.data = &data;
            ioctl(fd, I2C_SMBUS, &args);
        }
        close(fd);
    }
}

bool is_remote_docked(void) {
    return (strcmp(g_battery_info.powersource, "cradle") == 0 ||
            strcmp(g_battery_info.powersource, "usb") == 0 ||
            g_battery_info.charging);
}

void get_cpu_freq_status(char *out, size_t max_len) {
    char cur_freq[32] = "N/A";
    char gov[32] = "N/A";
    FILE *fc = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
    if (fc) {
        if (fscanf(fc, "%31s", cur_freq) != 1) strncpy(cur_freq, "err", sizeof(cur_freq) - 1);
        fclose(fc);
    }
    FILE *fg = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "r");
    if (fg) {
        if (fscanf(fg, "%31s", gov) != 1) strncpy(gov, "err", sizeof(gov) - 1);
        fclose(fg);
    }
    snprintf(out, max_len, "gov=%.16s, cur=%.16s", gov, cur_freq);
}

/* The processor frequency is never changed (it used to drop to 96 MHz while the screen was off).
 * The stock kernel's I2C driver hooks every frequency change: i2c_davinci_cpufreq_transition()
 * puts the I2C controller in reset and recomputes its clock, and the wait that should let a
 * running transfer finish first does not wait (its completion is only ever completed, so it is a
 * counter that is never zero). A frequency change can therefore cut a transfer on the bus the
 * MSP430 keypad controller, the PMIC, the touch controller and the accelerometer share. On two
 * remotes that ended with the remote dark and dead to keys and USB until the battery was pulled,
 * at the first screen blank after a wake from suspend. The stock firmware never writes cpufreq;
 * the kernel default is the performance governor. */

bool is_night_time(void) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (!tm) return false;
    int h = tm->tm_hour;
    if (g_night_start_hour > g_night_end_hour) {
        return (h >= g_night_start_hour || h < g_night_end_hour);
    } else {
        return (h >= g_night_start_hour && h < g_night_end_hour);
    }
}

void fb_set_blank(bool blank) {
    if (blank) transition_reset(); /* the frame before sleep isn't one to slide away from */
    int blank_res = -1;
    if (g_fb_fd >= 0) {
        blank_res = ioctl(g_fb_fd, FBIOBLANK, blank ? FB_BLANK_POWERDOWN : FB_BLANK_UNBLANK);
    }
    int fd = open(FB_BLANK_SYSFS, O_WRONLY);
    if (fd >= 0) {
        const char *val = blank ? "4\n" : "0\n";
        write(fd, val, strlen(val));
        close(fd);
    }
    g_screen_on = !blank;
    uint64_t now_ms = get_boot_ms(); /* the blank time counts a suspend (get_boot_ms) */

    if (g_screen_on) {
        s_last_wake_ms = now_ms;
        uint64_t sleep_sec = s_last_blank_time_ms > 0 ? (now_ms - s_last_blank_time_ms) / 1000 : 0;
        set_backlight_level(g_brightness_val);
        clearpad_wake(false);
        read_remote_battery();
        rf_wake_and_reconnect();
        g_screen_dirty = true;
        flightlog_event("SCREEN_ON", "off_s=%llu", (unsigned long long)sleep_sec);
        log_power_debug("=== WAKING FROM STANDBY (Screen Active) ===\n");
        log_power_debug("  Slept for: %llu seconds | Backlight: %d | Battery: %dmV, %d%% (%s, chg=%d)\n",
                        (unsigned long long)sleep_sec, g_brightness_val,
                        g_battery_info.voltage_mv, g_battery_info.percent,
                        g_battery_info.powersource, g_battery_info.charging);
    } else {
        s_last_blank_time_ms = now_ms;
        set_backlight_level(0);
        clearpad_doze();
        if (g_gyro_fd >= 0) {
            struct input_event ev[16];
            while (read(g_gyro_fd, ev, sizeof(ev)) > 0) {}
        }
        read_remote_battery();
        flightlog_mark("SCREEN_OFF");
        bool usb_plug = (strcmp(g_battery_info.powersource, "usb") == 0);
        log_power_debug("=== ENTERING STANDBY (Screen Blank) ===\n");
        log_power_debug("  fb_blank=%d, bl=0 | USB cable=%d | Battery: %dmV, %d%% (%s, chg=%d)\n",
                        blank_res, usb_plug,
                        g_battery_info.voltage_mv, g_battery_info.percent,
                        g_battery_info.powersource, g_battery_info.charging);
    }
    int lfd = open(BTN_LED_SYSFS, O_WRONLY);
    if (lfd >= 0) {
        bool turn_on = !blank;
        if (turn_on) {
            if (g_btn_backlight_mode == 0) turn_on = false;
            else if (g_btn_backlight_mode == 1) turn_on = true;
            else if (g_btn_backlight_mode == 2) turn_on = is_night_time();
        }
        write(lfd, turn_on ? "255\n" : "0\n", turn_on ? 4 : 2);
        close(lfd);
    }
}

bool is_usb_hardware_connected(void) {
    FILE *fp = fopen("/sys/devices/platform/i2c_davinci.1/i2c-1/1-0048/tps6507x-juniper/powersource", "r");
    if (fp) {
        char psrc[16] = {0};
        if (fscanf(fp, "%15s", psrc) == 1) {
            fclose(fp);
            strncpy(g_battery_info.powersource, psrc, sizeof(g_battery_info.powersource) - 1);
            return (strcmp(psrc, "usb") == 0);
        }
        fclose(fp);
    }
    return false;
}

#define BATT_SAMPLE_COUNT 16
static uint16_t s_batt_samples[BATT_SAMPLE_COUNT];
static int s_batt_sample_idx = 0;
static int s_batt_sample_num = 0;
static uint64_t s_last_batt_sample_ms = 0;
static uint8_t s_filtered_pct = 0xFF;
static remote_batt_filter_t s_batt_filter = { 0xFF, 0 };

time_t ui_wall_time(void) {
    return time(NULL);
}

/* Set by a resume after a suspend longer than REMOTE_BATT_REST_MS: the cell rested all that
 * time, so the next reading is a rested one even if the screen is already on */
static bool s_resume_rested = false;

void read_remote_battery(void) {
    uint64_t now_ms = get_boot_ms(); /* rest, drop and debounce times count a suspend */

    if (s_last_batt_sample_ms > 0 && (now_ms - s_last_batt_sample_ms < 1000) && s_batt_sample_num > 0) {
        return;
    }
    s_last_batt_sample_ms = now_ms;

    /* The PMIC's battery voltage in volts ("4.188"): one Li-ion cell, as the stock remote reads
     * it (juniper/init.lua: default "3.80", low battery below 3.55 V). It used to be multiplied
     * by 2000, which reported twice the cell voltage (8376 mV for 4.188 V). */
    FILE *fv = fopen("/sys/devices/platform/i2c_davinci.1/i2c-1/1-0048/tps6507x-juniper/vbat", "r");
    float v = 0.0f;
    uint16_t raw_mv = g_battery_info.voltage_mv;
    if (fv) {
        if (fscanf(fv, "%f", &v) == 1) {
            if (v > 0.5f && v < 6.0f) raw_mv = (uint16_t)(v * 1000.0f + 0.5f);  /* volts (the PMIC) */
            else if (v > 500.0f && v < 6000.0f) raw_mv = (uint16_t)(v + 0.5f);  /* millivolts (the test mock) */
        }
        fclose(fv);
    }

    int is_charging = 0;
    FILE *fc = fopen("/sys/devices/platform/i2c_davinci.1/i2c-1/1-0048/tps6507x-juniper/charger", "r");
    int chg = 0;
    if (fc) {
        if (fscanf(fc, "%d", &chg) == 1) {
            is_charging = (chg > 0) ? 1 : 0;
        }
        fclose(fc);
    }

    FILE *fp = fopen("/sys/devices/platform/i2c_davinci.1/i2c-1/1-0048/tps6507x-juniper/powersource", "r");
    char psrc[16] = {0};
    if (fp) {
        if (fscanf(fp, "%15s", psrc) == 1) {
            strncpy(g_battery_info.powersource, psrc, sizeof(g_battery_info.powersource) - 1);
            if (strcmp(psrc, "cradle") == 0 || strcmp(psrc, "usb") == 0) {
                is_charging = 1;
            } else if (strcmp(psrc, "battery") == 0) {
                is_charging = 0;
            }
        }
        fclose(fp);
    }
    /* Debounced: lifting the remote off the cradle or setting it down flips the charger and
     * power source readings several times within seconds (remote_battery.h) */
    if (s_filtered_pct == 0xFF) remote_battery_filter_init(&s_batt_filter);
    bool chg_transition = false;
    is_charging = remote_battery_charging(&s_batt_filter, is_charging != 0, now_ms, &chg_transition) ? 1 : 0;
    g_battery_info.charging = is_charging;

    if (s_batt_sample_num == 0 || chg_transition) {
        for (int i = 0; i < BATT_SAMPLE_COUNT; i++) {
            s_batt_samples[i] = raw_mv;
        }
        s_batt_sample_num = BATT_SAMPLE_COUNT;
        s_batt_sample_idx = 0;
    } else {
        s_batt_samples[s_batt_sample_idx] = raw_mv;
        s_batt_sample_idx = (s_batt_sample_idx + 1) % BATT_SAMPLE_COUNT;
    }

    uint32_t sum = 0;
    for (int i = 0; i < BATT_SAMPLE_COUNT; i++) {
        sum += s_batt_samples[i];
    }
    uint16_t avg_mv = (uint16_t)(sum / BATT_SAMPLE_COUNT);
    g_battery_info.voltage_mv = avg_mv;

    /* Rested = the screen has been off long enough for the cell to recover from the load
     * (remote_battery.h); the percentage follows the rested voltage and only creeps down under
     * load, so a wake no longer walks it down 2 points at a time */
    bool rested = (!g_screen_on && s_last_blank_time_ms > 0 &&
                   now_ms - s_last_blank_time_ms >= REMOTE_BATT_REST_MS) || s_resume_rested;
    s_resume_rested = false;
    s_filtered_pct = remote_battery_filter_step(&s_batt_filter, g_battery_info.voltage_mv, is_charging,
                                               chg_transition, rested, now_ms);
    g_battery_info.percent = s_filtered_pct;
}

void stop_usbeth_and_dropbear(void) {
    log_power_debug("[*] Stopping USB Ethernet and Dropbear synchronously...\n");
    system("killall -9 dropbear dropbearmulti usbhid 2>/dev/null; "
           "/usr/bin/usbeth stop 2>/dev/null; "
           "rmmod g_ether 2>/dev/null; "
           "ifconfig usb0 down 2>/dev/null; "
           "chmod -x /usr/bin/usbhid 2>/dev/null; "
           "killall -9 usbhid 2>/dev/null; true");
    log_power_debug("[+] USB Ethernet stopped and Dropbear killed.\n");
}

void start_usbeth_and_dropbear(void) {
    log_power_debug("[*] Starting USB Ethernet and Dropbear...\n");
    system("chmod -x /usr/bin/usbhid 2>/dev/null; killall -9 usbhid 2>/dev/null; "
           "if [ ! -d /sys/class/net/usb0 ]; then /usr/bin/usbeth start 2>/dev/null & fi; "
           "if ! pidof dropbear >/dev/null 2>&1 && ! pidof dropbearmulti >/dev/null 2>&1; then "
           "/data/dropbear -R -p 0.0.0.0:22 2>/dev/null || /data/dropbear -R -B 2>/dev/null || true & fi");
    log_power_debug("[+] USB Ethernet and Dropbear started.\n");
}

uint64_t g_suspend_hold_until_ms = 0;

bool remote_deep_standby_due(uint64_t standby_s) {
    if (g_deep_standby_min == 0) return false;
    if (g_deep_standby_min < 0) return standby_s >= REMOTE_SUSPEND_AT_ONCE_S;
    return standby_s >= (uint64_t)g_deep_standby_min * 60;
}

bool remote_should_suspend(uint64_t standby_s, uint64_t now_ms) {
    if (is_remote_docked()) return false;
    if (strcmp(g_battery_info.powersource, "battery") != 0) return false;
    if (g_is_syncing_config || g_is_ota_updating) return false;
    if (now_ms < g_suspend_hold_until_ms) return false;
    return remote_deep_standby_due(standby_s);
}

void remote_power_report_sent(void) {
    uint64_t now = get_monotonic_ms();
    if (g_suspend_hold_until_ms > now + 800) g_suspend_hold_until_ms = now + 800; /* time for the ACK */
}

void remote_flightlog_context(char *buf, size_t buf_sz) {
    uint64_t now = get_monotonic_ms();
    uint64_t last = g_last_hub_frame_ms; /* stamped by the RF thread, may be newer than now */
    char hub[24] = "-";
    if (last > 0) {
        uint64_t age = now >= last ? now - last : 0;
        snprintf(hub, sizeof(hub), "%llu.%llus", (unsigned long long)(age / 1000),
                 (unsigned long long)((age % 1000) / 100));
    }
    snprintf(buf, buf_sz, "batt=%umV %u%% %s chg=%u scr=%d act=%s sw=%d sync=%d ota=%d link=%d hubrx=%s",
             (unsigned)g_battery_info.voltage_mv, (unsigned)g_battery_info.percent,
             g_battery_info.powersource[0] ? g_battery_info.powersource : "?", (unsigned)g_battery_info.charging,
             g_screen_on ? 1 : 0, g_active_activity_id[0] ? g_active_activity_id : "-",
             g_is_switching_activity ? 1 : 0, g_is_syncing_config ? 1 : 0, g_is_ota_updating ? 1 : 0,
             g_rf_link_connected ? 1 : 0, hub);
}

void enter_kernel_suspend(void) {
    flush_activity_usage();
    read_remote_battery();
    if (strcmp(g_battery_info.powersource, "battery") != 0) {
        return;
    }
    if (g_usb_connected) {
        g_usb_connected = false;
        stop_usbeth_and_dropbear();
    }

    /* On flash before the kernel is asked to sleep: if the remote never comes back, this is the
     * last record and the next start reports it (remote_flightlog.h) */
    flightlog_event(FLIGHTLOG_EV_SUSPEND_ENTER, "state=%s wakeup_s=3600", REMOTE_SLEEP_STATE);

    int lfd = open(BTN_LED_SYSFS, O_WRONLY);
    if (lfd >= 0) {
        write(lfd, "0\n", 2);
        close(lfd);
    }
    remote_wdt_close();

    int mfd = open("/sys/bus/i2c/devices/1-0034/wakeup", O_WRONLY);
    if (mfd >= 0) {
        write(mfd, "3600\n", 5);
        close(mfd);
    }

    log_power_debug("[*] Entering kernel suspend (echo %s > /sys/power/state)...\n", REMOTE_SLEEP_STATE);
    uint64_t wall_before = get_wall_ms(), mono_before = get_monotonic_ms();
    int fd = open("/sys/power/state", O_WRONLY);
    if (fd >= 0) {
        ssize_t w = write(fd, REMOTE_SLEEP_STATE "\n", sizeof(REMOTE_SLEEP_STATE "\n") - 1);
        int write_errno = errno;
        close(fd);
        if (w < 0) {
            log_power_debug("[-] Kernel suspend returned error: %s\n", strerror(write_errno));
            remote_wdt_init();
            flightlog_event(FLIGHTLOG_EV_SUSPEND_RETURN, "rc=error errno=%d (%s) awake_ms=%llu", write_errno,
                            strerror(write_errno), (unsigned long long)(get_monotonic_ms() - mono_before));
            flightlog_kernel();
            usleep(5000000);
            return;
        }
    }
    uint64_t mono_after = get_monotonic_ms();
    uint64_t gap_ms = remote_suspend_gap_ms(wall_before, mono_before, get_wall_ms(), mono_after);
    g_suspended_ms += gap_ms;
    log_power_debug("[*] Resumed from kernel suspend after %llu s\n", (unsigned long long)(gap_ms / 1000));
    remote_wdt_init();
    remote_power_resumed(gap_ms);

    char wev[32] = "unreadable";
    FILE *fev = fopen("/sys/bus/i2c/devices/1-0034/wakeup_event", "r");
    if (fev) {
        if (fscanf(fev, "%31s", wev) == 1) {
            log_power_debug("[*] Kernel resume wakeup_event: %s\n", wev);
            if (strcmp(wev, "alarm") != 0) {
                g_wake_requested = 1;
            } else {
                /* the hourly alarm: stay up to link and report the battery, then suspend again */
                g_suspend_hold_until_ms = get_monotonic_ms() + REMOTE_ALARM_WAKE_HOLD_MS;
            }
        }
        fclose(fev);
    } else {
        g_wake_requested = 1;
    }

    /* slept_ms is measured on the wall clock; awake_ms is the time the write itself took on the
     * monotonic clock, which stops while the processor sleeps: the way in plus the way out */
    read_remote_battery();
    flightlog_event(FLIGHTLOG_EV_SUSPEND_RETURN, "rc=%s slept_ms=%llu awake_ms=%llu wakeup_event=%s",
                    fd >= 0 ? "ok" : "open_failed", (unsigned long long)gap_ms,
                    (unsigned long long)(mono_after - mono_before), wev);
    flightlog_kernel();

    rf_wake_and_reconnect();
}

void remote_power_resumed(uint64_t suspended_ms) {
    /* The samples in the average are from before the suspend: start it from the next reading */
    s_batt_sample_num = 0;
    s_last_batt_sample_ms = 0;
    if (suspended_ms >= REMOTE_BATT_REST_MS) s_resume_rested = true;
    /* CLOCK_MONOTONIC did not see the suspend, and the RF link may not have looked down to the
     * main loop: report the battery once the link is up (alarm wakes stay up until it went out) */
    remote_battery_request_report(&g_batt_report);
}

void trigger_manual_suspend_to_ram(void) {
    read_remote_battery();
    if (strcmp(g_battery_info.powersource, "usb") == 0) {
        show_notice("Unplug USB first!", 2500);
        render_screen();
        return;
    }
    printf("[*] Manual Suspend to RAM initiated from Settings\n");
    show_notice("Suspending system...", 1200);
    render_screen();
    usleep(400000);
    fb_set_blank(true);
    enter_kernel_suspend();
}

void sig_wake_handler(int sig) {
    (void)sig;
    g_wake_requested = 1;
}

void sig_deep_sleep_handler(int sig) {
    (void)sig;
    fb_set_blank(true);
    uint64_t now_ms = get_boot_ms();
    s_last_blank_time_ms = (now_ms > 2000000) ? (now_ms - 2000000) : 1;
    log_power_debug("[*] Forcing IMMEDIATE Deep Standby via SIGUSR2\n");
    clearpad_sleep();
    enter_kernel_suspend();
}

#define CHARGING_PAGE_MS 4000

void *power_manager(void *arg) {
    (void)arg;
    uint64_t last_standby_heartbeat_ms = 0;
    static char s_last_dock_psrc[16] = "";
    static uint64_t s_last_dock_wake_ms = 0;
    uint64_t last_flight_heartbeat_ms = get_boot_ms();

    while (g_running) {
        remote_wdt_ping();
        /* A record every 5 minutes awake (a resume counts the time asleep, so one follows it too):
         * the battery over time, and how long before a halt the remote was still running */
        uint64_t hb_now_ms = get_boot_ms();
        if (hb_now_ms - last_flight_heartbeat_ms >= 300000) {
            last_flight_heartbeat_ms = hb_now_ms;
            flightlog_mark("HEARTBEAT");
        }
        /* Check power source switch (battery <-> usb <-> cradle) */
        FILE *fpsrc = fopen("/sys/devices/platform/i2c_davinci.1/i2c-1/1-0048/tps6507x-juniper/powersource", "r");
        if (fpsrc) {
            char psrc[16] = {0};
            if (fscanf(fpsrc, "%15s", psrc) == 1) {
                if (s_last_dock_psrc[0] == '\0') {
                    strncpy(s_last_dock_psrc, psrc, sizeof(s_last_dock_psrc) - 1);
                }
                if (strcmp(s_last_dock_psrc, psrc) != 0) {
                    uint64_t now_ms = get_monotonic_ms();
                    if (now_ms - s_last_dock_wake_ms > 1500) {
                        s_last_dock_wake_ms = now_ms;
                        log_power_debug("[*] Power source transition (%s -> %s), waking display\n",
                                        s_last_dock_psrc, psrc);
                        flightlog_event("POWER", "source=%s->%s", s_last_dock_psrc, psrc);
                        if (strcmp(s_last_dock_psrc, "battery") == 0)
                            g_charging_page_until_ms = (uint32_t)(now_ms + CHARGING_PAGE_MS) | 1; /* 0 means off */
                        g_screen_dirty = true; /* the header's battery changes even when the screen is on */
                        strncpy(s_last_dock_psrc, psrc, sizeof(s_last_dock_psrc) - 1);
                        s_last_batt_sample_ms = 0;
                        read_remote_battery();
                        g_wake_requested = 1;
                    } else {
                        strncpy(s_last_dock_psrc, psrc, sizeof(s_last_dock_psrc) - 1);
                    }
                }
            }
            fclose(fpsrc);
        }

        if (!g_screen_on) {
            bool is_docked = is_remote_docked();
            uint64_t now_ms = get_monotonic_ms();  /* holds */
            uint64_t boot_ms = get_boot_ms();      /* time since the blank, suspends included */
            uint64_t standby_s = (s_last_blank_time_ms > 0) ? (boot_ms - s_last_blank_time_ms) / 1000 : 0;

            if (is_docked) {
                usleep(300000);
            } else {
                read_remote_battery();
                if (g_is_syncing_config || g_is_ota_updating) {
                    usleep(500000); /* a transfer that started with the screen off: suspend after it */
                } else if (now_ms < g_suspend_hold_until_ms) {
                    usleep(200000); /* awake after an alarm until the battery report went out */
                } else if (remote_deep_standby_due(standby_s)) {
                    if (remote_should_suspend(standby_s, now_ms)) {
                        log_power_debug("[*] Standby time %llus reached deep standby (%s), suspending to RAM...\n",
                                        (unsigned long long)standby_s, g_deep_standby_min < 0 ? "at once" : "timeout");
                        clearpad_sleep();
                        enter_kernel_suspend();
                        usleep(100000);
                    } else {
                        usleep(500000);
                    }
                } else {
                    /* Light Standby: stay in Linux userspace, CPU idle in WFI, touch/gyro active */
                    usleep(200000);
                }
            }

            if (boot_ms - last_standby_heartbeat_ms >= 300000) {
                last_standby_heartbeat_ms = boot_ms;
                read_remote_battery();
                char cstat[96];
                get_cpu_freq_status(cstat, sizeof(cstat));
                uint64_t elapsed_s = s_last_blank_time_ms > 0 ? (boot_ms - s_last_blank_time_ms) / 1000 : 0;
                log_power_debug("[HEARTBEAT in STANDBY] Slept %llus | Batt: %dmV, %d%%, %s (chg=%d) | CPU: %s\n",
                                (unsigned long long)elapsed_s,
                                g_battery_info.voltage_mv, g_battery_info.percent,
                                g_battery_info.powersource, g_battery_info.charging, cstat);
            }
        } else {
            usleep(200000);
            last_standby_heartbeat_ms = 0;
        }
        if (g_wake_requested) {
            g_wake_requested = 0;
            reset_idle_timer();
        }
        if (g_timeout_seconds > 0 && g_screen_on) {
            time_t now = time(NULL);
            if (now - g_last_activity_time > g_timeout_seconds) {
                /* Blanking suspends a battery-powered remote, which would kill the transfer */
                if (g_is_ota_updating || g_is_syncing_config) {
                    reset_idle_timer();
                } else {
                    printf("[*] Idle timeout reached (%ds), blanking display\n", g_timeout_seconds);
                    fb_set_blank(true);
                    flush_activity_usage();
                }
            }
        }
    }
    return NULL;
}

void reset_idle_timer(void) {
    g_last_activity_time = time(NULL);
    if (!g_screen_on) {
        fb_set_blank(false);
        rf_wake_and_reconnect();
        render_screen();
    }
}

void init_gyro_interrupt(void) {
    int fdr = open(GYRO_DATA_RATE_SYS, O_WRONLY);
    if (fdr >= 0) {
        char rstr[16];
        snprintf(rstr, sizeof(rstr), "%d\n", g_gyro_rate_hz > 0 ? g_gyro_rate_hz : 50);
        write(fdr, rstr, strlen(rstr));
        close(fdr);
    }
    int fdt = open(GYRO_INT1_THRESHOLD_SYS, O_WRONLY);
    if (fdt >= 0) {
        write(fdt, "5\n", 2);
        close(fdt);
    }
    int fdd = open(GYRO_INT1_DURATION_SYS, O_WRONLY);
    if (fdd >= 0) {
        write(fdd, "1\n", 2);
        close(fdd);
    }
    printf("[+] Accelerometer INT1 hardware interrupt configured (rate=%dHz, thresh=5, dur=1)\n",
           g_gyro_rate_hz > 0 ? g_gyro_rate_hz : 50);
}

static int s_wdt_fd = -1;
static uint64_t s_last_wdt_ping_ms = 0;

#define REMOTE_WDT_TIMEOUT_S 60

void remote_wdt_init(void) {
    if (s_wdt_fd < 0) {
        /* O_CLOEXEC: dropbear and the other helpers started with system() must not inherit the
         * watchdog. A child holding it keeps the device open after remote_ui stops, so nothing
         * else can open it to feed it and the board resets when the timeout runs out. */
        s_wdt_fd = open("/dev/watchdog", O_WRONLY | O_CLOEXEC);
        if (s_wdt_fd >= 0) {
            /* The remote's DaVinci driver resets after 10 s without a write and ignores
             * WDIOC_SETTIMEOUT; remote_wdt_ping writes every 5 s. Ask for more anyway and log the
             * timeout the driver reports. */
            int timeout = REMOTE_WDT_TIMEOUT_S;
            if (ioctl(s_wdt_fd, WDIOC_SETTIMEOUT, &timeout) != 0) {
                timeout = 0;
                if (ioctl(s_wdt_fd, WDIOC_GETTIMEOUT, &timeout) != 0) timeout = 0;
            }
            if (timeout > 0) {
                printf("[+] Hardware watchdog (/dev/watchdog) armed with %ds timeout\n", timeout);
            } else {
                printf("[+] Hardware watchdog (/dev/watchdog) armed (driver default timeout)\n");
            }
            remote_wdt_ping();
        } else {
            printf("[!] Hardware watchdog not armed: open(/dev/watchdog) failed: %s\n", strerror(errno));
        }
    }
}

void remote_wdt_ping(void) {
    if (s_wdt_fd >= 0) {
        uint64_t now_ms = get_monotonic_ms();
        if (s_last_wdt_ping_ms == 0 || (now_ms - s_last_wdt_ping_ms >= 5000)) {
            write(s_wdt_fd, "\0", 1);
            s_last_wdt_ping_ms = now_ms;
        }
    }
}

void remote_wdt_close(void) {
    if (s_wdt_fd >= 0) {
        close(s_wdt_fd);
        s_wdt_fd = -1;
        s_last_wdt_ping_ms = 0;
    }
}

