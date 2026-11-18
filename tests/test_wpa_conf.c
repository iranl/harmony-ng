/*
 * Wi-Fi values in wpa_supplicant.conf (wpa_conf.h, hub_network_util.c).
 *
 * wpa_supplicant reads a quoted value as it is (no escape sequences), so the values must be
 * written as typed: a backslash added before $ or " becomes part of the key. Checks:
 * - wpa_conf_value_error(): line breaks and control characters refused, SSID 1 to 32 bytes,
 *   pass phrase 8 to 63 characters or 64 hex digits;
 * - net_test_wifi_connection() writes ssid and psk unchanged, a 64 hex digit key unquoted,
 *   refuses a value the file cannot carry before touching anything, and runs one test at a
 *   time (a second test while one runs is refused);
 * - net_get_current_wifi_psk() reads a quoted pass phrase up to the last quote of the line.
 *
 * The wpa_cli calls (popen, system), the file writes and fopen of /etc are replaced below;
 * nothing on the host is touched.
 */
#define _GNU_SOURCE
#include "hub_network_util.h"
#include "wpa_conf.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_failures;
#define CHECK(c, msg) do { if (c) printf("  PASS %s\n", msg); else { printf("  FAIL %s (line %d)\n", msg, __LINE__); g_failures++; } } while (0)

/* ---- replaced dependencies ---- */
static char g_written[2048];
static int g_writes;
static volatile int g_block_ping, g_ping_entered;
static char g_psk_file[256];

const char *config_store_path(const char *name) { return name; }

int write_file_content(const char *filepath, const char *content) {
    if (strcmp(filepath, "/etc/wpa_supplicant.conf") == 0) {
        snprintf(g_written, sizeof(g_written), "%s", content);
        g_writes++;
    }
    return 0;
}

char *read_file_text_alloc(const char *path, size_t max_sz, size_t *out_len) {
    (void)path; (void)max_sz;
    if (out_len) *out_len = 0;
    return NULL; /* no previous configuration */
}

int system(const char *cmd) { (void)cmd; return 0; }

/* the host's /etc is never looked at or changed: no Wi-Fi file there, removals only counted */
int access(const char *path, int mode) {
    static int (*real_access)(const char *, int);
    if (!real_access) real_access = (int (*)(const char *, int))dlsym(RTLD_NEXT, "access");
    if (strncmp(path, "/etc/", 5) == 0) {
        errno = ENOENT;
        return -1;
    }
    return real_access(path, mode);
}

int unlink(const char *path) {
    static int (*real_unlink)(const char *);
    if (!real_unlink) real_unlink = (int (*)(const char *))dlsym(RTLD_NEXT, "unlink");
    if (strncmp(path, "/etc/", 5) == 0) return 0;
    return real_unlink(path);
}

FILE *popen(const char *cmd, const char *mode) {
    (void)mode;
    if (strstr(cmd, " ping")) {
        g_ping_entered = 1;
        for (int i = 0; g_block_ping && i < 3000; i++) usleep(1000); /* never hang the test */
        return fmemopen((void *)"PONG\n", 5, "r");
    }
    if (strstr(cmd, " status")) return fmemopen((void *)"wpa_state=COMPLETED\n", 20, "r");
    return NULL;
}

int pclose(FILE *f) { return fclose(f); }

unsigned int sleep(unsigned int s) { (void)s; return 0; }

FILE *fopen(const char *path, const char *mode) {
    static FILE *(*real_fopen)(const char *, const char *);
    if (!real_fopen) real_fopen = (FILE * (*)(const char *, const char *))dlsym(RTLD_NEXT, "fopen");
    if (strcmp(path, "/etc/wpa_supplicant.conf") == 0) path = g_psk_file;
    else if (strcmp(path, "/etc/wpa_supplicant.ath0.conf") == 0) path = "/nonexistent/x";
    return real_fopen(path, mode);
}

/* ---- tests ---- */
static void test_value_rules(void) {
    CHECK(wpa_conf_value_error("Home", 0) == NULL, "plain SSID accepted");
    CHECK(wpa_conf_value_error("My \"Home\" $net", 0) == NULL, "SSID with quotes and $ accepted");
    CHECK(wpa_conf_value_error("Home\nnetwork={", 0) != NULL, "SSID with a line break refused");
    CHECK(wpa_conf_value_error("", 0) != NULL, "empty SSID refused");
    CHECK(wpa_conf_value_error("123456789012345678901234567890123", 0) != NULL, "SSID of 33 bytes refused");
    CHECK(wpa_conf_value_error("Pa$$word2026", 1) == NULL, "pass phrase with $ accepted");
    CHECK(wpa_conf_value_error("short", 1) != NULL, "pass phrase of 5 characters refused");
    CHECK(wpa_conf_value_error("pass\rword123", 1) != NULL, "pass phrase with a control character refused");
    char long_pw[65];
    memset(long_pw, 'x', 64);
    long_pw[64] = '\0';
    CHECK(wpa_conf_value_error(long_pw, 1) != NULL, "pass phrase of 64 non-hex characters refused");
    memset(long_pw, 'a', 64);
    CHECK(wpa_conf_value_error(long_pw, 1) == NULL && wpa_psk_is_hex(long_pw), "64 hex digits accepted as the raw key");
}

