#include "flash_image.h"

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
    if (ops->erase(part->fd, off, bs) != 0) {
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
    /* block 0 (the header) last */
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
