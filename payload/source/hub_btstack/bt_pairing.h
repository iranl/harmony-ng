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
host_conn_t * register_host_target(const bd_addr_t addr, const char *name);
void update_bthid_status(void);
void update_status(const char *state_str);
int get_pinned_target(bd_addr_t addr);
int get_pinned_host_target(bd_addr_t addr);
void link_bt_device_to_addr(const char *dev_id, const char *addr_str);
const char *get_default_host_name(void);
void init_known_hosts(void);
void try_connect_host_addr(const bd_addr_t addr);
void try_connect_host(void);
void start_host_reconnect(const bd_addr_t target, int max_attempts);
void stop_host_reconnect(void);
void hog_start_scan(void);
void hog_connect(void);
void hog_start_connect(void);
void schedule_reconnect(uint32_t delay_ms);
void start_user_scan(void);
void start_user_pair(const char *addr_str);
void write_scan_results(bool scanning);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_BT_PAIRING_H */