static void test_written_as_typed(void) {
    char err[256] = "";
    g_writes = 0;
    int rc = net_test_wifi_connection("My \"Home\"", "Pa$$word`2026\\", 0, 0, err, sizeof(err));
    CHECK(rc == 0, "test with special characters succeeds");
    CHECK(strstr(g_written, "ssid=\"My \"Home\"\"\n") != NULL, "SSID written unchanged");
    CHECK(strstr(g_written, "psk=\"Pa$$word`2026\\\"\n") != NULL, "pass phrase written unchanged (no backslashes added)");

    char hexkey[65];
    memset(hexkey, 'b', 64);
    hexkey[64] = '\0';
    rc = net_test_wifi_connection("Home", hexkey, 0, 0, err, sizeof(err));
    char want[80];
    snprintf(want, sizeof(want), "psk=%s\n", hexkey);
    CHECK(rc == 0 && strstr(g_written, want) != NULL, "64 hex digit key written without quotes");

    g_writes = 0;
    err[0] = '\0';
    rc = net_test_wifi_connection("Home", "short", 0, 0, err, sizeof(err));
    CHECK(rc != 0 && g_writes == 0 && strstr(err, "8 to 63"), "short pass phrase refused before anything is written");
    rc = net_test_wifi_connection("Home\nnetwork={", "password123", 0, 0, err, sizeof(err));
    CHECK(rc != 0 && g_writes == 0, "SSID with a line break refused before anything is written");
}

static void *first_test(void *arg) {
    char err[256] = "";
    *(int *)arg = net_test_wifi_connection("Home", "password123", 0, 0, err, sizeof(err));
    return NULL;
}

static void test_one_at_a_time(void) {
    int rc1 = -99;
    pthread_t th;
    g_block_ping = 1;
    g_ping_entered = 0;
    pthread_create(&th, NULL, first_test, &rc1);
    for (int i = 0; i < 5000 && !g_ping_entered; i++) usleep(1000);
    char err[256] = "";
    int writes_before = g_writes;
    int rc2 = net_test_wifi_connection("Other", "password456", 0, 0, err, sizeof(err));
    int writes_during = g_writes - writes_before;
    g_block_ping = 0;
    pthread_join(th, NULL);
    CHECK(rc2 != 0 && strstr(err, "Another Wi-Fi test is running") != NULL, "a second test while one runs is refused");
    CHECK(writes_during == 0, "the refused test writes nothing");
    CHECK(rc1 == 0, "the first test completes");
    err[0] = '\0';
    CHECK(net_test_wifi_connection("Other", "password456", 0, 0, err, sizeof(err)) == 0, "the next test runs after it");
}

static void test_psk_reader(void) {
    FILE *(*real_fopen)(const char *, const char *) = (FILE * (*)(const char *, const char *))dlsym(RTLD_NEXT, "fopen");
    snprintf(g_psk_file, sizeof(g_psk_file), "/tmp/test_wpa_conf_%d.conf", (int)getpid());
    FILE *f = real_fopen(g_psk_file, "w");
    fputs("ctrl_interface=/var/run/wpa_supplicant\nnetwork={\n\tssid=\"Home\"\n\tpsk=\"Pa$$\"word2026\"\n}\n", f);
    fclose(f);
    char psk[128];
    net_get_current_wifi_psk(psk, sizeof(psk));
    CHECK(strcmp(psk, "Pa$$\"word2026") == 0, "stored pass phrase read up to the last quote");

    f = real_fopen(g_psk_file, "w");
    fputs("network={\n\tssid=\"Home\"\n\tpsk=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n}\n", f);
    fclose(f);
    net_get_current_wifi_psk(psk, sizeof(psk));
    CHECK(strlen(psk) == 64 && psk[0] == 'b', "stored raw key (unquoted) read");
    unlink(g_psk_file);
}

int main(void) {
    test_value_rules();
    test_written_as_typed();
    test_one_at_a_time();
    test_psk_reader();
    if (g_failures) {
        printf("%d Wi-Fi configuration check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("All Wi-Fi configuration checks passed\n");
    return 0;
}
