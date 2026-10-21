#ifndef CODEX_IR_ENCODER_H
#define CODEX_IR_ENCODER_H

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
int ir_encode_harmony_keycode(const char *keycode, uint8_t min_repeats,
                             uint8_t **out_buf, size_t *out_len);

void ir_free_encoded(uint8_t *buf);

#ifdef __cplusplus
}
#endif

#endif /* CODEX_IR_ENCODER_H */
