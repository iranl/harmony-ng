#ifndef HARMONY_BT_CMD_H
#define HARMONY_BT_CMD_H

#include "bt_state.h"

#ifdef __cplusplus
extern "C" {
#endif

extern btstack_timer_source_t cmd_poll_timer;

void execute_btstack_cmd(const char *cmd_in);
void poll_bthid_fifo(void);
void init_cmd_socket(void);
void cleanup_cmd_socket(void);
void check_command_file(void);
void cmd_poll_timeout(btstack_timer_source_t *ts);
/* Pairing mode: on for at most PAIRING_TIMEOUT_MS, then off as with pair_host_off */
#define PAIRING_TIMEOUT_MS (180 * 1000)
void pairing_mode_off(const char *reason);
void pairing_timeout_cancel(void);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_BT_CMD_H */
