#ifndef RF_BUTTON_LIST_H
#define RF_BUTTON_LIST_H

#include <stdint.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* start/stop keys a push carries (the remote keeps MAX_ACT_TRIGGERS = 16 of them) */
#define RF_BUTTON_TRIGGERS_MAX 15

/* One RF_MSG_BUTTON_LIST_CHUNK entry of the config push */
typedef struct {
    char ctx[12];        /* activity of an on-screen button ("-1" = idle), or a trigger's target */
    char label[16];
    uint8_t ctype;       /* 0x01 on-screen button, 0x00 hard key */
    uint8_t atype;       /* 0x01 device command, 0x02 activity start, 0x03 activity stop */
    uint8_t idx;         /* position in the activity's on-screen list or hard-key map */
} rf_button_entry_t;

/*
 * The button list for a remote in activity mode, from the map's "activities" object.
 *
 * The remote uses two kinds of entries: on-screen buttons (its Devices view lists those of the
 * running activity, "-1" while idle) and the hard keys that start or stop an activity (it plays
 * the transition locally). Every other hard-key mapping stays on the Hub, which maps the raw key,
 * so it is not sent. The list starts with the start/stop keys of every activity (at most
 * RF_BUTTON_TRIGGERS_MAX, the remote keeps 16) and a PwrOff stop key when no activity maps PwrOff,
 * then the on-screen buttons of every activity; at most max entries. A limit therefore only ever
 * cuts on-screen buttons, also on a remote with an older firmware that keeps 48 entries.
 * Returns the number of entries written.
 */
int rf_build_button_list(const cJSON *acts_map, rf_button_entry_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* RF_BUTTON_LIST_H */
