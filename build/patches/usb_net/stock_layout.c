/*
 * Compiles only when the copy's structs have the stock Hub kernel's layout. modversions
 * cannot catch a layout difference (the CRCs come from the stock image), and a shifted
 * struct sk_buff makes usbnet submit its first receive URB with a bogus buffer.
 * The offsets are read from the stock kernel image and the stock modules (athrs_gmac.ko,
 * umac.ko, usbcore.ko). build_usb_ethernet.sh builds this file with the drivers and does not
 * ship it.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
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

MODULE_LICENSE("GPL");
