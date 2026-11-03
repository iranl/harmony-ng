#ifndef HARMONY_BT_MANTRA_H
#define HARMONY_BT_MANTRA_H

#include "bt_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MANTRA_LEARN_FILE "/tmp/hub_mantra_learn.json"

/* Handle "mantra_learn [seconds]" and "mantra_learn_stop"; the stop also ends pairing. */
void bt_mantra_learn_cmd(const char *args, bool stop);
/* Handle "mantra_pair [seconds]": search until a freshly powered-on fan answers, then bind. */
void bt_mantra_pair_cmd(const char *args);
/* Check each advertising report for a Mantra command while listening. */
void bt_mantra_handle_adv(const uint8_t *ad_data, uint8_t ad_len, int8_t rssi);
/* Keep the search on air and end the window when it times out; called from the 100 ms command poll. */
void bt_mantra_learn_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_BT_MANTRA_H */
