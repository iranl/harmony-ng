#ifndef HARMONY_BT_ADV_H
#define HARMONY_BT_ADV_H

#include "bt_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Handle "adv <hex> [ms]": queue the advertising data for a timed broadcast. */
void bt_adv_handle_cmd(const char *args);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_BT_ADV_H */
