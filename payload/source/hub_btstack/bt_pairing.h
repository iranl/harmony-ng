#ifndef HARMONY_BT_PAIRING_H
#define HARMONY_BT_PAIRING_H

#include "bt_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bd_addr_t addr;
    uint8_t addr_type;
    char name[64];
    int8_t rssi;
} discovered_remote_t;

#define MAX_DISCOVERED 16
extern discovered_remote_t g_discovered[MAX_DISCOVERED];
extern int g_discovered_count;
extern bool g_user_scan_active;
extern time_t g_user_scan_end;

uint16_t get_target_host_hid_cid(void);
bool is_any_host_connected(void);
bool is_host_connected(const bd_addr_t addr);
bool all_known_hosts_connected(void);
/* The Bluetooth host of the activity with this id (its first BT device with a bdaddr) */
bool get_activity_host_target(const char *act_id, bd_addr_t out);
/* Per-host "disconnect requested" flag; a null address applies to every known host */
bool host_manual_disconnect(const bd_addr_t addr);
void set_host_manual_disconnect(const bd_addr_t addr, bool on);
/* Host to reconnect after a drop: the current target host, else the pinned one */
bool get_reconnect_target(bd_addr_t out);
host_conn_t * register_host_target(const bd_addr_t addr, const char *name);
void update_bthid_status(void);
void update_status(const char *state_str);
int get_pinned_target(bd_addr_t addr);
int get_pinned_host_target(bd_addr_t addr);
void link_bt_device_to_addr(const char *dev_id, const char *addr_str);
void init_known_hosts(void);
void try_connect_host_addr(const bd_addr_t addr);
void try_connect_host(void);
void start_host_reconnect(const bd_addr_t target, int max_attempts);
void stop_host_reconnect(void);
void hog_start_scan(void);
void hog_connect(void);
/* Drop the pinned BLE remote: disconnect, delete the target file, its LE bond and the HOGD tag */
void forget_ble_remote(void);
void hog_start_connect(void);
void schedule_reconnect(uint32_t delay_ms);
void start_user_scan(void);
void stop_user_scan(void);
void start_user_pair(const char *addr_str);
void write_scan_results(bool scanning);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_BT_PAIRING_H */
