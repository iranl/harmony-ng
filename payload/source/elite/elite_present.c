/* Showing frames on the remote's framebuffer, and the frame timing log (ADR-0009) */
#include "elite_present.h"
#include "elite_state.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define FRAME_TIMING_FLAG "/tmp/elite_frame_timing"
#define FRAME_TIMING_LOG  "/tmp/frame_timing.log"

bool fb_present(const uint16_t *frame, bool wait_vsync) {
    (void)wait_vsync;
    for (size_t off = 0; off + FB_SIZE <= g_fb_size && off < FB_SIZE * 3; off += FB_SIZE) {
        memcpy((uint8_t *)g_fb_mem + off, frame, FB_SIZE);
    }
    return false;
}

static FILE *s_timing_log = NULL;
static uint64_t s_timing_checked_ms = 0;

void frame_timing_poll(void) {
    uint64_t now = get_monotonic_ms();
    if (s_timing_checked_ms && now - s_timing_checked_ms < 1000) return;
    s_timing_checked_ms = now;
    bool want = access(FRAME_TIMING_FLAG, F_OK) == 0;
    if (want && !s_timing_log) {
        s_timing_log = fopen(FRAME_TIMING_LOG, "a");
        if (s_timing_log) fprintf(s_timing_log, "# ms screen render_us present_us flipped\n");
    } else if (!want && s_timing_log) {
        fclose(s_timing_log);
        s_timing_log = NULL;
    }
}

bool frame_timing_enabled(void) {
    return s_timing_log != NULL;
}

void frame_timing_record(const char *screen, uint32_t render_us, uint32_t present_us, bool flipped) {
    if (!s_timing_log) return;
    fprintf(s_timing_log, "%llu %s %u %u %d\n", (unsigned long long)get_monotonic_ms(), screen,
            render_us, present_us, flipped ? 1 : 0);
    fflush(s_timing_log);
}
