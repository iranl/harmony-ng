#ifndef HARMONY_BT_LISTEN_H
#define HARMONY_BT_LISTEN_H

#include "bt_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* True while a plugin listens: the LE scan belongs to it, the remote reconnect waits */
extern bool g_adv_listen_active;

/* Handle "adv_listen <plugin_id> <seconds> [hex_filter]": scan passively for that long and send
 * each matching advertising report to /tmp/plugins/<plugin_id>.sock. 0 seconds stops early. */
void bt_listen_handle_cmd(const char *args);

/* Called for every advertising report while listening */
void bt_listen_on_report(const uint8_t *packet);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_BT_LISTEN_H */
