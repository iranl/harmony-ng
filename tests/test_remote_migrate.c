/* remote_migrate_legacy(): <data>/elite_* -> <data>/remote_*, old names left as links.
 * Build: gcc -Wall -O2 -Ipayload/source/remote tests/test_remote_migrate.c payload/source/remote/remote_migrate.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "remote_migrate.h"

static int g_failures;
#define CHECK(cond, msg) do { if (!(cond)) { g_failures++; printf("  FAIL %s (line %d)\n", msg, __LINE__); } } while (0)
static char g_dir[64];

static void put(const char *name, const char *text) {
    char p[256]; snprintf(p, sizeof(p), "%s/%s", g_dir, name);
    FILE *f = fopen(p, "w"); fputs(text, f); fclose(f);
}
static int is(const char *name, const char *text) {
    char p[256], b[64] = {0}; snprintf(p, sizeof(p), "%s/%s", g_dir, name);
    FILE *f = fopen(p, "r"); if (!f) return 0;
    size_t n = fread(b, 1, sizeof(b) - 1, f); fclose(f); b[n] = 0;
    return strcmp(b, text) == 0;
}
static int is_link_to(const char *name, const char *target) {
    char p[256], b[64] = {0}; snprintf(p, sizeof(p), "%s/%s", g_dir, name);
    ssize_t n = readlink(p, b, sizeof(b) - 1);
    return n > 0 && strcmp(b, target) == 0;
}
static int exists(const char *name) {
    char p[256]; struct stat st; snprintf(p, sizeof(p), "%s/%s", g_dir, name);
    return lstat(p, &st) == 0;
}

int main(void) {
    printf("[*] remote_migrate_legacy tests...\n");
    snprintf(g_dir, sizeof(g_dir), "/tmp/remote_migrate_c_XXXXXX");
    if (!mkdtemp(g_dir)) { perror("mkdtemp"); return 1; }

    /* 1. a remote on a build from before the rename */
    put("elite_ui", "bin-old"); put("elite_sync", "sync"); put("elite_settings.conf", "settings");
    put("elite_rf_config.json", "rf"); put("elite_activity_usage.conf", "usage");
    char d[256]; snprintf(d, sizeof(d), "%s/elite_conf_p", g_dir); mkdir(d, 0755);
    CHECK(remote_migrate_legacy(g_dir) == 6, "six files moved");
    CHECK(is("remote_ui", "bin-old") && is_link_to("elite_ui", "remote_ui"), "binary moved, old name a link");
    CHECK(is("remote_sync", "sync") && is_link_to("elite_sync", "remote_sync"), "sync helper moved");
    CHECK(is("remote_settings.conf", "settings") && is("elite_settings.conf", "settings"), "settings moved and still readable under the old name");
    CHECK(is("remote_rf_config.json", "rf") && is("remote_activity_usage.conf", "usage"), "RF config and usage moved");
    CHECK(exists("remote_conf_p") && !exists("elite_conf_p"), "factory-reset backup directory moved");
    struct stat st; char p[256]; snprintf(p, sizeof(p), "%s/remote_ui", g_dir);
    CHECK(stat(p, &st) == 0 && (st.st_mode & 0111), "binary executable");

    /* 2. idempotent */
    CHECK(remote_migrate_legacy(g_dir) == 0, "a second run moves nothing");

    /* 3. an older installer put a new binary under the old name */
    snprintf(p, sizeof(p), "%s/elite_ui", g_dir); unlink(p); put("elite_ui", "bin-new");
    CHECK(remote_migrate_legacy(g_dir) == 1 && is("remote_ui", "bin-new") && is_link_to("elite_ui", "remote_ui"),
          "a binary installed under the old name replaces remote_ui");

    /* 4. a new settings file is never replaced by an old one */
    put("elite_ui.conf", "stale"); put("remote_ui.conf", "current");
    remote_migrate_legacy(g_dir);
    CHECK(is("remote_ui.conf", "current"), "an existing new file is kept");

    char cmd[128]; snprintf(cmd, sizeof(cmd), "rm -rf %s", g_dir);
    if (system(cmd) != 0) { /* nothing */ }

    /* 5. a fresh install: links only for what exists */
    if (!mkdtemp(strcpy(g_dir, "/tmp/remote_migrate_c_XXXXXX"))) return 1;
    put("remote_ui", "bin");
    CHECK(remote_migrate_legacy(g_dir) == 0, "fresh install: nothing moved");
    CHECK(is_link_to("elite_ui", "remote_ui") && !exists("elite_settings.conf") && !exists("elite_sync"), "fresh install: only the binary link");
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_dir);
    if (system(cmd) != 0) { /* nothing */ }

    if (g_failures) { printf("[-] %d migration checks failed\n", g_failures); return 1; }
    printf("[+] All remote_migrate_legacy checks passed\n");
    return 0;
}
