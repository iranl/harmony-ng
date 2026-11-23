#ifndef HARMONY_BT_ADV_H
#define HARMONY_BT_ADV_H

#include "bt_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Handle "adv <hex> [ms] [addr=AA:BB:CC:DD:EE:FF]": queue the advertising data for a timed
 * broadcast, from the given address (set as the LE random address while it is on air) or the
 * Hub's own. */
void bt_adv_handle_cmd(const char *args);

/* True while a broadcast is on air from a given address: an LE connection would use it */
bool bt_adv_borrowed_addr(void);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_BT_ADV_H */
