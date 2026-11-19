#ifndef USB_WIFI_H
#define USB_WIFI_H

/* USB Wi-Fi setup applet of hub_util (usb_wifi.c): the stock usbhid's TCP peer on
 * 127.0.0.1:16716, answering only the Wi-Fi file /sys/wifi/connect. */

#include <stddef.h>

#define USBWIFI_PORT 16716
/* connections=<n> (usbhid connects again each time a computer configures the USB device),
 * requests=<n>, last_attempt=none|ok|failed; read by network_manager.sh */
#define USBWIFI_STATE_PATH "/tmp/harmony_usbwifi.state"
/* USB_WIFI_SETUP=0 there turns the setup off (POST /api/ethernet-save usb_wifi_setup) */
#define USBWIFI_SETTING_PATH "/data/harmony/ethernet.conf"

int usbwifi_main(int argc, char **argv);

/* 0 when USB_WIFI_SETUP=0 is in ethernet.conf, else 1 */
int usbwifi_enabled(void);

/* The status the PC reads from /sys/wifi/connect: key,value lines; returns the length */
size_t usbwifi_status_text(char *out, size_t len);

#endif /* USB_WIFI_H */
