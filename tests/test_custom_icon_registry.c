#include "custom_icon_registry.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

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

    printf("CUSTOM ICON REGISTRY UNIT TESTS PASSED!\n");
    return 0;
}
