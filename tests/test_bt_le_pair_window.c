/* Bluetooth LE remote pairing window (hub_btstack/bt_le_pair_window.c): a new LE pairing is
 * accepted only for the address the user asked to pair, and only for LE_PAIR_WINDOW_SEC seconds. */
#include <stdio.h>
#include "bt_le_pair_window.h"

static int failed;
#define CHECK(c, name) do { if (c) printf("  PASS %s\n", name); else { printf("  FAIL %s\n", name); failed++; } } while (0)

int main(void) {
    const uint8_t remote[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x05 };
    const uint8_t other[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x06 };
    uint32_t t0 = 1000;

    printf("[-] LE pairing window\n");
    CHECK(!le_pair_window_allows(remote, t0), "closed at start: nothing may pair");
    le_pair_window_open(remote, t0);
    CHECK(le_pair_window_allows(remote, t0), "open: the requested address may pair");
    CHECK(le_pair_window_allows(remote, t0 + LE_PAIR_WINDOW_SEC - 1), "still open just before the end");
    CHECK(!le_pair_window_allows(other, t0 + 1), "another address may not pair while it is open");
    CHECK(!le_pair_window_allows(remote, t0 + LE_PAIR_WINDOW_SEC), "closed when the time is up");
    CHECK(!le_pair_window_allows(remote, t0 + 1), "stays closed once expired");

    le_pair_window_open(remote, t0);
    le_pair_window_close();
    CHECK(!le_pair_window_allows(remote, t0 + 1), "closed by le_pair_window_close (pairing done or remote forgotten)");

    le_pair_window_open(remote, t0);
    le_pair_window_open(other, t0 + 5);
    CHECK(!le_pair_window_allows(remote, t0 + 6) && le_pair_window_allows(other, t0 + 6),
          "a new request replaces the address");

    le_pair_window_open(remote, 0xFFFFFFF0u);
    CHECK(le_pair_window_allows(remote, 0xFFFFFFF0u + 60) && !le_pair_window_allows(remote, 0xFFFFFFF0u + LE_PAIR_WINDOW_SEC),
          "works across the counter wrap");

    uint32_t a = le_pair_now_s(), b = le_pair_now_s();
    CHECK(b >= a, "monotonic clock does not go back");
    if (failed) {
        printf("[-] %d check(s) failed\n", failed);
        return 1;
    }
    printf("[+] bt_le_pair_window: all passed\n");
    return 0;
}
