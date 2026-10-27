#ifndef HUB_NETWORK_UTIL_H
#define HUB_NETWORK_UTIL_H

#include <stddef.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

void net_get_local_ip(int sock_fd, char *buf, size_t max_len);
void net_get_network_details(char *iface_out, size_t iface_len,
                             char *gw_out, size_t gw_len,
                             char *mask_out, size_t mask_len,
                             char *mac_out, size_t mac_len);
void net_get_iface_ip_mask_mac(const char *ifname,
                               char *ip_out, size_t ip_len,
                               char *mask_out, size_t mask_len,
                               char *mac_out, size_t mac_len);
int net_is_onboard_eth_device(const char *ifname);
int net_find_usb_eth_interface_ex(char *ifname_out, size_t ifname_len, int *is_onboard_out);
int net_get_iface_carrier(const char *ifname);
int net_get_usb_host_mode(void);
void net_get_wifi_ssid(char *ssid_out, size_t ssid_len);
void net_get_current_wifi_psk(char *psk_out, size_t psk_len);
int net_scan_wifi_networks(cJSON *out_array);
int net_test_wifi_connection(const char *ssid, const char *psk, int is_open, int hidden, char *err_msg, size_t err_len);

#ifdef __cplusplus
}
#endif

#endif /* HUB_NETWORK_UTIL_H */
