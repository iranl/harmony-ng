#ifndef HARMONY_HW_ACTION_H
#define HARMONY_HW_ACTION_H

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
 * Cancels active IR transmission on specified ports.
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
int hw_device_command_send(const char *device_id, const char *command);
int hw_device_command_send_by_id(const char *device_id, const char *command);
int hw_device_command_send_by_name(const char *device_name, const char *command);

/*
 * In-process handler for commands of MQTT-type devices: hub_mqtt publishes the device's pulse
 * (its topic template, "1" then "0" after mqttPulseMs). Registered by hub_mqtt_init(); the
 * command is also posted to /api/button-press so the bus gets the button event.
 */
typedef int (*hw_mqtt_button_handler_t)(const char *device_id, const char *dev_name, const char *command);
void hw_set_mqtt_button_handler(hw_mqtt_button_handler_t handler);
/* The port hub_daemon listens on (the MQTT device path posts /api/button-press to itself) */
void hw_set_local_api_port(int port);

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

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_HW_ACTION_H */
