#include "custom_icon_registry.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>

static int s_push_count = 0;
static char s_last_pushed_id[32] = {0};

int rf_push_custom_icon_file(int target_slot, const char *file_path, const char *id_str) {
    (void)target_slot; (void)file_path;
    s_push_count++;
    if (id_str) strncpy(s_last_pushed_id, id_str, sizeof(s_last_pushed_id) - 1);
    return 0;
}

int main(void) {
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
