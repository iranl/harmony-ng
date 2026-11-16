/*
 * test_remote_settings_files.c - the remote's settings, Hub config and usage files.
 *
 *  1. A write that fails part way (here: a file size limit, RLIMIT_FSIZE, as a stand-in for a
 *     reset or a full flash in the middle of the write) must leave the previous file whole.
 *     The files used to be truncated and rewritten in place.
 *  2. Settings read from the file are range checked like the Settings screens set them: key
 *     light mode 0-2, night hours 0-23, accelerometer rate 10/25/50/100 Hz, log level 0-3.
 *     A settings push from the Hub with an accelerometer rate the remote does not offer is
 *     ignored.
 *
 * Run with the mock library and a fresh mock root (the /data files live there):
 *   HARMONY_MOCK_DIR=<dir> LD_PRELOAD=libmock_hardware.so ./test_remote_settings_files
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>

#include "remote/remote_state.h"
#include "remote/remote_config.h"
#include "remote/remote_rf_sync.h"

static int s_fail;
#define CHECK(cond, msg) do { if (!(cond)) { printf("[-] %s\n", msg); s_fail = 1; } else printf("[+] %s\n", msg); } while (0)

static char s_mock[256];

static size_t read_mock(const char *data_name, char *buf, size_t size) {
    char p[400];
    snprintf(p, sizeof(p), "%s/data/%s", s_mock, data_name);
    FILE *f = fopen(p, "r");
    if (!f) { buf[0] = '\0'; return 0; }
    size_t n = fread(buf, 1, size - 1, f);
    buf[n] = '\0';
    fclose(f);
    return n;
}

static void write_mock(const char *data_name, const char *text) {
    char p[400];
    snprintf(p, sizeof(p), "%s/data/%s", s_mock, data_name);
    FILE *f = fopen(p, "w");
    if (f) { fputs(text, f); fclose(f); }
}

/* Writes larger than the limit fail with EFBIG, as a write cut off by a reset would end */
static void limit_file_size(rlim_t bytes) {
    struct rlimit rl = { bytes, bytes };
    signal(SIGXFSZ, SIG_IGN);
    setrlimit(RLIMIT_FSIZE, &rl);
}

static void unlimit_file_size(void) {
    struct rlimit rl;
    getrlimit(RLIMIT_FSIZE, &rl);
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_FSIZE, &rl);
}

int main(void) {
    const char *md = getenv("HARMONY_MOCK_DIR");
    snprintf(s_mock, sizeof(s_mock), "%s", md && md[0] ? md : "/tmp/harmony_mock");
    g_rf_link_connected = false;
    g_rf_fd = -1;

    /* 1a. Settings file */
    g_brightness_val = 3;
    save_settings_conf();
    char before[4096], after[4096];
    size_t n_before = read_mock("remote_settings.conf", before, sizeof(before));
    CHECK(n_before > 200 && strstr(before, "brightness=3"), "settings saved");
    g_brightness_val = 4;
    /* hard limit can only go down: the rest of the test runs in a child */
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        limit_file_size(64);
        save_settings_conf();
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    read_mock("remote_settings.conf", after, sizeof(after));
    CHECK(strcmp(before, after) == 0, "a settings write cut short leaves the previous file whole");

    /* 1b. Hub config file */
    g_rf_device_count = 1;
    snprintf(g_rf_devices[0].id, sizeof(g_rf_devices[0].id), "%s", "3");
    snprintf(g_rf_devices[0].name, sizeof(g_rf_devices[0].name), "%s", "TV");
    g_config_version = 5;
    save_rf_config();
    n_before = read_mock("remote_rf_config.json", before, sizeof(before));
    CHECK(n_before > 20, "Hub config saved");
    g_config_version = 6;
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        limit_file_size(16);
        save_rf_config();
        _exit(0);
    }
    waitpid(pid, &st, 0);
    read_mock("remote_rf_config.json", after, sizeof(after));
    CHECK(strcmp(before, after) == 0, "a Hub config write cut short leaves the previous file whole");

    /* 1c. Usage counts */
    record_activity_start("10");
    record_activity_start("11");
    flush_activity_usage();
    n_before = read_mock("remote_activity_usage.conf", before, sizeof(before));
    CHECK(strstr(before, "10=1") && strstr(before, "11=1"), "usage counts saved");
    record_activity_start("10");
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        limit_file_size(4);
        flush_activity_usage();
        _exit(0);
    }
    waitpid(pid, &st, 0);
    read_mock("remote_activity_usage.conf", after, sizeof(after));
    CHECK(strcmp(before, after) == 0, "a usage write cut short leaves the previous file whole");
    unlimit_file_size();

    /* 2. Range checks */
    write_mock("remote_settings.conf",
               "btn_backlight=3\nnight_start=30\nnight_end=-1\ngyro_rate=7\nlog_level=9\n");
    g_btn_backlight_mode = 1;
    load_settings_conf();
    CHECK(g_btn_backlight_mode >= 0 && g_btn_backlight_mode <= 2, "key light mode out of range is not taken");
    CHECK(g_night_start_hour >= 0 && g_night_start_hour <= 23 && g_night_end_hour >= 0 && g_night_end_hour <= 23,
          "night hours out of range are not taken");
    CHECK(g_gyro_rate_hz == 10 || g_gyro_rate_hz == 25 || g_gyro_rate_hz == 50 || g_gyro_rate_hz == 100,
          "an accelerometer rate the remote does not offer is not taken from the file");
    CHECK(g_log_level >= LOG_LEVEL_ERR && g_log_level <= LOG_LEVEL_DEBUG, "log level out of range is not taken");

    g_gyro_rate_hz = 25;
    rf_frame_t f;
    memset(&f, 0, sizeof(f));
    f.msg_type = RF_MSG_SETTINGS_PUSH;
    f.payload[0] = 3;
    f.payload[3] = 0;
    f.payload[4] = 30;
    f.payload[11] = 7;
    g_no_screen_mode = false;
    handle_hub_frame_remote(&f);
    CHECK(g_gyro_rate_hz == 25, "a settings push with an accelerometer rate the remote does not offer keeps the rate");
    f.payload[11] = 100;
    handle_hub_frame_remote(&f);
    CHECK(g_gyro_rate_hz == 100, "a settings push with an offered rate is applied");

    if (s_fail) {
        printf("[-] test_remote_settings_files: FAILED\n");
        return 1;
    }
    printf("[+] test_remote_settings_files: all checks passed\n");
    return 0;
}
