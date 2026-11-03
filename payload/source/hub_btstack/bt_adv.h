#ifndef HARMONY_BT_ADV_H
#define HARMONY_BT_ADV_H

#include "bt_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Handle "adv <hex> [ms]": queue the advertising data for a timed broadcast. */
void bt_adv_handle_cmd(const char *args);
/* Queue advertising data for ms milliseconds (0 = default). Returns 0, or -1 if invalid or the queue is full. */
int bt_adv_enqueue(const uint8_t *data, int len, int ms);
/* Broadcasts queued or on air. */
int bt_adv_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_BT_ADV_H */
