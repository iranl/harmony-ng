/* rf_migrate_legacy_names_in(): Hub files of builds before the elite -> remote rename.
 * Build: gcc -Wall -O2 -Ipayload/source tests/test_rf_migrate.c payload/source/rf_state.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rf_state.h"

static int g_failures;
#define CHECK(cond, msg) do { if (!(cond)) { g_failures++; printf("  FAIL %s (line %d)\n", msg, __LINE__); } } while (0)

static void put(const char *dir, const char *name, const char *text) {
    char p[256]; snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE *f = fopen(p, "w"); fputs(text, f); fclose(f);
}
static int is(const char *dir, const char *name, const char *text) {
    char p[256], b[64] = {0}; snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE *f = fopen(p, "r"); if (!f) return 0;
    size_t n = fread(b, 1, sizeof(b) - 1, f); fclose(f); b[n] = 0;
    return strcmp(b, text) == 0;
}
static int gone(const char *dir, const char *name) {
    char p[256]; snprintf(p, sizeof(p), "%s/%s", dir, name); return access(p, F_OK) != 0;
}

int main(void) {
    printf("[*] Hub legacy name migration tests...\n");
    char h[] = "/tmp/rf_migrate_h_XXXXXX", c[] = "/tmp/rf_migrate_c_XXXXXX";
    if (!mkdtemp(h) || !mkdtemp(c)) { perror("mkdtemp"); return 1; }
    put(h, "elite_slot1_version.json", "v1"); put(h, "elite_slot1_config.json", "c1");
    put(h, "elite_slot2_version.json", "v2"); put(h, "elite_ui.ota.gz", "img"); put(h, "elite_ui.version", "2.3.0");
    put(c, "elite_ui.ota.gz", "cimg");
    put(h, "remote_slot2_config.json", "new-c2"); put(h, "elite_slot2_config.json", "old-c2");
    put(h, "remote_slot1_map.json", "map");
    int moved = rf_migrate_legacy_names_in(h, c);
    CHECK(moved == 6, "six files moved");
    CHECK(is(h, "remote_slot1_version.json", "v1") && gone(h, "elite_slot1_version.json"), "slot 1 version file");
    CHECK(is(h, "remote_slot1_config.json", "c1") && is(h, "remote_slot2_version.json", "v2"), "slot config and slot 2 version");
    CHECK(is(h, "remote_ui.ota.gz", "img") && is(h, "remote_ui.version", "2.3.0"), "staged update image and its version");
    CHECK(is(c, "remote_ui.ota.gz", "cimg"), "update image staged in /cache");
    CHECK(is(h, "remote_slot2_config.json", "new-c2") && is(h, "elite_slot2_config.json", "old-c2"), "an existing new file is not replaced");
    CHECK(is(h, "remote_slot1_map.json", "map"), "the button map is not touched");
    CHECK(rf_migrate_legacy_names_in(h, c) == 0, "a second run moves nothing");
    char cmd[128]; snprintf(cmd, sizeof(cmd), "rm -rf %s %s", h, c);
    if (system(cmd) != 0) { /* nothing */ }
    if (g_failures) { printf("[-] %d migration checks failed\n", g_failures); return 1; }
    printf("[+] All Hub legacy name migration checks passed\n");
    return 0;
}
