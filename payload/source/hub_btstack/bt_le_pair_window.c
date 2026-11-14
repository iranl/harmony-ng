#include "bt_le_pair_window.h"

#include <string.h>
#include <time.h>

static bool s_open;
static uint8_t s_addr[6];
static uint32_t s_until;

uint32_t le_pair_now_s(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint32_t)ts.tv_sec;
}

void le_pair_window_open(const uint8_t addr[6], uint32_t now_s) {
    if (!addr) return;
    memcpy(s_addr, addr, sizeof(s_addr));
    s_until = now_s + LE_PAIR_WINDOW_SEC;
    s_open = true;
}

void le_pair_window_close(void) {
    s_open = false;
    memset(s_addr, 0, sizeof(s_addr));
    s_until = 0;
}

bool le_pair_window_allows(const uint8_t addr[6], uint32_t now_s) {
    if (!s_open || !addr) return false;
    if ((int32_t)(now_s - s_until) >= 0) {
        le_pair_window_close();
        return false;
    }
    return memcmp(addr, s_addr, sizeof(s_addr)) == 0;
}
