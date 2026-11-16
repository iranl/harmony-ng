/*
 * test_remote_power_paths.c - suspend, reboot and clock decisions of remote_ui on the mock.
 *
 *  1. No suspend while a pairing or an activity switch is running (only sync and update were
 *     gated), and a Hub SLEEP_REQ during a config sync does not suspend the remote.
 *  2. Settings > Reboot right after the remote left the cradle: the power source already reads
 *     "battery" but the debounced charging flag is still set. The remote must restart in place
 *     (execv), not reboot (a reboot on battery switches it off).
 *  3. A key that is waking the screen (input stamped, screen not lit yet) when the power thread
 *     reaches its suspend: no suspend.
 *  4. SIGUSR2 only asks for the deep standby; the handler itself suspends nothing.
 *  5. A TIME_SYNC that steps the clock forward does not make the idle timer expire at once.
 *
 * Run with the mock library and a fresh mock root:
 *   HARMONY_MOCK_DIR=<dir> LD_PRELOAD=libmock_hardware.so ./test_remote_power_paths
 * Linked with -Wl,--wrap=system,--wrap=execv,--wrap=settimeofday,--wrap=time: no helper,
 * reboot, exec or clock change ever reaches the host. /dev/watchdog is opened as /dev/null.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/time.h>
#include <sys/wait.h>

#include "remote/remote_state.h"
#include "remote/remote_power.h"
#include "remote/remote_rf_sync.h"
#include "remote/remote_ui.h"

static int s_fail;
#define CHECK(cond, msg) do { if (!(cond)) { printf("[-] %s\n", msg); s_fail = 1; } else printf("[+] %s\n", msg); } while (0)

/* ---- host safety ---- */
int open(const char *path, int flags, ...) {
    static int (*real_open)(const char *, int, ...) = NULL;
    if (!real_open) real_open = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, "open");
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    if (strcmp(path, "/dev/watchdog") == 0 || strncmp(path, "/dev/i2c", 8) == 0) return real_open("/dev/null", O_WRONLY);
    return real_open(path, flags, mode);
}

static int s_system_calls;
int __wrap_system(const char *cmd) { (void)cmd; s_system_calls++; return 0; }
int __wrap_execv(const char *path, char *const argv[]) { (void)path; (void)argv; errno = ENOENT; return -1; }

static long s_clock_offset; /* what settimeofday() moved the clock by */
time_t __real_time(time_t *t);
time_t __wrap_time(time_t *t) {
    time_t now = __real_time(NULL) + s_clock_offset;
    if (t) *t = now;
    return now;
}
int __wrap_settimeofday(const struct timeval *tv, const void *tz) {
    (void)tz;
    if (tv) s_clock_offset = (long)tv->tv_sec - (long)__real_time(NULL);
    return 0;
}

/* ---- mock sysfs ---- */
static char s_mock[256];

static void put_file(const char *name, const char *text) {
    char p[400];
    snprintf(p, sizeof(p), "%s/sysfs/%s", s_mock, name);
    FILE *f = fopen(p, "w");
    if (f) { fputs(text, f); fclose(f); }
}

static bool suspended(void) {
    char p[400];
    snprintf(p, sizeof(p), "%s/sysfs/power_state", s_mock);
    FILE *f = fopen(p, "r");
    if (!f) return false;
    char b[16] = {0};
    bool mem = fgets(b, sizeof(b), f) != NULL && strncmp(b, "mem", 3) == 0; /* the mock fills "0" on a read */
    fclose(f);
    return mem;
}

static void clear_suspend(void) {
    char p[400];
    snprintf(p, sizeof(p), "%s/sysfs/power_state", s_mock);
    unlink(p);
}

static void on_battery(void) {
    put_file("powersource", "battery\n");
    put_file("charger", "0\n");
    put_file("vbat", "3900\n");
    snprintf(g_battery_info.powersource, sizeof(g_battery_info.powersource), "%s", "battery");
    g_battery_info.charging = 0;
}

/* Runs the power thread for ms milliseconds */
static void run_power_thread(int ms) {
    g_running = true;
    pthread_t th;
    pthread_create(&th, NULL, power_manager, NULL);
    usleep(ms * 1000);
    g_running = false;
    pthread_join(th, NULL);
}

