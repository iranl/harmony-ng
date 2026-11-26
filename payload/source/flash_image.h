/* Write a U-Boot kernel image (uImage) across one or more flash partitions, checked block by
 * block. The device I/O goes through callbacks so the logic runs on plain files in tests. */
#ifndef FLASH_IMAGE_H
#define FLASH_IMAGE_H

#include <stddef.h>
#include <stdint.h>

struct flash_part {
    int fd;
    uint32_t size;       /* bytes */
    uint32_t erasesize;  /* bytes, the write unit */
};

struct flash_ops {
    int (*erase)(int fd, uint32_t off, uint32_t len);   /* 0 on success */
    long (*pread)(int fd, void *buf, uint32_t len, uint32_t off);
    long (*pwrite)(int fd, const void *buf, uint32_t len, uint32_t off);
};

uint32_t flash_crc32(uint32_t crc, const uint8_t *p, size_t len);

/* 0 when img is a complete uImage with valid header and data CRCs and len covers it. */
int uimage_check(const uint8_t *img, size_t len, char *err, size_t errlen);

/* Writes img over the partitions, which must follow each other on the flash in the given
 * order. Every erase block is read first and skipped when it already holds the data; others
 * are erased, written and read back. The first block (the uImage header) is written last, so
 * a write cut short leaves a header whose CRC check fails rather than a valid-looking image.
 * Returns 0, or -1 with err set. *written counts the blocks that were rewritten. */
int flash_write_image(const struct flash_part *parts, int nparts, const struct flash_ops *ops,
                      const uint8_t *img, size_t len, int *written, char *err, size_t errlen);

#endif
