/* hub_util usbhid: relays the HID gadget's 64-byte reports to hub_util usbwifi (usb_hid_relay.c) */
#ifndef USB_HID_RELAY_H
#define USB_HID_RELAY_H

int usbhid_relay_main(int argc, char **argv);

#endif
