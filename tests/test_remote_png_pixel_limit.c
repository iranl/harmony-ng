/*
 * test_remote_png_pixel_limit.c - Custom icons are limited to 16 KB on their way to the remote,
 * but a flat PNG compresses about 700:1, so a file that small can declare thousands of pixels per
 * side; blit_png_file() decoded it at full size (34 MB of RGBA for 3000x3000 on a remote with
 * 64 MB of RAM) and kept it in its cache. Writes a 600x600 PNG (stored deflate, any size works the
 * same way) and a 70x70 one, and draws both at 40x40.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>

#include "remote/remote_state.h"
#include "remote/remote_fb.h"

static uint32_t crc32_of(const uint8_t *p, size_t n, uint32_t crc) {
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void put32(FILE *f, uint32_t v) {
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    fwrite(b, 1, 4, f);
}

static void chunk(FILE *f, const char *type, const uint8_t *data, size_t n) {
    put32(f, (uint32_t)n);
    fwrite(type, 1, 4, f);
    if (n) fwrite(data, 1, n, f);
    uint32_t crc = crc32_of((const uint8_t *)type, 4, 0);
    put32(f, crc32_of(data, n, crc));
}

/* A grey w x h 8-bit greyscale PNG, image data in stored (uncompressed) deflate blocks */
static int write_png(const char *path, uint32_t w, uint32_t h) {
    size_t raw_len = (size_t)(w + 1) * h;
    uint8_t *raw = calloc(1, raw_len);
    if (!raw) return -1;
    for (uint32_t y = 0; y < h; y++) memset(raw + (size_t)y * (w + 1) + 1, 0x80, w);
    size_t blocks = (raw_len + 65534) / 65535;
    size_t z_len = 2 + raw_len + blocks * 5 + 4;
    uint8_t *z = malloc(z_len), *o = z;
    *o++ = 0x78; *o++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t off = 0; off < raw_len; off += 65535) {
        uint16_t n = (uint16_t)((raw_len - off) > 65535 ? 65535 : (raw_len - off));
        *o++ = (off + n == raw_len) ? 1 : 0;
        *o++ = (uint8_t)n; *o++ = (uint8_t)(n >> 8);
        *o++ = (uint8_t)~n; *o++ = (uint8_t)(~n >> 8);
        memcpy(o, raw + off, n);
        o += n;
    }
    for (size_t i = 0; i < raw_len; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    uint32_t adler = (b << 16) | a;
    *o++ = (uint8_t)(adler >> 24); *o++ = (uint8_t)(adler >> 16); *o++ = (uint8_t)(adler >> 8); *o++ = (uint8_t)adler;
    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return -1; }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                        (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 0, 0, 0, 0};
    chunk(f, "IHDR", ihdr, sizeof(ihdr));
    chunk(f, "IDAT", z, (size_t)(o - z));
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
    return 0;
}

int main(void) {
    char dir[64], big[128], small[128];
    snprintf(dir, sizeof(dir), "/tmp/remote_png_limit_test_%d", (int)getpid());
    mkdir(dir, 0755);
    snprintf(big, sizeof(big), "%s/activity_12.png", dir);
    snprintf(small, sizeof(small), "%s/activity_13.png", dir);
    if (write_png(big, 600, 600) != 0 || write_png(small, 70, 70) != 0) {
        printf("[-] test_remote_png_pixel_limit: cannot write the test images\n");
        return 1;
    }

    int fail = 0, decodes, scales;
    fb_png_work_take(&decodes, &scales);
    bool drawn = blit_png_file(0, 0, 40, 40, big);
    fb_png_work_take(&decodes, &scales);
    if (drawn || decodes != 0) {
        printf("[-] test_remote_png_pixel_limit: a 600x600 icon was decoded at full size (drawn=%d, decodes=%d)\n", drawn, decodes);
        fail = 1;
    }
    drawn = blit_png_file(0, 0, 40, 40, small);
    fb_png_work_take(&decodes, &scales);
    if (!drawn || decodes != 1) {
        printf("[-] test_remote_png_pixel_limit: a 70x70 icon must still draw (drawn=%d, decodes=%d)\n", drawn, decodes);
        fail = 1;
    }
    unlink(big);
    unlink(small);
    rmdir(dir);
    if (fail) return 1;
    printf("[+] test_remote_png_pixel_limit: oversized images are refused before they are decoded\n");
    return 0;
}
