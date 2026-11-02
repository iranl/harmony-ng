#ifndef HARMONY_ELITE_PRESENT_H
#define HARMONY_ELITE_PRESENT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Showing frames on /dev/fb0, and the frame timing log (ADR-0009).
 * Remote only: the host renderer stubs these in tools/elite_render/host_platform.c. */

/* Shows a frame by copying it into every framebuffer page. Returns whether the frame was
 * shown by page flipping. */
bool fb_present(const uint16_t *frame, bool wait_vsync);

/* Frame timing: while /tmp/elite_frame_timing exists, each frame's times are appended to
 * /tmp/frame_timing.log */
void frame_timing_poll(void);
bool frame_timing_enabled(void);
void frame_timing_record(const char *screen, uint32_t render_us, uint32_t present_us, bool flipped);

#ifdef __cplusplus
}
#endif

#endif
