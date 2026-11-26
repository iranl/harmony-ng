/* flash_image.c on two files standing in for the kernel and kernel2 partitions. */
#include "flash_image.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failed;

#define CHECK(cond, name)                                         \
    do {                                                          \
        if (cond) {                                               \
            printf("  PASS %s\n", name);                          \
        } else {                                                  \
            printf("  FAIL %s (line %d)\n", name, __LINE__);      \
            failed++;                                             \
        }                                                         \
    } while (0)

#define BS 4096u
#define PART 4u  /* blocks per partition */

static uint32_t erase_log[64];
static int nerase;
static int fail_erase_at = -1;  /* erase number that fails */

static int f_erase(int fd, uint32_t off, uint32_t len) {
    if (nerase == fail_erase_at) return -1;
    erase_log[nerase++] = (uint32_t)fd << 24 | off;
    uint8_t ff[BS];
    memset(ff, 0xFF, sizeof(ff));
    return pwrite(fd, ff, len, off) == (ssize_t)len ? 0 : -1;
}

static long f_pread(int fd, void *buf, uint32_t len, uint32_t off) { return pread(fd, buf, len, off); }
static long f_pwrite(int fd, const void *buf, uint32_t len, uint32_t off) { return pwrite(fd, buf, len, off); }

static const struct flash_ops ops = { f_erase, f_pread, f_pwrite };

static void rd(int fd, void *buf, uint32_t len, uint32_t off) {
    if (pread(fd, buf, len, off) != (ssize_t)len) {
        printf("  FAIL read back\n");
        failed++;
    }
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

/* a uImage with size bytes of data */
static uint8_t *make_uimage(size_t size, size_t *len) {
    uint8_t *img = calloc(1, 64 + size);
    for (size_t i = 0; i < size; i++) img[64 + i] = (uint8_t)(i * 7 + 3);
    put32(img, 0x27051956);
    put32(img + 12, (uint32_t)size);
    put32(img + 24, flash_crc32(0, img + 64, size));
    put32(img + 4, flash_crc32(0, img, 64));
    *len = 64 + size;
    return img;
}

static int make_part(const char *path) {
    FILE *f = fopen(path, "w+b");
    for (unsigned i = 0; i < PART * BS; i++) fputc(0xA5, f);  /* old content */
    fclose(f);
    return open(path, O_RDWR);
}

static int matches(const struct flash_part *p, const uint8_t *img, size_t len) {
    uint8_t *all = malloc(2 * PART * BS);
    rd(p[0].fd, all, PART * BS, 0);
    rd(p[1].fd, all + PART * BS, PART * BS, 0);
    int ok = memcmp(all, img, len) == 0;
    size_t tail_end = (len + BS - 1) / BS * BS;
    for (size_t i = len; i < tail_end; i++) ok &= all[i] == 0xFF;
    for (size_t i = tail_end; i < 2 * PART * BS; i++) ok &= all[i] == 0xA5;  /* untouched */
    free(all);
    return ok;
}

int main(void) {
    char err[160];
    size_t len;
    uint8_t *img = make_uimage(5 * BS + 100, &len);  /* 6 blocks: 4 in kernel, 2 in kernel2 */

    printf("uimage_check\n");
    CHECK(uimage_check(img, len, err, sizeof(err)) == 0, "valid image accepted");
    CHECK(uimage_check(img, len - 1, err, sizeof(err)) != 0, "cut image refused");
    img[100] ^= 1;
    CHECK(uimage_check(img, len, err, sizeof(err)) != 0 && strstr(err, "data CRC"), "data CRC");
    img[100] ^= 1;
    img[40] ^= 1;
    CHECK(uimage_check(img, len, err, sizeof(err)) != 0 && strstr(err, "header CRC"), "header CRC");
    img[40] ^= 1;

    printf("flash_write_image\n");
    char p0[] = "/tmp/fi_kernelXXXXXX", p1[] = "/tmp/fi_kernel2XXXXXX";
    close(mkstemp(p0));
    close(mkstemp(p1));
    struct flash_part parts[2] = {
        { make_part(p0), PART * BS, BS },
        { make_part(p1), PART * BS, BS },
    };
    int written;
    nerase = 0;
    CHECK(flash_write_image(parts, 2, &ops, img, len, &written, err, sizeof(err)) == 0, "write ok");
    CHECK(written == 6, "6 blocks written");
    CHECK(matches(parts, img, len), "content across both partitions, tail erased, rest untouched");
    CHECK(nerase == 6 && erase_log[5] == ((uint32_t)parts[0].fd << 24 | 0), "header block erased last");

    nerase = 0;
    CHECK(flash_write_image(parts, 2, &ops, img, len, &written, err, sizeof(err)) == 0 && written == 0
          && nerase == 0, "same image again: nothing erased");

    size_t big;
    uint8_t *huge = make_uimage(8 * BS, &big);
    CHECK(flash_write_image(parts, 2, &ops, huge, big, &written, err, sizeof(err)) != 0, "too big refused");
    free(huge);

    /* new image, the 3rd erase fails: reported, header block still the old one */
    uint8_t first_before[BS];
    rd(parts[0].fd, first_before, BS, 0);
    img[64 + 2 * BS] ^= 0xFF;
    put32(img + 24, flash_crc32(0, img + 64, len - 64));
    put32(img + 4, 0);
    put32(img + 4, flash_crc32(0, img, 64));
    nerase = 0;
    fail_erase_at = 1;
    CHECK(flash_write_image(parts, 2, &ops, img, len, &written, err, sizeof(err)) != 0
          && strstr(err, "erase failed"), "erase failure reported");
    uint8_t first_after[BS];
    rd(parts[0].fd, first_after, BS, 0);
    CHECK(memcmp(first_before, first_after, BS) == 0, "header block untouched after a failure");
    fail_erase_at = -1;

    close(parts[0].fd);
    close(parts[1].fd);
    unlink(p0);
    unlink(p1);
    free(img);
    printf("%s\n", failed ? "FAILED" : "OK");
    return failed ? 1 : 0;
}
