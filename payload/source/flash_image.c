#include "flash_image.h"

#include <mtd/mtd-user.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UIMAGE_MAGIC 0x27051956u
#define UIMAGE_HDR 64u

uint32_t flash_crc32(uint32_t crc, const uint8_t *p, size_t len) {
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

int uimage_check(const uint8_t *img, size_t len, char *err, size_t errlen) {
    if (len < UIMAGE_HDR || be32(img) != UIMAGE_MAGIC) {
        snprintf(err, errlen, "not a uImage");
        return -1;
    }
    uint8_t hdr[UIMAGE_HDR];
    memcpy(hdr, img, UIMAGE_HDR);
    memset(hdr + 4, 0, 4);  /* the header CRC is computed with its own field zeroed */
    if (flash_crc32(0, hdr, UIMAGE_HDR) != be32(img + 4)) {
        snprintf(err, errlen, "uImage header CRC mismatch");
        return -1;
    }
    uint32_t size = be32(img + 12);
    if (size > len - UIMAGE_HDR) {
        snprintf(err, errlen, "uImage is cut: header says %u data bytes, file has %u",
                 (unsigned)size, (unsigned)(len - UIMAGE_HDR));
        return -1;
    }
    if (flash_crc32(0, img + UIMAGE_HDR, size) != be32(img + 24)) {
        snprintf(err, errlen, "uImage data CRC mismatch");
        return -1;
    }
    return 0;
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

int squashfs_check(const uint8_t *img, size_t len, int comp, char *err, size_t errlen) {
    if (len < 96 || memcmp(img, "hsqs", 4) != 0) {
        snprintf(err, errlen, "not a squashfs image");
        return -1;
    }
    unsigned int c = img[20] | img[21] << 8;
    unsigned int major = img[28] | img[29] << 8;
    if (major != 4) {
        snprintf(err, errlen, "squashfs version %u, not 4", major);
        return -1;
    }
    if (c != (unsigned int)comp) {
        snprintf(err, errlen, "squashfs compression %u, not %d", c, comp);
        return -1;
    }
    /* bytes_used, 64-bit little endian at 40: the image must not be cut */
    if (le32(img + 44) != 0 || le32(img + 40) > len) {
        snprintf(err, errlen, "squashfs is cut: %u bytes used, file has %u", le32(img + 40),
                 (unsigned)len);
        return -1;
    }
    return 0;
}

static int mtd_erase(int fd, uint32_t off, uint32_t len) {
    struct erase_info_user ei = { .start = off, .length = len };
    return ioctl(fd, MEMERASE, &ei);
}

static long mtd_pread(int fd, void *buf, uint32_t len, uint32_t off) {
    return pread(fd, buf, len, off);
}

static long mtd_pwrite(int fd, const void *buf, uint32_t len, uint32_t off) {
    return pwrite(fd, buf, len, off);
}

const struct flash_ops flash_mtd_ops = { mtd_erase, mtd_pread, mtd_pwrite };

/* one erase block: content wanted at (part, off) */
static int write_block(const struct flash_part *part, const struct flash_ops *ops, uint32_t off,
                       const uint8_t *want, uint8_t *buf, int *written, char *err, size_t errlen) {
    uint32_t bs = part->erasesize;
    if (ops->pread(part->fd, buf, bs, off) != (long)bs) {
        snprintf(err, errlen, "read failed at 0x%x", (unsigned)off);
        return -1;
    }
    if (memcmp(buf, want, bs) == 0)
        return 0;
    /* erased already (block 0 is erased first, see flash_write_image) */
    size_t i = 0;
    while (i < bs && buf[i] == 0xFF) i++;
    if (i < bs && ops->erase(part->fd, off, bs) != 0) {
        snprintf(err, errlen, "erase failed at 0x%x", (unsigned)off);
        return -1;
    }
    if (ops->pwrite(part->fd, want, bs, off) != (long)bs) {
        snprintf(err, errlen, "write failed at 0x%x", (unsigned)off);
        return -1;
    }
    if (ops->pread(part->fd, buf, bs, off) != (long)bs || memcmp(buf, want, bs) != 0) {
        snprintf(err, errlen, "verify failed at 0x%x", (unsigned)off);
        return -1;
    }
    (*written)++;
    return 0;
}

int flash_write_image(const struct flash_part *parts, int nparts, const struct flash_ops *ops,
                      const uint8_t *img, size_t len, int *written, char *err, size_t errlen) {
    *written = 0;
    uint64_t total = 0;
    uint32_t bs = nparts > 0 ? parts[0].erasesize : 0;
    for (int i = 0; i < nparts; i++) {
        if (parts[i].erasesize != bs || bs == 0 || parts[i].size % bs) {
            snprintf(err, errlen, "partitions have different or odd erase sizes");
            return -1;
        }
        total += parts[i].size;
    }
    if (nparts == 0 || len == 0 || len > total) {
        snprintf(err, errlen, "image (%u bytes) does not fit the partitions (%llu bytes)",
                 (unsigned)len, (unsigned long long)total);
        return -1;
    }

    uint8_t *want = malloc(bs), *buf = malloc(bs);
    if (!want || !buf) {
        free(want);
        free(buf);
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    int rc = 0;
    size_t nblocks = (len + bs - 1) / bs;
    /* Block 0 (the uImage header, the squashfs superblock) is erased first when it changes and
     * written last: a write cut short leaves no header and no magic, so U-Boot does not boot a
     * kernel of two versions and init does not mount a root of two (it boots the fallback and
     * retries the staged image), instead of the old header over partly new blocks. */
    if (nblocks > 1) {
        size_t n0 = len < bs ? len : bs;
        memcpy(want, img, n0);
        memset(want + n0, 0xFF, bs - n0);
        if (ops->pread(parts[0].fd, buf, bs, 0) != (long)bs) {
            snprintf(err, errlen, "read failed at 0x0");
            rc = -1;
        } else if (memcmp(buf, want, bs) != 0 && ops->erase(parts[0].fd, 0, bs) != 0) {
            snprintf(err, errlen, "erase failed at 0x0");
            rc = -1;
        }
    }
    for (size_t step = 1; step <= nblocks && rc == 0; step++) {
        size_t b = step % nblocks;
        size_t pos = b * bs;
        size_t n = len - pos < bs ? len - pos : bs;
        memcpy(want, img + pos, n);
        memset(want + n, 0xFF, bs - n);  /* the tail of the last block stays erased */
        int p = 0;
        uint64_t start = 0;
        while (pos >= start + parts[p].size) {
            start += parts[p].size;
            p++;
        }
        rc = write_block(&parts[p], ops, (uint32_t)(pos - start), want, buf, written, err, errlen);
    }
    free(want);
    free(buf);
    return rc;
}
