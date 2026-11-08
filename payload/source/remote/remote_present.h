#ifndef HARMONY_REMOTE_PRESENT_H
#define HARMONY_REMOTE_PRESENT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Showing frames on /dev/fb0 by page flipping, and the frame timing log.
 * Remote only: the host renderer stubs these in tools/remote_render/host_platform.c. */

/* Finds how many framebuffer pages can be flipped between, and shows page 0 */
void fb_present_init(void);
/* Shows a frame: copies it into a hidden page and pans the display to it. With wait_vsync
 * set, it pans at a refresh at least 25 ms after the last such frame, so animation runs at
 * 36 frames per second. Without page flipping it copies into every page.
 * Returns whether the frame was shown by page flipping. */
bool fb_present(const uint16_t *frame, bool wait_vsync);

/* Whether frames can wait for display refreshes, which animation needs */
bool fb_can_animate(void);

/* Frame timing: while /tmp/remote_frame_timing exists, each frame is appended to
 * /tmp/frame_timing.log */
typedef struct {
    const char *screen, *theme, *layout;
    uint32_t render_us, present_us;
    bool flipped;
    int decodes, scales; /* PNGs decoded and scaled while drawing the frame */
} frame_timing_t;

void frame_timing_poll(void);
bool frame_timing_enabled(void);
void frame_timing_record(const frame_timing_t *t);

#ifdef __cplusplus
}
#endif

#endif
