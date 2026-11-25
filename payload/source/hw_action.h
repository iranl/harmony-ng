#ifndef HARMONY_HW_ACTION_H
#define HARMONY_HW_ACTION_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include <unistd.h>

#ifndef DEBUG_LOG_CONFIG
#define DEBUG_LOG_CONFIG "/data/harmony/debug_logging.conf"
#endif
#ifndef BT_DEBUG_FLAG
#define BT_DEBUG_FLAG "/data/harmony/bt_remote_debug"
#endif

/* Set while this thread sends a key that just came from a remote (the key channel from hub_rf):
 * the gaps between commands (interDeviceDelay, interKeyDelay, holdInterKeyDelay) are for sequences, a live key goes
 * out at once */
extern __thread int g_live_remote_key;

/* Key trace (debug logging only): when hub_rf received the key this thread is sending and when
 * hub_daemon took it from the key channel, both CLOCK_MONOTONIC in ms; 0 = not a key from a
 * remote. With debug logging on they go to hub_btstack in a "TS" line before the command;
 * hub_btstack adds its own times and writes one line per key to /tmp/key_trace.log. */
extern __thread unsigned long long g_key_trace_rx_ms;
extern __thread unsigned long long g_key_trace_daemon_ms;

static inline int is_debug_log_enabled(void) {
    return (access(DEBUG_LOG_CONFIG, F_OK) == 0 || access(BT_DEBUG_FLAG, F_OK) == 0);
}

#define IR_PORT_BLASTER1  (1 << 0) /* 1 */
#define IR_PORT_BLASTER2  (1 << 1) /* 2 */
#define IR_PORT_INTERNAL  (1 << 2) /* 4 */
#define IR_PORT_ALL       (IR_PORT_BLASTER1 | IR_PORT_BLASTER2 | IR_PORT_INTERNAL) /* 7 */

#ifdef __cplusplus
extern "C" {
#endif

extern int g_dry_run;


/*
 * Direct Hardware IR Transmit via /dev/i2s
 * Encodes Pronto Hex string into binary waveform and transmits on specified blaster ports.
 * Returns 0 on success, negative on error.
 */
int hw_ir_send_pronto(const char *pronto_hex, uint8_t ports, uint8_t repeats);

/*
 * Direct Hardware IR Transmit with raw microsecond timings.
 */
#ifdef TEST_HARNESS
cJSON *hw_test_last_raw_json(void);
#endif
int hw_ir_send_raw(uint32_t freq_hz, const uint32_t *timings_us, size_t count, uint8_t ports, uint8_t repeats);

/*
 * A held IR key (HW_BTN_ACTION_PRESS) repeats until its release. It also stops by itself when
 * no hold (HW_BTN_ACTION_REPEAT, the heartbeat of the key that is down) came for this long:
 * a lost release must not leave a key repeating. The stock firmware allows 500 ms for holds
 * 300 ms apart; remotes on older harmony-ng firmware send their first hold after 450 ms.
 */
#define HW_IR_HOLD_DEADLINE_MS 600

/*
 * Ends the held IR key, if any, after the frame on air (does not wait for it).
 */
int hw_ir_cancel(uint8_t ports);

/*
 * Direct Hardware IR Transmit for Harmony KeyCode (G:<Proto>:()(0x...):repeats or Pronto)
 */
int hw_ir_send_harmony_keycode(const char *keycode, uint8_t ports, uint8_t repeats);

/* Button actions for hold/repeat normalization */
#define HW_BTN_ACTION_TAP     0
#define HW_BTN_ACTION_PRESS   1
#define HW_BTN_ACTION_REPEAT  2
#define HW_BTN_ACTION_RELEASE 3

/*
 * Direct device command dispatcher: looks up device and command in json,
 * encodes protocol waveform, and sends directly via I2S, BTstack FIFO, or MQTT.
 */
int hw_device_command_send_action(const char *target, int match_mode, const char *command, int action);
/* As _send_action, with a numeric argument for plugin commands (the datagram's "value"). A
 * plugin command with "range": [min, max] refuses a value outside it with HW_ERR_VALUE_RANGE,
 * and the senders without a value (HW_VALUE_NONE: remote keys, activity steps, send_command
 * without "value") with HW_ERR_VALUE_MISSING. Other command types ignore the value. */
int hw_device_command_send_value(const char *target, int match_mode, const char *command, int action, int value);
#define HW_VALUE_NONE INT_MIN
/* outside the errno range: the infrared senders return -errno codes (-7 is -E2BIG) */
#define HW_ERR_VALUE_RANGE (-1007)
#define HW_ERR_VALUE_MISSING (-1008)
/* an HTTP/TCP command not sent: the send queue of net_action.c is full */
#define HW_ERR_QUEUE_FULL (-1009)
/* 1 when value fits the command's "range" (or the command has none) */
int hw_command_value_in_range(const cJSON *cmd, int value);
/* 1 when the device's command (exact name, any case) is a plugin command with a range, so a
 * send without a value would be refused. 0 while the command's infrared alternative is on: the
 * infrared send takes no value */
int hw_device_command_needs_value(const char *target, int match_mode, const char *command);
int hw_device_command_send(const char *device_id, const char *command);
int hw_device_command_send_by_id(const char *device_id, const char *command);
int hw_device_command_send_by_name(const char *device_name, const char *command);
int hw_device_exists(const char *target, int match_mode);
int hw_device_command_exists(const char *target, int match_mode, const char *command);

/* The Hub's name for plugins (settings hub_name), sent as "hub_name" in every plugin datagram: a
 * plugin that registers the Hub on its device (a Samsung TV's list of allowed devices) shows it.
 * HW_HUB_NAME_DEFAULT when none was set (or the old default "Harmony Hub", the stock Logitech
 * name). Set by apply_hub_settings() at start and on every settings save. */
#define HW_HUB_NAME_DEFAULT "Harmony-NG Hub"
void hw_set_hub_name(const char *name);
void hw_get_hub_name(char *out, size_t len);

/*
 * In-process handler for commands of MQTT-type devices: hub_mqtt publishes the device's pulse
 * (its topic template, "1" then "0" after mqttPulseMs). Registered by hub_mqtt_init(); the
 * command is also posted to /api/button-press so the bus gets the button event.
 */
typedef int (*hw_mqtt_button_handler_t)(const char *device_id, const char *dev_name, const char *command);
void hw_set_mqtt_button_handler(hw_mqtt_button_handler_t handler);

/*
 * Sends a BT HID report directly via BTstack FIFO (/tmp/bthid_input).
 */
int hw_bthid_send_report(const uint8_t *report, size_t len);
/* RELEASE to hub_btstack: every key it still holds on any host goes up. 0 when the pipe took it. */
int hw_bt_release_all(void);
/* The Bluetooth host a device's keys go to: its bdaddr in config/devices.json; for a device of
 * type BT without one, the default host. 1 and the address in out, 0 when the device has none. */
int hw_device_bt_host(const char *device_id, char *out, size_t outlen);
/* Closes the Bluetooth link of a device's host (disconnect_host <addr> to hub_btstack), as the
 * stock firmware does after a Bluetooth device's power-off command. 0 when sent, -1 without a host. */
int hw_device_bt_disconnect(const char *device_id);
/*
 * Plugin socket command dispatcher: sends formatted command JSON to /tmp/plugins/<plugin_id>.sock
 */
int hw_plugin_command_send(const char *plugin_id, const char *dev_id, const char *dev_name,
                          const char *command, int action, int value, const cJSON *config);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_HW_ACTION_H */
