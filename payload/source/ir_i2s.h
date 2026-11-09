#ifndef IR_I2S_H
#define IR_I2S_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Emitter selection masks for AR9331 START ioctl (high 16 bits of arg)
 * Bit 0: GPIO16 (IR Port 1)
 * Bit 1: GPIO28 (IR Port 2)
 * Bit 2: GPIO13 (Internal IR Blaster)
 */
#define IR_I2S_EMITTER_PORT1    (1 << 0)
#define IR_I2S_EMITTER_PORT2    (1 << 1)
#define IR_I2S_EMITTER_BLASTER  (1 << 2)
#define IR_I2S_EMITTER_ALL      (IR_I2S_EMITTER_PORT1 | IR_I2S_EMITTER_PORT2 | IR_I2S_EMITTER_BLASTER)

/**
 * Blast an IR carrier bitstream directly to /dev/i2s.
 * Bypasses Logitech HAL completely.
 *
 * @param blob Binary IR blob containing 16-byte Logitech header + duration words
 * @param blob_len Length of blob in bytes
 * @param port_mask 0 or combination of IR_I2S_EMITTER_* (default all)
 * @return 0 on success, negative error code on failure
 */
int ir_i2s_blast_blob(const uint8_t *blob, size_t blob_len, uint8_t port_mask);

/**
 * Blast raw mark/space durations directly to /dev/i2s.
 *
 * @param carrier_hz Carrier frequency in Hz (e.g. 38000)
 * @param duty Duty cycle percentage (1..50, default 50)
 * @param words Array of BE16 duration words (bit 15=mark, bits 0..14=us)
 * @param num_words Number of duration words
 * @param port_mask Emitter mask
 * @return 0 on success, negative error code on failure
 */
int ir_i2s_blast_words(uint32_t carrier_hz, uint8_t duty,
                       const uint16_t *words, size_t num_words,
                       uint8_t port_mask);

/**
 * Capture an incoming IR signal from /dev/i2s (RX ring).
 *
 * @param out Buffer to receive the captured raw timing string (F9470P...S...)
 * @param outlen Size of out buffer in bytes
 * @param timeout_sec Maximum seconds to wait for IR remote activity
 * @return 0 on success, negative error code on timeout/failure
 */
int ir_i2s_capture(char *out, size_t outlen, unsigned int timeout_sec);

int ir_i2s_init(void);
void ir_i2s_shutdown(void);
int ir_i2s_decode_capture(const uint8_t *raw_buf, size_t raw_len, char *out, size_t outlen);
/* As ir_i2s_decode_capture; *measured_hz gets the measured carrier, or 0 when there were too
 * few carrier pulses to measure (the string then says 38 kHz). */
int ir_i2s_decode_capture_ex(const uint8_t *raw_buf, size_t raw_len, char *out, size_t outlen,
                             uint32_t *measured_hz);

/*
 * Capture mode (Home Assistant spec 3.14): a thread keeps the receiver open and calls cb once
 * per button press with the decoded raw string (F...P...S...) and the measured carrier (0 when
 * not measurable). Sends keep working: the thread drops what it reads during a send and resets
 * the receive clock after it. The one-shot ir_i2s_capture() answers -11 while the mode is on.
 * cb runs on the capture thread.
 */
typedef void (*ir_rx_signal_cb)(const char *raw, uint32_t measured_hz, void *user);

/* enable 1: starts listening (or updates cb when already on); returns 0, -EBUSY (a one-shot
 * capture runs), -2 (no receiver), -3 (setup failed), -4 or -7 (a stalled session has not
 * closed), with a message in err. enable 0: stops and waits for the thread; returns 0. */
int ir_i2s_capture_mode_set(int enable, ir_rx_signal_cb cb, void *user, char *err, size_t errlen);
int ir_i2s_capture_mode_enabled(void);
/* 1 and the message when the running mode has a problem (receiver stalled or failing) */
int ir_i2s_capture_mode_error(char *buf, size_t len);
/* Called (no lock held) when the mode turns on or off or its problem message changes; read the
 * state with the two functions above. Runs on the caller of ir_i2s_capture_mode_set() or on the
 * capture thread. */
typedef void (*ir_capture_state_cb)(void *user);
void ir_i2s_set_capture_state_listener(ir_capture_state_cb cb, void *user);

#ifdef __cplusplus
}
#endif

#endif /* IR_I2S_H */
