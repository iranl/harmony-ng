/* Flight recorder of the remote (payload/source/remote/remote_flightlog.c): records that are on
 * disk when the call returns, the report of a suspend that never returned, rotation, and the
 * kernel log filter. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "remote_flightlog.h"

static int g_failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { g_failures++; printf("  FAIL %s (line %d)\n", msg, __LINE__); } } while (0)

static char g_dir[128], g_file[192], g_old[200], g_enable[192];

/* Set to make the context callback behave like a crash inside the recorder: it runs with the
 * recorder's lock held, as a crash handler on the same thread would */
static int g_crash_in_context = 0;

static void context(char *buf, size_t buf_sz) {
    if (g_crash_in_context) {
        g_crash_in_context = 0;
        flightlog_crash(11);
    }
    snprintf(buf, buf_sz, "batt=3980mV 87%% battery");
}

static char *slurp(const char *path) {
    static char buf[65536];
    buf[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return buf;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static int count(const char *hay, const char *needle) {
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += strlen(needle)) n++;
    return n;
}

static void enable(int on) {
    if (on) {
        FILE *f = fopen(g_enable, "w");
        if (f) fclose(f);
    } else {
        unlink(g_enable);
    }
}

int main(void) {
    snprintf(g_dir, sizeof(g_dir), "/tmp/flightlog_test_%d", (int)getpid());
    mkdir(g_dir, 0755);
    snprintf(g_file, sizeof(g_file), "%s/flight.txt", g_dir);
    snprintf(g_old, sizeof(g_old), "%s.1", g_file);
    snprintf(g_enable, sizeof(g_enable), "%s/on", g_dir);
    flightlog_set_paths(g_file, g_enable, 4096);
    flightlog_set_context(context);

    /* Off: nothing is written, nothing is created */
    flightlog_init("9.9.9");
    flightlog_mark("SCREEN_OFF");
    flightlog_crash(11);
    CHECK(access(g_file, F_OK) != 0, "disabled: no file");
    CHECK(!flightlog_enabled(), "disabled without the enable file");

    /* On: the first start has no previous record */
    enable(1);
    CHECK(flightlog_enabled(), "enabled by the enable file");
    flightlog_init("9.9.9");
    char *txt = slurp(g_file);
    CHECK(strstr(txt, " BOOT version=9.9.9 ") != NULL, "BOOT record with the version");
    CHECK(strstr(txt, "prev=none") != NULL, "first start: no previous record");
    CHECK(strstr(txt, "SUSPEND_DID_NOT_RETURN") == NULL, "first start: no unfinished suspend");
    CHECK(strstr(txt, " | batt=3980mV 87% battery\n") != NULL, "the context ends the record");
    CHECK(strstr(txt, " up=") != NULL && strstr(txt, " n=1 ") != NULL, "uptime and record counter");

    /* A suspend that returns */
    flightlog_event(FLIGHTLOG_EV_SUSPEND_ENTER, "state=%s", "mem");
    char ev[40] = "", st[40] = "";
    CHECK(flightlog_last_event(g_file, ev, sizeof(ev), st, sizeof(st)) && strcmp(ev, "SUSPEND_ENTER") == 0,
          "the announcement is the last record while asleep");
    CHECK(strlen(st) >= 19 && st[4] == '-' && st[10] == 'T', "the time stamp of the last record");
    flightlog_event(FLIGHTLOG_EV_SUSPEND_RETURN, "rc=ok slept_ms=%d wakeup_event=%s", 3600000, "alarm");
    flightlog_init("9.9.9");
    txt = slurp(g_file);
    CHECK(strstr(txt, "prev=SUSPEND_RETURN") != NULL, "restart after a finished suspend");
    CHECK(strstr(txt, "SUSPEND_DID_NOT_RETURN") == NULL, "a finished suspend is not reported");

    /* A suspend that never returns: the battery is pulled, the next thing is a start */
    flightlog_event(FLIGHTLOG_EV_SUSPEND_ENTER, "state=%s", "mem");
    flightlog_init("9.9.9");
    txt = slurp(g_file);
    CHECK(count(txt, "SUSPEND_DID_NOT_RETURN") == 1, "the unfinished suspend is reported once");
    CHECK(strstr(txt, "prev=SUSPEND_ENTER prev_t=2") != NULL, "with the time it was entered");

    /* Kernel lines after a record do not hide it, and a line break in a record stays one line */
    FILE *f = fopen(g_file, "a");
    if (f) { fputs("  PM: Syncing filesystems ... done.\n", f); fclose(f); }
    CHECK(flightlog_last_event(g_file, ev, sizeof(ev), NULL, 0) && strcmp(ev, "BOOT") == 0,
          "indented kernel lines are not records");
    flightlog_event("NOTE", "two\nlines");
    txt = slurp(g_file);
    CHECK(strstr(txt, " NOTE two lines | ") != NULL, "a record is always one line");

    /* Rotation: the file never grows past the limit, the previous one is kept once */
    for (int i = 0; i < 200; i++) flightlog_event("HEARTBEAT", "i=%d", i);
    struct stat sb;
    CHECK(stat(g_file, &sb) == 0 && sb.st_size <= 4096, "the file stays under the limit");
    CHECK(stat(g_old, &sb) == 0 && sb.st_size <= 4096, "the previous file is kept as .1");
    CHECK(strstr(slurp(g_file), "HEARTBEAT i=199") != NULL, "the newest record is in the current file");

    /* A record without details */
    flightlog_mark("SCREEN_OFF");
    CHECK(strstr(slurp(g_file), " SCREEN_OFF | batt=3980mV") != NULL, "a mark is the event and the context");

    /* The crash record: written with the raw time, and found by the next start */
    flightlog_crash(11);
    txt = slurp(g_file);
    CHECK(strstr(txt, " CRASH signal=11 | batt=3980mV") != NULL, "the crash record");
    CHECK(strstr(txt, "\nepoch=") != NULL, "the crash record's time is not converted");
    CHECK(flightlog_last_event(g_file, ev, sizeof(ev), st, sizeof(st)) && strcmp(ev, "CRASH") == 0 &&
          strncmp(st, "epoch=", 6) == 0, "a start after a crash finds it as the last record");

    /* A crash while the recorder holds its lock on the same thread: the handler must come back
     * (it has _exit() to reach) and the record is skipped. With a blocking lock this never
     * returns; the alarm turns that into a failure instead of a hung test. */
    int crashes_before = count(slurp(g_file), " CRASH ");
    g_crash_in_context = 1;
    alarm(10);
    flightlog_event("NOTE", "interrupted=%d", 1);
    alarm(0);
    txt = slurp(g_file);
    CHECK(strstr(txt, " NOTE interrupted=1 | ") != NULL, "the interrupted record is still written");
    CHECK(count(txt, " CRASH ") == crashes_before, "no crash record while the lock is held, and no deadlock");

    /* Turned off while running */
    enable(0);
    flightlog_mark("AFTER_OFF");
    CHECK(strstr(slurp(g_file), "AFTER_OFF") == NULL, "no record once the enable file is gone");

    /* Kernel log filter: the board's pin dump goes, the rest stays, long input is cut */
    const char *klog =
        "<6>PM: Syncing filesystems ... done.\n"
        "<4>Freezing user space processes ... (elapsed 0.01 seconds) done.\n"
        "PINMUX00\t= 00000000\n"
        "<4>PINMUX19\t= 00000000\n"
        "DIR01\t\t= ffffffff\n"
        "OUT_DATA23\t= 00000000\n"
        "<6>[  123.456789] IN_DATA8\t= 00000000\n"
        "<3>i2c_davinci i2c_davinci.1: controller timed out\n"
        "\n"
        "<3>msp430_i2c_read_status error";
    char out[1024];
    size_t n = flightlog_filter_klog(klog, out, sizeof(out), 40);
    CHECK(n == strlen(out) && n > 0, "the filter returns what it wrote");
    CHECK(strstr(out, "PINMUX") == NULL && strstr(out, "DIR01") == NULL && strstr(out, "OUT_DATA") == NULL &&
          strstr(out, "IN_DATA") == NULL, "the pin dump is dropped");
    CHECK(strstr(out, "  PM: Syncing filesystems ... done.\n") != NULL, "kernel lines are indented, level removed");
    CHECK(strstr(out, "  i2c_davinci i2c_davinci.1: controller timed out\n") != NULL, "an I2C error is kept");
    CHECK(strstr(out, "  msp430_i2c_read_status error\n") != NULL, "the last line without a newline is kept");
    CHECK(count(out, "\n") == 4, "four lines kept");
    n = flightlog_filter_klog(klog, out, sizeof(out), 2);
    CHECK(count(out, "\n") == 3 && strncmp(out, "  (2 earlier kernel lines left out)\n", 36) == 0,
          "over the limit the oldest lines go, and the note says how many");
    CHECK(strstr(out, "controller timed out") != NULL && strstr(out, "msp430_i2c_read_status error") != NULL &&
          strstr(out, "Syncing") == NULL && strstr(out, "Freezing") == NULL, "the newest lines are the ones kept");

    unlink(g_file);
    unlink(g_old);
    unlink(g_enable);
    rmdir(g_dir);

    if (g_failures) { printf("[-] %d flight recorder checks failed\n", g_failures); return 1; }
    printf("[+] All flight recorder checks passed\n");
    return 0;
}
