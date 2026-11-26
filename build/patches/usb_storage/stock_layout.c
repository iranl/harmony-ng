/*
 * Compiles only when the copy matches the stock Hub kernel where the storage modules depend on
 * it and no CRC can tell (the CRCs come from the stock image). build_kmods.sh builds this file
 * with usb-storage and does not ship it.
 * - struct request: the stock blk_rq_init clears 196 bytes (32-bit sector_t, no LBDAF); with
 *   200 bytes scsi_mod hit a BUG in scsi_setup_blk_pc_cmnd at the first scan.
 * - HZ: the stock msecs_to_jiffies is (m + 3) / 4, so 250; sd and usb-storage compile their
 *   timeouts as HZ multiples.
 * - kmalloc(<constant>) is inlined as an index into the kernel's malloc_sizes[] (8-byte entries
 *   without ZONE_DMA, as read from the stock image).
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/slab.h>

#define STOCK(what, want) extern char stock_layout[(what) == (want) ? 1 : -1]
STOCK(sizeof(struct request), 196);
STOCK(sizeof(sector_t), 4);
STOCK(HZ, 250);
STOCK(sizeof(struct cache_sizes), 8);
STOCK(PAGE_SIZE, 4096);
STOCK(L1_CACHE_BYTES, 32);

MODULE_LICENSE("GPL");
