#ifndef HARMONY_BT_PS3_H
#define HARMONY_BT_PS3_H

/* PlayStation 3 host profile: the Hub pairs as Sony's BD Remote Control, as the stock firmware
 * did for its "ps3" device type (usr/bin/hal holds the same SDP record and report descriptor).
 *
 * Input report 1, after the 0xa1 DATA header and the report id (Linux drivers/hid/hid-sony.c,
 * ps3remote_rdesc and its key maps):
 *   [0..2] bitmap of the gamepad keys, bit n = usage n+1 of ps3remote_keymap_joypad_buttons
 *   [3]    key code, usage of ps3remote_keymap_remote_buttons (0xff: none)
 *   [4..8] 0xff
 *   [9]    1 while a key is down, 0 on release
 *   [10]   battery level 0..5 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BT_PROFILE_KEYBOARD 0
#define BT_PROFILE_PS3      1

#define PS3_REMOTE_NAME     "BD Remote Control"
#define PS3_REMOTE_COD      0x00050C   /* Peripheral, remote control */
#define PS3_REMOTE_SUBCLASS 0x0C
#define PS3_REMOTE_VID      0x054C     /* Sony */
#define PS3_REMOTE_PID      0x0306
#define PS3_REPORT_LEN      13         /* 0xa1, id, 11 bytes */

typedef struct {
    const char *name;
    uint8_t code;
    int8_t bit;     /* gamepad bitmap bit, -1 for a remote-only key */
} ps3_key_t;

/* The report descriptor of the SDP record (stock hal, the real remote's) */
extern const uint8_t ps3_remote_descriptor[];
extern const size_t ps3_remote_descriptor_len;

/* The key a command name stands for (case does not matter), or NULL */
const ps3_key_t *ps3_key_by_name(const char *name);
/* The key a "PS3 <hex>" line names, or NULL */
const ps3_key_t *ps3_key_by_code(unsigned int code);
/* A keyboard (page 0x07) or consumer (page 0x0C) usage sent to a PS3 host, or NULL: a BT device
 * made for a keyboard host works on a PS3 for the keys both have */
const ps3_key_t *ps3_key_from_keyboard(uint8_t usage);
const ps3_key_t *ps3_key_from_consumer(uint16_t usage);
/* The input report for key down, or for the release when key is NULL */
void ps3_report(const ps3_key_t *key, uint8_t out[PS3_REPORT_LEN]);
/* "keyboard" / "ps3" and back; unknown names give -1 */
int bt_profile_from_name(const char *name);
const char *bt_profile_name(int profile);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_BT_PS3_H */
