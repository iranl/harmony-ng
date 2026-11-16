#ifndef BT_LE_PAIR_WINDOW_H
#define BT_LE_PAIR_WINDOW_H

/* Bluetooth LE remote pairing window. A new LE pairing (Just Works or numeric comparison) is
 * accepted only for the address the user chose in the web UI (POST /api/remote-pair -> "PAIR
 * <addr>"), and only for LE_PAIR_WINDOW_SEC seconds after that request. Outside the window a
 * device that advertises with the pinned remote's address can neither pair nor replace the
 * stored bond. No BTstack dependency, so it can be tested on its own. */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LE_PAIR_WINDOW_SEC 120

/* Monotonic seconds (not changed by SNTP setting the clock). */
uint32_t le_pair_now_s(void);
void le_pair_window_open(const uint8_t addr[6], uint32_t now_s);
void le_pair_window_close(void);
bool le_pair_window_allows(const uint8_t addr[6], uint32_t now_s);

#ifdef __cplusplus
}
#endif

#endif
