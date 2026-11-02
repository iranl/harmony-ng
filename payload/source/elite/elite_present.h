#ifndef HARMONY_ELITE_PRESENT_H
#define HARMONY_ELITE_PRESENT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Showing frames on /dev/fb0 by page flipping (ADR-0009), and the frame timing log.
 * Remote only: the host renderer stubs these in tools/elite_render/host_platform.c. */

/* Finds how many framebuffer pages can be flipped between, and shows page 0 */
void fb_present_init(void);
/* Shows a frame: copies it into a hidden page and pans the display to it, at the next
 * refresh when wait_vsync is set. Without page flipping it copies into every page.
 * Returns whether the frame was shown by page flipping. */
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
