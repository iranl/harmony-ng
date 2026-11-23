#include "custom_icon_registry.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>

static int s_push_count = 0;
static char s_last_pushed_id[32] = {0};
static char s_last_path[256];
static char s_last_bytes[4096];
static size_t s_last_len;

int rf_push_custom_icon_file(int target_slot, const char *file_path, const char *id_str) {
    (void)target_slot;
    s_push_count++;
    if (id_str) strncpy(s_last_pushed_id, id_str, sizeof(s_last_pushed_id) - 1);
    /* what the sender gets: the file as it is while the transfer runs */
    snprintf(s_last_path, sizeof(s_last_path), "%s", file_path ? file_path : "");
    s_last_len = 0;
    FILE *f = file_path ? fopen(file_path, "rb") : NULL;
    if (f) {
        s_last_len = fread(s_last_bytes, 1, sizeof(s_last_bytes), f);
        fclose(f);
    }
    return 0;
}

/* With DIR (the fixture of test_icon_pack.py, built with -DICON_PACK_ARCHIVES): a stock icon that
 * is only in the packed archive reaches the sender byte for byte through a file in /tmp, which is
 * removed after the transfer */
static int pack_case(const char *dir) {
    char big[300];
    snprintf(big, sizeof(big), "%s/big.png", dir);
    FILE *f = fopen(big, "rb");
    assert(f);
    static char want[4096];
    size_t want_len = fread(want, 1, sizeof(want), f);
    fclose(f);
    uint16_t id = custom_icon_registry_get_id("big.png");
    s_push_count = 0;
    int rc = custom_icon_registry_sync_remote(1, id, "big.png");
    assert(rc == 0 && s_push_count == 1);
    assert(strncmp(s_last_path, "/tmp/icon_push.", 15) == 0);
    assert(s_last_len == want_len && memcmp(s_last_bytes, want, want_len) == 0);
    assert(access(s_last_path, F_OK) != 0); /* removed after the transfer */
    printf("CUSTOM ICON REGISTRY PACK CASE PASSED!\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1) return pack_case(argv[1]);
    /* Clean up any test registry */
    unlink("/data/harmony/custom_icons_registry.json");
    unlink("/data/harmony/custom_icons_pushed.json");
    unlink("/tmp/custom_icons_registry.json");
    unlink("/tmp/custom_icons_pushed.json");

    uint16_t id1 = custom_icon_registry_get_id("switch.png");
    assert(id1 == 0x8001);
    assert(id1 & 0x8000);

    /* Idempotent lookup for same icon */
    uint16_t id1_again = custom_icon_registry_get_id("switch");
    assert(id1_again == id1);

    /* Second distinct icon */
    uint16_t id2 = custom_icon_registry_get_id("heos.png");
    assert(id2 == 0x8002);
    assert(id2 & 0x8000);

    /* Test custom_icon_registry_sync_remote always calls rf_push_custom_icon_file when file exists */
    mkdir("/tmp/www", 0755);
    mkdir("/tmp/www/assets", 0755);
    mkdir("/tmp/www/assets/icons", 0755);
    FILE *ft = fopen("/tmp/www/assets/icons/switch.png", "w");
    if (ft) { fputs("test", ft); fclose(ft); }

    s_push_count = 0;
    int rc = custom_icon_registry_sync_remote(1, id1, "switch.png");
    assert(rc == 0);
    assert(s_push_count == 1);
    assert(strcmp(s_last_pushed_id, "32769") == 0);

    /* On subsequent push, it still invokes rf_push_custom_icon_file so remote can probe / confirm */
    rc = custom_icon_registry_sync_remote(1, id1, "switch.png");
    assert(rc == 0);
    assert(s_push_count == 2);

    unlink("/tmp/www/assets/icons/switch.png");

    printf("CUSTOM ICON REGISTRY UNIT TESTS PASSED!\n");
    return 0;
}
