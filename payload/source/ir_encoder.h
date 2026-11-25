#ifndef HARMONY_IR_ENCODER_H
#define HARMONY_IR_ENCODER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Encodes a standard Pronto Hex string into the 16-bit big-endian mark/space
 * binary waveform required by the Logitech Harmony HAL (/ir/ir_send).
 *
 * Parameters:
 *   pronto_hex: Hex string of 4-digit words (e.g. "0000 006d 0022 0002 ...")
 *   min_repeats: minimum repeat count (default 3 if 0)
 *   pre_silence_us: lead-in silence in microseconds (default 500 if 0)
 *   out_buf: pointer to allocated byte buffer (caller must free with ir_free_encoded)
 *   out_len: receives length of allocated buffer in bytes
 *
 * Returns 0 on success, negative error code on failure.
 */
int ir_encode_pronto(const char *pronto_hex, uint8_t min_repeats, uint16_t pre_silence_us,
                     uint8_t **out_buf, size_t *out_len);

/*
 * Encodes raw microsecond timings (alternating Mark, Space, Mark, Space...)
 * into the HAL binary waveform format.
 *
 * Parameters:
 *   freq_hz: carrier frequency (e.g. 38000 for 38 kHz)
 *   timings_us: array of durations in microseconds
 *   count: number of elements in timings_us
 *   min_repeats: minimum repeat count
 *   out_buf: pointer to allocated byte buffer
 *   out_len: receives length in bytes
 *
 * Returns 0 on success, negative error code on failure.
 */
int ir_encode_raw(uint32_t freq_hz, const uint32_t *timings_us, size_t count,
                  uint8_t min_repeats, uint8_t **out_buf, size_t *out_len);

/*
 * Encodes a Harmony protocol KeyCode string (e.g. "G:Sharp 48 Bit 2:()(0x...):3",
 * "G:Sharp 15 Bit 2:()(0x...):3", "G:NEC:()(0x...):1", or Pronto Hex "0000 ...")
 * directly into HAL binary waveform format.
 * Returns 0 on success, negative error code on failure.
 */
/* Most timings a raw string carries (the encoder's and the capture event's limit) */
#define IR_RAW_MAX_TIMINGS 2048

/* Parses F<carrier hex>P<mark hex>S<space hex>... (the Harmony raw string the IR capture
 * produces and the encoder plays). *carrier_hz gets the F value (0 if it was 0); timings get
 * microseconds, marks positive and spaces negative. Returns the number of timings, or -1 when
 * code is not a raw string. */
int ir_parse_harmony_raw(const char *code, uint32_t *carrier_hz, int32_t *timings, size_t max);

int ir_encode_harmony_keycode(const char *keycode, uint8_t min_repeats,
                             uint8_t **out_buf, size_t *out_len);

/* 1 when keycode is a "G:<protocol>:..." code of an RC5 protocol with a toggle bit (its
 * protocol name has "RC5" and "Toggle", as "Philips RC5 13 Bit Toggle") */
int ir_keycode_is_rc5_toggle(const char *keycode);

/* ir_encode_harmony_keycode() with the RC5 toggle bit of this press: rc5_toggle 1 sends an RC5
 * "Toggle" code with its toggle bit (bit 11 of the 13 sent) flipped, 0 as stored. Other codes
 * ignore it. A stock Hub keeps the bit per device and protocol and flips it after each press. */
int ir_encode_harmony_keycode_toggle(const char *keycode, uint8_t min_repeats, int rc5_toggle,
                                    uint8_t **out_buf, size_t *out_len);

/* A device's "pressMinRepeats" when it has none: what a stock Hub uses for an account without
 * the value. The field takes 0..IR_PRESS_MIN_REPEATS_MAX. */
#define IR_PRESS_MIN_REPEATS_DEFAULT 3
#define IR_PRESS_MIN_REPEATS_MAX     20

/*
 * How many repeat parts a press of keycode sends after its start part, as a stock Hub does
 * (irmanager.lua generateFromKeyCode, hal ir_send_thread): N = press_min_repeats, the device's
 * "pressMinRepeats" (negative: not set, IR_PRESS_MIN_REPEATS_DEFAULT; at most
 * IR_PRESS_MIN_REPEATS_MAX). blob is the code as ir_encode_harmony_keycode() made it.
 *  - a learned code (F<carrier>P...S...) or keycode NULL (timings given as they are): 0, the
 *    code goes out once
 *  - start and repeat are the same sequence (a keycode with an empty start group, most
 *    protocols): max(N, floor) - 1, so a press sends the frame max(N, floor) times; floor is
 *    3 for Sony (its receivers act only on a repeated frame), else 1
 *  - a start and a separate repeat sequence (e.g. "Toshiba 32 Bit", the frame then the NEC
 *    repeat code): N
 * The command's own "repeats" and the keycode's trailing ":N" do not count, as on stock.
 */
int ir_press_repeat_parts(const char *keycode, const uint8_t *blob, size_t blob_len, int press_min_repeats);

void ir_free_encoded(uint8_t *buf);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_IR_ENCODER_H */
