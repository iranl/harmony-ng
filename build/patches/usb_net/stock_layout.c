/*
 * Compiles only when the copy's structs have the stock Hub kernel's layout. modversions
 * cannot catch a layout difference (the CRCs come from the stock image), and a shifted
 * struct sk_buff makes usbnet submit its first receive URB with a bogus buffer.
 * The offsets are read from the stock kernel image and the stock modules (athrs_gmac.ko,
 * umac.ko, usbcore.ko; HZ from msecs_to_jiffies). build_usb_ethernet.sh builds this file with the drivers and does not
 * ship it.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/usb.h>

#define STOCK(what, want) extern char stock_layout[(what) == (want) ? 1 : -1]
STOCK(offsetof(struct sk_buff, len), 80);
STOCK(offsetof(struct sk_buff, protocol), 104);
STOCK(offsetof(struct sk_buff, tail), 140);
STOCK(offsetof(struct sk_buff, data), 152);
STOCK(offsetof(struct sk_buff, users), 160);
STOCK(offsetof(struct net_device, netdev_ops), 180);
STOCK(offsetof(struct net_device, dev_addr), 320);
STOCK(ALIGN(sizeof(struct net_device), NETDEV_ALIGN), 704);
STOCK(offsetof(struct urb, transfer_buffer), 60);
STOCK(offsetof(struct urb, complete), 112);
STOCK(offsetof(struct usb_device, dev), 104);
STOCK(offsetof(struct usb_interface, dev), 32);
STOCK(sizeof(struct sk_buff), 168);
STOCK(offsetof(struct net_device, watchdog_timeo), 424);
STOCK(offsetof(struct usb_device, actconfig), 288);
STOCK(sizeof(struct timer_list), 24);
STOCK(sizeof(struct mutex), 12);
STOCK(sizeof(wait_queue_head_t), 8);
/* not in a CRC either: HZ timeouts are compiled into the modules, and kmalloc(<constant>)
 * is inlined as an index into the kernel's malloc_sizes[] (8-byte entries without ZONE_DMA) */
STOCK(HZ, 250);
STOCK(sizeof(struct cache_sizes), 8);
STOCK(PAGE_SIZE, 4096);
STOCK(L1_CACHE_BYTES, 32);

MODULE_LICENSE("GPL");
