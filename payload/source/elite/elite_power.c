#include "elite_power.h"
#include "elite_fb.h"
#include "elite_rf.h"
#include "elite_config.h"
#include "elite_ui.h"

volatile sig_atomic_t g_wake_requested = 0;
char g_timezone_str[48] = "CET-1CEST,M3.5.0,M10.5.0/3";

typedef struct {
    const char *prefix;
    const char *full_tz;
} tz_mapping_t;

static const tz_mapping_t g_tz_mappings[] = {
    { "CET-1CEST",      "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "GMT0BST",        "GMT0BST,M3.5.0/1,M10.5.0" },
    { "WET0WEST",       "WET0WEST,M3.5.0/1,M10.5.0" },
    { "EET-2EEST",      "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "EST5EDT",        "EST5EDT,M3.2.0,M11.1.0" },
    { "CST6CDT",        "CST6CDT,M3.2.0,M11.1.0" },
    { "MST7MDT",        "MST7MDT,M3.2.0,M11.1.0" },
    { "PST8PDT",        "PST8PDT,M3.2.0,M11.1.0" },
    { "AKST9AKDT",      "AKST9AKDT,M3.2.0,M11.1.0" },
    { "NST3:30NDT",     "NST3:30NDT,M3.2.0,M11.1.0" },
    { "AST4ADT",        "AST4ADT,M3.2.0,M11.1.0" },
    { "AEST-10AEDT",    "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "ACST-9:30ACDT",  "ACST-9:30ACDT,M10.1.0,M4.1.0/3" },
    { "NZST-12NZDT",    "NZST-12NZDT,M9.5.0,M4.1.0/3" },
};

void expand_timezone(const char *in, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    if (!in || !in[0]) {
        snprintf(out, out_sz, "CET-1CEST,M3.5.0,M10.5.0/3");
        return;
    }
    const char *c1 = strchr(in, ',');
    if (c1 && strchr(c1 + 1, ',')) {
        strncpy(out, in, out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }
    for (size_t i = 0; i < sizeof(g_tz_mappings) / sizeof(g_tz_mappings[0]); i++) {
        if (strncmp(in, g_tz_mappings[i].prefix, strlen(g_tz_mappings[i].prefix)) == 0) {
            strncpy(out, g_tz_mappings[i].full_tz, out_sz - 1);
            out[out_sz - 1] = '\0';
            return;
        }
    }
    if (!strchr(in, ',')) {
        strncpy(out, in, out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }
    snprintf(out, out_sz, "CET-1CEST,M3.5.0,M10.5.0/3");
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
        char tz[48] = {0};
        if (fgets(tz, sizeof(tz), ftz)) {
            char *nl = strchr(tz, '\n'); if (nl) *nl = '\0';
            char *cr = strchr(tz, '\r'); if (cr) *cr = '\0';
            if (tz[0] && strstr(tz, "EST5EDT") == NULL) {
                expand_timezone(tz, g_timezone_str, sizeof(g_timezone_str));
            }
        }
        fclose(ftz);
    }
    expand_timezone(g_timezone_str, g_timezone_str, sizeof(g_timezone_str));
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

void clearpad_hw_init(void) {
    int fd = open("/dev/i2c-1", O_RDWR);
    if (fd < 0) return;
    if (ioctl(fd, I2C_SLAVE_FORCE, 0x20) < 0) {
        close(fd);
        return;
    }

    union i2c_smbus_data data;
    struct i2c_smbus_ioctl_data args;

    data.byte = 0x01;
    args.read_write = I2C_SMBUS_WRITE;
    args.command = 0x68;
    args.size = I2C_SMBUS_BYTE_DATA;
    args.data = &data;
    ioctl(fd, I2C_SMBUS, &args);

    usleep(150000);

    data.byte = 0x00;
    args.command = 0x35;
    ioctl(fd, I2C_SMBUS, &args);

    data.byte = 0x07;
    args.command = 0x36;
    ioctl(fd, I2C_SMBUS, &args);

    args.read_write = I2C_SMBUS_READ;
    args.command = 0x14;
    ioctl(fd, I2C_SMBUS, &args);

    close(fd);
    printf("[+] Synaptics Clearpad hardware reset & initialized via I2C\n");
}

void clearpad_wake(bool full_reset) {
    if (full_reset) {
        clearpad_hw_init();
    } else {
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

bool is_elite_docked(void) {
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

void set_cpu_freq_standby(bool standby) {
    if (standby && is_elite_docked()) {
        log_power_debug("CPU scale -> STANDBY bypassed (docked/charging: holding 456MHz)\n");
        return;
    }
    char before[96];
    get_cpu_freq_status(before, sizeof(before));
    if (standby) {
        bool gov_ok = false;
        FILE *fg = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "w");
        if (fg) {
            if (fputs("userspace\n", fg) >= 0) gov_ok = true;
            fclose(fg);
        }
        bool speed_ok = false;
        if (gov_ok) {
            FILE *fs = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed", "w");
            if (fs) {
                if (fputs("96000\n", fs) >= 0) speed_ok = true;
                fclose(fs);
            }
        }
        if (!gov_ok || !speed_ok) {
            FILE *fp = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "w");
            if (fp) {
                fputs("powersave\n", fp);
                fclose(fp);
            }
        }
        char after[96];
        get_cpu_freq_status(after, sizeof(after));
        log_power_debug("CPU scale -> STANDBY: before [%s] -> after [%s] (gov_ok=%d, speed_ok=%d)\n",
                        before, after, gov_ok, speed_ok);
    } else {
        bool gov_ok = false;
        FILE *fg = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "w");
        if (fg) {
            if (fputs("performance\n", fg) >= 0) gov_ok = true;
            fclose(fg);
        }
        FILE *fs = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed", "w");
        if (fs) {
            fputs("456000\n", fs);
            fclose(fs);
        }
        if (!gov_ok) {
            FILE *fgu = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "w");
            if (fgu) {
                fputs("userspace\n", fgu);
                fclose(fgu);
            }
        }
        char after[96];
        get_cpu_freq_status(after, sizeof(after));
        log_power_debug("CPU scale -> ACTIVE: before [%s] -> after [%s] (gov_ok=%d)\n",
                        before, after, gov_ok);
    }
}

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
    uint64_t now_ms = get_monotonic_ms();

    if (g_screen_on) {
        s_last_wake_ms = now_ms;
        uint64_t sleep_sec = s_last_blank_time_ms > 0 ? (now_ms - s_last_blank_time_ms) / 1000 : 0;
        set_cpu_freq_standby(false);
        set_backlight_level(g_brightness_val);
        clearpad_wake(sleep_sec >= 10);
        read_elite_battery();
        rf_wake_and_reconnect();
        g_screen_dirty = true;
        log_power_debug("=== WAKING FROM STANDBY (Screen Active) ===\n");
        log_power_debug("  Slept for: %llu seconds | Backlight: %d | Battery: %dmV, %d%% (%s, chg=%d)\n",
                        (unsigned long long)sleep_sec, g_brightness_val,
                        g_battery_info.voltage_mv, g_battery_info.percent,
                        g_battery_info.powersource, g_battery_info.charging);
    } else {
        s_last_blank_time_ms = now_ms;
        set_backlight_level(0);
        set_cpu_freq_standby(true);
        clearpad_sleep();
        if (g_gyro_fd >= 0) {
            struct input_event ev[16];
            while (read(g_gyro_fd, ev, sizeof(ev)) > 0) {}
        }
        read_elite_battery();
        bool usb_plug = (strcmp(g_battery_info.powersource, "usb") == 0);
        log_power_debug("=== ENTERING STANDBY (Screen Blank) ===\n");
        log_power_debug("  fb_blank=%d, bl=0 | USB Eth: cfg=%d, cable=%d | Battery: %dmV, %d%% (%s, chg=%d)\n",
                        blank_res, g_usbeth_enabled, usb_plug,
                        g_battery_info.voltage_mv, g_battery_info.percent,
                        g_battery_info.powersource, g_battery_info.charging);
    }
    int lfd = open(BTN_LED_SYSFS, O_WRONLY);
    if (lfd >= 0) {
        bool turn_on = !blank;
        if (turn_on) {
            if (g_btn_backlight_mode == 0) turn_on = false;
            else if (g_btn_backlight_mode == 1) turn_on = true;
            else if (g_btn_backlight_mode == 2) turn_on = !is_night_time();
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

void read_elite_battery(void) {
    uint64_t now_ms = get_monotonic_ms();

    if (s_last_batt_sample_ms > 0 && (now_ms - s_last_batt_sample_ms < 1000) && s_batt_sample_num > 0) {
        return;
    }
    s_last_batt_sample_ms = now_ms;

    FILE *fv = fopen("/sys/devices/platform/i2c_davinci.1/i2c-1/1-0048/tps6507x-juniper/vbat", "r");
    float v = 2.05f;
    uint16_t raw_mv = g_battery_info.voltage_mv;
    if (fv) {
        if (fscanf(fv, "%f", &v) == 1) {
            raw_mv = (uint16_t)(v * 2000.0f);
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
    g_battery_info.charging = is_charging;

    static uint8_t s_last_charging_flag = 0xFF;
    bool chg_transition = (s_last_charging_flag != 0xFF && s_last_charging_flag != is_charging);
    s_last_charging_flag = is_charging;

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

    int mv = g_battery_info.voltage_mv;
    int pct = 0;
    if (mv > 5000) {
        if (mv >= 8200) pct = 100;
        else if (mv <= 6800) pct = 0;
        else pct = (int)((mv - 6800) * 100 / (8200 - 6800));
    } else {
        if (mv >= 4150) pct = 100;
        else if (mv <= 3400) pct = 0;
        else pct = (int)((mv - 3400) * 100 / (4150 - 3400));
    }
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    if (s_filtered_pct == 0xFF || chg_transition) {
        s_filtered_pct = (uint8_t)pct;
    } else {
        if (g_battery_info.charging) {
            if (pct > s_filtered_pct) {
                s_filtered_pct = (uint8_t)pct;
            } else if (s_filtered_pct - pct > 4) {
                s_filtered_pct--;
            }
        } else {
            if (pct < s_filtered_pct) {
                s_filtered_pct = (uint8_t)pct;
            } else if (pct - s_filtered_pct > 4) {
                s_filtered_pct = (uint8_t)pct;
            }
        }
    }
    g_battery_info.percent = s_filtered_pct;
}

void stop_usbeth_and_dropbear(void) {
    log_power_debug("[*] Stopping USB Ethernet and Dropbear synchronously...\n");
    system("killall -9 dropbear dropbearmulti 2>/dev/null; /usr/bin/usbeth stop 2>/dev/null; ifconfig usb0 down 2>/dev/null; chmod +x /usr/bin/usbhid 2>/dev/null; true");
    log_power_debug("[+] USB Ethernet stopped and Dropbear killed.\n");
}

void start_usbeth_and_dropbear(void) {
    log_power_debug("[*] Starting USB Ethernet and Dropbear...\n");
    system("killall -9 usbhid 2>/dev/null; chmod -x /usr/bin/usbhid 2>/dev/null; /usr/bin/usbeth start 2>/dev/null; /data/dropbear -R -p 0.0.0.0:22 2>/dev/null || /data/dropbear -R -B 2>/dev/null || true &");
    log_power_debug("[+] USB Ethernet and Dropbear started.\n");
}

void toggle_usbeth(void) {
    g_usbeth_enabled = !g_usbeth_enabled;
    if (g_usbeth_enabled) {
        save_settings_conf();
        g_usb_connected = is_usb_hardware_connected();
        if (g_usb_connected) {
            show_notice("Starting USB Ethernet...", 1500);
            render_screen();
            start_usbeth_and_dropbear();
            show_notice("USB Ethernet: ACTIVE", 2000);
        } else {
            stop_usbeth_and_dropbear();
            show_notice("USB Eth ON (unplugged)", 2500);
        }
    } else {
        show_notice("Stopping USB Ethernet...", 1500);
        render_screen();
        stop_usbeth_and_dropbear();
        save_settings_conf();
        show_notice("USB Ethernet: OFF", 2000);
    }
}

void enter_kernel_suspend(void) {
    read_elite_battery();
    if (strcmp(g_battery_info.powersource, "battery") != 0) {
        return;
    }

    int mfd = open("/sys/bus/i2c/devices/1-0034/wakeup", O_WRONLY);
    if (mfd >= 0) {
        write(mfd, "3600\n", 5);
        close(mfd);
    }

    log_power_debug("[*] Entering kernel Suspend-to-RAM (echo mem > /sys/power/state)...\n");
    int fd = open("/sys/power/state", O_WRONLY);
    if (fd >= 0) {
        ssize_t w = write(fd, "mem\n", 4);
        close(fd);
        if (w < 0) {
            log_power_debug("[-] Kernel suspend returned error: %s\n", strerror(errno));
            usleep(5000000);
            return;
        }
    }
    log_power_debug("[*] Resumed from kernel Suspend-to-RAM!\n");

    FILE *fev = fopen("/sys/bus/i2c/devices/1-0034/wakeup_event", "r");
    if (fev) {
        char wev[32] = {0};
        if (fscanf(fev, "%31s", wev) == 1) {
            log_power_debug("[*] Kernel resume wakeup_event: %s\n", wev);
            if (strcmp(wev, "alarm") != 0) {
                g_wake_requested = 1;
            }
        }
        fclose(fev);
    } else {
        g_wake_requested = 1;
    }

    rf_wake_and_reconnect();
}

void trigger_manual_suspend_to_ram(void) {
    read_elite_battery();
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
    uint64_t now_ms = get_monotonic_ms();
    s_last_blank_time_ms = (now_ms > 2000000) ? (now_ms - 2000000) : 1;
    log_power_debug("[*] Forcing IMMEDIATE Deep Standby via SIGUSR2\n");
}

void *power_manager(void *arg) {
    (void)arg;
    uint64_t last_standby_heartbeat_ms = 0;

    while (g_running) {
        if (!g_screen_on) {
            bool is_docked = is_elite_docked();
            if (is_docked) {
                usleep(500000);
            } else {
                read_elite_battery();
                if (strcmp(g_battery_info.powersource, "battery") == 0) {
                    enter_kernel_suspend();
                    usleep(100000);
                } else {
                    usleep(10000000);
                }
            }

            FILE *fpsrc = fopen("/sys/devices/platform/i2c_davinci.1/i2c-1/1-0048/tps6507x-juniper/powersource", "r");
            if (fpsrc) {
                char psrc[16] = {0};
                if (fscanf(fpsrc, "%15s", psrc) == 1) {
                    bool was_charging = (strcmp(g_battery_info.powersource, "cradle") == 0 ||
                                         strcmp(g_battery_info.powersource, "usb") == 0 ||
                                         g_battery_info.charging);
                    bool now_charging = (strcmp(psrc, "cradle") == 0 || strcmp(psrc, "usb") == 0);

                    if (was_charging && !now_charging) {
                        log_power_debug("[*] Dock disconnected (%s -> %s), waking display\n",
                                        g_battery_info.powersource, psrc);
                        printf("[*] Dock disconnected (%s -> %s), waking display\n",
                               g_battery_info.powersource, psrc);
                        s_last_batt_sample_ms = 0;
                        read_elite_battery();
                        g_wake_requested = 1;
                    } else if (!was_charging && now_charging) {
                        log_power_debug("[*] Dock connected (%s -> %s), waking display\n",
                                        g_battery_info.powersource, psrc);
                        printf("[*] Dock connected (%s -> %s), waking display\n",
                               g_battery_info.powersource, psrc);
                        s_last_batt_sample_ms = 0;
                        read_elite_battery();
                        g_wake_requested = 1;
                    }
                }
                fclose(fpsrc);
            }

            uint64_t now_ms = get_monotonic_ms();
            if (now_ms - last_standby_heartbeat_ms >= 300000) {
                last_standby_heartbeat_ms = now_ms;
                read_elite_battery();
                char cstat[96];
                get_cpu_freq_status(cstat, sizeof(cstat));
                uint64_t elapsed_s = s_last_blank_time_ms > 0 ? (now_ms - s_last_blank_time_ms) / 1000 : 0;
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
                if (g_is_ota_updating) {
                    reset_idle_timer();
                } else {
                    printf("[*] Idle timeout reached (%ds), blanking display\n", g_timeout_seconds);
                    fb_set_blank(true);
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
        snprintf(rstr, sizeof(rstr), "%d\n", g_gyro_rate_hz > 0 ? g_gyro_rate_hz : 25);
        write(fdr, rstr, strlen(rstr));
        close(fdr);
    }
    int fdt = open(GYRO_INT1_THRESHOLD_SYS, O_WRONLY);
    if (fdt >= 0) {
        write(fdt, "3\n", 2);
        close(fdt);
    }
    int fdd = open(GYRO_INT1_DURATION_SYS, O_WRONLY);
    if (fdd >= 0) {
        write(fdd, "1\n", 2);
        close(fdd);
    }
    printf("[+] Accelerometer INT1 hardware interrupt configured (rate=%dHz, thresh=3, dur=1)\n",
           g_gyro_rate_hz > 0 ? g_gyro_rate_hz : 25);
}
