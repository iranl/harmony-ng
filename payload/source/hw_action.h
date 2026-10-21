#ifndef CODEX_HW_ACTION_H
#define CODEX_HW_ACTION_H

#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#ifndef DEBUG_LOG_CONFIG
#define DEBUG_LOG_CONFIG "/data/codex/debug_logging.conf"
#endif
#ifndef BT_DEBUG_FLAG
#define BT_DEBUG_FLAG "/data/codex/bt_remote_debug"
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
int hw_ir_send_raw(uint32_t freq_hz, const uint32_t *timings_us, size_t count, uint8_t ports, uint8_t repeats);

/*
 * Cancels active IR transmission on specified ports.
 */
int hw_ir_cancel(uint8_t ports);

/*
 * Direct Hardware IR Transmit for Harmony KeyCode (G:<Proto>:()(0x...):repeats or Pronto)
 */
int hw_ir_send_harmony_keycode(const char *keycode, uint8_t ports, uint8_t repeats);

/*
 * Direct device command dispatcher: looks up device and command in DeviceList.json,
 * encodes protocol waveform, and sends directly via I2S, BTstack FIFO, or MQTT.
 */
int hw_device_command_send(const char *device_id, const char *command);

/*
 * Callback registration for in-process MQTT button dispatch (used by codex_daemon to avoid loopback socket deadlock).
 */
typedef int (*hw_mqtt_button_handler_t)(const char *device_id, const char *dev_name, const char *command);
void hw_set_mqtt_button_handler(hw_mqtt_button_handler_t handler);

/*
 * Sends a BT HID report directly via BTstack FIFO (/tmp/bthid_input).
 */
int hw_bthid_send_report(const uint8_t *report, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* CODEX_HW_ACTION_H */
