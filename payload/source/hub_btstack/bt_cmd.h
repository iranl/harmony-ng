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
/* Pairing mode restarts its timeout (a host refused 0000: the user pairs it again) */
void pairing_timeout_restart(void);

/* In bt_main.c (they touch the SDP records and the HCI PIN replies):
 * what the Hub offers itself as while pairing: name, class of device and SDP records of the
 * keyboard or of the BD Remote (BT_PROFILE_*); back to the keyboard when pairing ends */
void bt_set_pairing_profile(uint8_t profile);
/* "pin_reply <digits>" / "pin_cancel": the answer to a held PIN request (bt_pin.h) */
void bt_pin_command(const char *cmd);
/* Pairing mode ended: a held PIN request gets a negative reply, the session is forgotten */
void bt_pin_pairing_end(void);
/* Generic Classic BT page: called from HCI_EVENT_CONNECTION_COMPLETE */
void bt_page_on_complete(const bd_addr_t addr, uint8_t status, hci_con_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_BT_CMD_H */