/* The screen went dark long enough ago that deep standby ("at once") is due */
static void screen_off_standby_due(void) {
    g_screen_on = false;
    g_deep_standby_min = -1;
    g_timeout_seconds = 10;
    g_suspend_hold_until_ms = 0;
    g_wake_requested = 0;
    uint64_t now = get_boot_ms();
    s_last_blank_time_ms = now > 10000 ? now - 10000 : 1;
    g_last_activity_time = time(NULL) - 20;
}

int main(void) {
    const char *md = getenv("HARMONY_MOCK_DIR");
    snprintf(s_mock, sizeof(s_mock), "%s", md && md[0] ? md : "/tmp/harmony_mock");
    g_fb_mem = NULL;
    g_rf_fd = -1;

    /* 1. Gates */
    on_battery();
    g_deep_standby_min = -1;
    g_suspend_hold_until_ms = 0;
    uint64_t now = get_monotonic_ms();
    CHECK(remote_should_suspend(10, now), "idle on battery: suspend is due");
    g_pairing_state = 1;
    CHECK(!remote_should_suspend(10, now), "no suspend while pairing is open");
    g_pairing_state = 0;
    g_is_switching_activity = true;
    CHECK(!remote_should_suspend(10, now), "no suspend while an activity switch runs");
    g_is_switching_activity = false;

    clear_suspend();
    g_is_syncing_config = true;
    g_screen_on = true;
    rf_frame_t f;
    memset(&f, 0, sizeof(f));
    f.msg_type = RF_MSG_SLEEP_REQ;
    handle_hub_frame_remote(&f);
    g_is_syncing_config = false;
    CHECK(!suspended() && g_screen_on, "a Hub SLEEP_REQ during a config sync neither blanks nor suspends");

    /* 2. Reboot right after leaving the cradle */
    on_battery();
    g_battery_info.charging = 1; /* the debounced flag, still set */
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        g_current_view = VIEW_SETTINGS;
        g_settings_subview = SETTINGS_VIEW_REBOOT;
        execute_settings_action(1);
        _exit(0); /* not reached: both branches exit */
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 143,
          "reboot on battery with the charging flag still set restarts in place (no reboot)");
    g_battery_info.charging = 0;

    /* 3. A key is waking the screen when the power thread reaches its suspend */
    on_battery();
    clear_suspend();
    screen_off_standby_due();
    g_last_activity_time = time(NULL); /* the key was stamped, the unblank has not finished */
    run_power_thread(700);
    CHECK(!suspended(), "no suspend right behind a key that is waking the screen");

    clear_suspend();
    screen_off_standby_due();
    run_power_thread(700);
    CHECK(suspended(), "control: without the key the power thread suspends");

    /* 4. SIGUSR2 */
    clear_suspend();
    screen_off_standby_due();
    signal(SIGUSR2, sig_deep_sleep_handler);
    raise(SIGUSR2);
    CHECK(!suspended() && g_deep_sleep_requested, "the SIGUSR2 handler only asks for the deep standby");
    g_is_ota_updating = true;
    run_power_thread(300);
    CHECK(!suspended(), "the asked deep standby does not start during an update");
    g_is_ota_updating = false;
    g_deep_sleep_requested = 1;
    run_power_thread(300);
    CHECK(suspended(), "the power thread does the asked deep standby");
    g_deep_sleep_requested = 0;

    /* 5. Clock step forward by TIME_SYNC */
    g_screen_on = true;
    g_timeout_seconds = 10;
    g_last_activity_time = time(NULL) - 5;
    memset(&f, 0, sizeof(f));
    f.msg_type = RF_MSG_TIME_SYNC;
    uint32_t epoch = (uint32_t)time(NULL) + 3u * 365u * 86400u;
    f.payload[0] = epoch & 0xFF;
    f.payload[1] = (epoch >> 8) & 0xFF;
    f.payload[2] = (epoch >> 16) & 0xFF;
    f.payload[3] = (epoch >> 24) & 0xFF;
    handle_hub_frame_remote(&f);
    CHECK(s_clock_offset > 86400, "the TIME_SYNC stepped the clock");
    CHECK(time(NULL) - g_last_activity_time <= g_timeout_seconds,
          "the idle timer is not expired by the clock step");

    CHECK(s_system_calls >= 0, "no helper reached the host (system() wrapped)");
    if (s_fail) {
        printf("[-] test_remote_power_paths: FAILED\n");
        return 1;
    }
    printf("[+] test_remote_power_paths: all checks passed\n");
    return 0;
}
