/* Showing frames on the remote's framebuffer, and the frame timing log.
 *
 * The framebuffer has up to three pages of 240x320. A frame is copied into a page the
 * display isn't showing, and FBIOPAN_DISPLAY points the LCD controller at it. That costs
 * one 150 KB copy instead of one per page, and the shown page is never written to. */
#include "remote_present.h"
#include "remote_state.h"
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, uint32_t)
#endif

#define FRAME_TIMING_FLAG "/tmp/remote_frame_timing"
#define FRAME_TIMING_LOG  "/tmp/frame_timing.log"

static struct fb_var_screeninfo s_vinfo;
static int s_pages = 1;  /* pages to flip between; 1 = copy into every page */
static int s_shown = 0;  /* page the display shows */
static uint64_t s_last_paced_us = 0;

/* Animation frames are shown on every second refresh of the 72 Hz display, 36 per second:
   wait for refreshes until 25 ms have passed since the last paced frame */
#define PACED_FRAME_MIN_US 25000

static void wait_for_paced_refresh(void) {
    for (int i = 0; i < 3; i++) {
        uint32_t crtc = 0;
        if (ioctl(g_fb_fd, FBIO_WAITFORVSYNC, &crtc) != 0) {
            uint64_t since = get_monotonic_us() - s_last_paced_us;
            if (since < PACED_FRAME_MIN_US) usleep((useconds_t)(PACED_FRAME_MIN_US - since));
            break;
        }
        if (get_monotonic_us() - s_last_paced_us >= PACED_FRAME_MIN_US) break;
    }
    s_last_paced_us = get_monotonic_us();
}

void fb_present_init(void) {
    s_pages = 1;
    s_shown = 0;
    if (g_fb_fd < 0 || !g_fb_mem) return;
    if (ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &s_vinfo) != 0) return;
    int pages = (int)(g_fb_size / FB_SIZE);
    if ((int)(s_vinfo.yres_virtual / FB_HEIGHT) < pages) pages = (int)(s_vinfo.yres_virtual / FB_HEIGHT);
    if (pages > 3) pages = 3;
    s_vinfo.xoffset = 0;
    s_vinfo.yoffset = 0;
    if (ioctl(g_fb_fd, FBIOPAN_DISPLAY, &s_vinfo) != 0) return;
    if (pages >= 2) s_pages = pages;
    printf("[+] Display: %d framebuffer pages, %s\n", pages, s_pages > 1 ? "page flipping" : "copying into every page");
}

bool fb_can_animate(void) {
    return s_pages >= 2;
}

static void copy_to_every_page(const uint16_t *frame) {
    for (size_t off = 0; off + FB_SIZE <= g_fb_size && off < FB_SIZE * 3; off += FB_SIZE) {
        memcpy((uint8_t *)g_fb_mem + off, frame, FB_SIZE);
    }
}

bool fb_present(const uint16_t *frame, bool wait_vsync) {
    if (s_pages < 2) {
        copy_to_every_page(frame);
        return false;
    }
    int next = (s_shown + 1) % s_pages;
    memcpy((uint8_t *)g_fb_mem + (size_t)next * FB_SIZE, frame, FB_SIZE);
    if (wait_vsync) wait_for_paced_refresh();
    s_vinfo.yoffset = (uint32_t)(next * FB_HEIGHT);
    if (ioctl(g_fb_fd, FBIOPAN_DISPLAY, &s_vinfo) != 0) {
        perror("[-] FBIOPAN_DISPLAY; copying frames into every page from now on");
        s_pages = 1;
        copy_to_every_page(frame);
        return false;
    }
    s_shown = next;
    return true;
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
        if (s_timing_log) fprintf(s_timing_log, "# ms screen theme layout render_us present_us flipped decodes scales\n");
    } else if (!want && s_timing_log) {
        fclose(s_timing_log);
        s_timing_log = NULL;
    }
}

bool frame_timing_enabled(void) {
    return s_timing_log != NULL;
}

void frame_timing_record(const frame_timing_t *t) {
    if (!s_timing_log) return;
    fprintf(s_timing_log, "%llu %s %s %s %u %u %d %d %d\n", (unsigned long long)get_monotonic_ms(),
            t->screen, t->theme, t->layout, t->render_us, t->present_us, t->flipped ? 1 : 0,
            t->decodes, t->scales);
    fflush(s_timing_log);
}
