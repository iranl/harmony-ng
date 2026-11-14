/*
 * test_remote_png_cache.c - fb_invalidate_png_cache() runs after every custom icon the remote
 * receives. It cleared the key of the matching cache entries but kept their pixels and their
 * slots: the cache has 192 slots and never reuses one, so after enough icon updates nothing new
 * could be cached and the icon was decoded and scaled again on every frame (and every update
 * leaked the old copies). Draws a custom icon at 40x40 (original + scaled copy), invalidates it,
 * many times over, then checks that a second frame is served from the cache.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "remote/remote_state.h"
#include "remote/remote_fb.h"

/* A 4x4 RGBA PNG */
static const unsigned char k_png[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x08, 0x06, 0x00, 0x00, 0x00, 0xa9, 0xf1, 0x9e,
    0x7e, 0x00, 0x00, 0x00, 0x12, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
    0x1f, 0x19, 0x33, 0x90, 0x2e, 0x00, 0x00, 0x3c, 0x40, 0x1f, 0xe1, 0x1a, 0xf3, 0xa5, 0x48, 0x00,
    0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
};

int main(void) {
    char dir[64], path[128];
    snprintf(dir, sizeof(dir), "/tmp/remote_png_cache_test_%d", (int)getpid());
    mkdir(dir, 0755);
    snprintf(path, sizeof(path), "%s/activity_12.png", dir);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(k_png, 1, sizeof(k_png), f) != sizeof(k_png)) {
        printf("[-] test_remote_png_cache: cannot write %s\n", path);
        return 1;
    }
    fclose(f);

    int decodes, scales, fail = 0;
    for (int update = 0; update < 300 && !fail; update++) {
        fb_png_work_take(&decodes, &scales);
        if (!blit_png_file(0, 0, 40, 40, path)) {
            printf("[-] test_remote_png_cache: the icon did not draw\n");
            fail = 1;
            break;
        }
        blit_png_file(0, 0, 40, 40, path); /* the next frame */
        fb_png_work_take(&decodes, &scales);
        if (decodes != 1 || scales != 1) {
            printf("[-] test_remote_png_cache: after %d icon updates two frames took %d decode(s) and %d scaling(s), expected 1 and 1\n",
                   update, decodes, scales);
            fail = 1;
        }
        fb_invalidate_png_cache("activity_12"); /* what the remote does when the icon is updated */
    }
    unlink(path);
    rmdir(dir);
    if (fail) return 1;
    printf("[+] test_remote_png_cache: 300 icon updates, the cache still keeps the icon between frames\n");
    return 0;
}
