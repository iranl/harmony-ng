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
/* Ends a running listen (no-op without one): a remote scan or pairing the user started wins */
void bt_listen_end(const char *why);
/* The pinned remote advertised during a listen: the listen ends, and a new one is refused
 * until the remote is connected (at most ADV_LISTEN_REMOTE_HOLD_MS) */
void bt_listen_end_for_remote(void);
/* false after the remote ended a listen and did not connect: its reports end no listen then */
bool bt_listen_remote_may_end(void);

/* Called for every advertising report while listening */
void bt_listen_on_report(const uint8_t *packet);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_BT_LISTEN_H */
