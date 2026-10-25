/*
 * codex_btstack.c - Standalone BTstack BLE Remote Daemon for Logitech Harmony Hub
 *
 * Implements HID-over-GATT (HOGP) Host client:
 *  - Exclusive ownership of hci0 via raw Linux HCI socket
 *  - Automatic scanning, connection, and SMP Just Works pairing / bonding
 *  - Persistent key storage in /data/codex/btstack_keys.tlv
 *  - Full Homatics B25 (and standard BLE remotes) HID button decoding
 *  - Emits events to /tmp/bt_remote_events.log and status to /tmp/codex_btstack_status.json
 *  - Dispatches actions according to /data/codex/bt_remote_map.json
 */
#define _GNU_SOURCE
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "btstack_config.h"
#include "btstack.h"
#include "btstack_event.h"
#include "btstack_run_loop_posix.h"
#include "btstack_tlv.h"
#include "btstack_tlv_posix.h"
#include "ble/le_device_db_tlv.h"
#include "ble/sm.h"
#include "ble/gatt-service/hids_host.h"
#include "ad_parser.h"
#include "hci_dump.h"
#include "hci_dump_posix_stdout.h"
#include "hci_transport.h"
#include "hci_transport_linux.h"
#include "cJSON.h"

#include "classic/sdp_server.h"
#include "classic/sdp_util.h"
#include "classic/device_id_server.h"
#include "classic/hid_device.h"
#include "classic/btstack_link_key_db.h"
#include "classic/btstack_link_key_db_tlv.h"

#define PID_FILE            "/tmp/codex_btstack.pid"
#define STATUS_FILE         "/tmp/codex_btstack_status.json"
#define LOG_FILE            "/tmp/codex-btstack.log"
#define BT_EVENT_FILE       "/tmp/bt_remote_events.log"
#define MAP_FILE            "/data/codex/bt_remote_map.json"
#define CURRENT_ACT_FILE    "/data/codex/current_activity"
#define TARGET_FILE         "/data/codex/bt_remote_target"
#define TLV_DB_PATH         "/data/codex/btstack_keys.tlv"
#define DEBUG_LOG_CONFIG    "/data/codex/debug_logging.conf"
#define BT_DEBUG_FLAG       "/data/codex/bt_remote_debug"

bool is_debug_log_enabled(void) {
    return (access(DEBUG_LOG_CONFIG, F_OK) == 0 || access(BT_DEBUG_FLAG, F_OK) == 0);
}
#define CMD_FILE            "/tmp/codex_btstack_cmd"
#define CMD_SOCK            "/tmp/codex_btstack.sock"
#define SCAN_RESULT_FILE    "/tmp/codex_btstack_scan.json"
#define BTHID_FIFO          "/tmp/bthid_input"
#define BTHID_STATUS_FILE   "/tmp/bthid_status"
#define LOG_MAX_BYTES       65536

static int g_cmd_sock_fd = -1;
static btstack_data_source_t g_cmd_sock_ds;

#define TLV_TAG_HOGD ((((uint32_t)'H') << 24) | (((uint32_t)'O') << 16) | (((uint32_t)'G') << 8) | 'D')
#define TLV_TAG_HIDR ((((uint32_t)'H') << 24) | (((uint32_t)'I') << 16) | (((uint32_t)'D') << 8) | 'R')

typedef struct {
    bd_addr_t addr;
    bd_addr_type_t addr_type;
} le_device_addr_t;

static enum {
    APP_IDLE = 0,
    APP_W4_WORKING,
    APP_SCANNING,
    APP_CONNECTING,
    APP_ENCRYPTING,
    APP_W4_HIDS,
    APP_READY
} app_state = APP_IDLE;

static le_device_addr_t remote_device;
static hci_con_handle_t connection_handle = HCI_CON_HANDLE_INVALID;
static uint16_t hids_cid = 0;
static hid_protocol_mode_t protocol_mode = HID_PROTOCOL_MODE_REPORT;
static uint8_t hid_descriptor_storage[1000];

static btstack_timer_source_t reconnect_timer;
static btstack_tlv_posix_t tlv_context;
static const btstack_tlv_t * tlv_impl = NULL;

static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_packet_callback_registration_t sm_event_callback_registration;
static gatt_client_notification_t gatt_notification_listener;

// Classic BT HID Device (Host connection to streaming device / TV / PC)
#define MAX_HOST_CONNS 4
typedef struct {
    uint16_t hid_cid;
    hci_con_handle_t acl_handle;
    bd_addr_t addr;
    bool connected;
    char name[64];
} host_conn_t;

static host_conn_t g_hosts[MAX_HOST_CONNS];
static int g_active_host_idx = 0;
static uint16_t g_host_hid_cid = 0;
static bd_addr_t g_host_addr;
static bool g_host_connected = false;
static bool g_host_pairing = false;
static char g_host_name[64] = "Harmony Keyboard";
static uint8_t hid_service_buffer[1024];
static uint8_t device_id_sdp_service_buffer[512];
static int g_bthid_fifo_fd = -1;
static unsigned long g_keys_sent = 0;

#define MAX_HOST_RECONNECT_ATTEMPTS 15
static btstack_timer_source_t g_host_reconnect_timer;
static bool g_host_reconnect_active = false;
static int g_host_reconnect_attempts = 0;
static bd_addr_t g_host_reconnect_target;
static bool g_host_connecting = false;
static time_t g_host_connecting_start = 0;
static char g_pending_wake_btn[32] = {0};
static time_t g_pending_wake_time = 0;

static const uint8_t hid_descriptor_composite[] = {
    // Report ID 1: Standard Keyboard
    0x05, 0x01,                    // Usage Page (Generic Desktop)
    0x09, 0x06,                    // Usage (Keyboard)
    0xa1, 0x01,                    // Collection (Application)
    0x85, 0x01,                    //   Report ID (1)
    0x05, 0x07,                    //   Usage Page (Key Codes)
    0x19, 0xe0,                    //   Usage Minimum (Left Control)
    0x29, 0xe7,                    //   Usage Maximum (Right GUI)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0x01,                    //   Logical Maximum (1)
    0x75, 0x01,                    //   Report Size (1)
    0x95, 0x08,                    //   Report Count (8)
    0x81, 0x02,                    //   Input (Data, Variable, Absolute) - Modifier byte
    0x95, 0x01,                    //   Report Count (1)
    0x75, 0x08,                    //   Report Size (8)
    0x81, 0x03,                    //   Input (Constant, Variable, Absolute) - Reserved byte
    0x95, 0x05,                    //   Report Count (5)
    0x75, 0x01,                    //   Report Size (1)
    0x05, 0x08,                    //   Usage Page (LEDs)
    0x19, 0x01,                    //   Usage Minimum (Num Lock)
    0x29, 0x05,                    //   Usage Maximum (Kana)
    0x91, 0x02,                    //   Output (Data, Variable, Absolute) - LEDs
    0x95, 0x01,                    //   Report Count (1)
    0x75, 0x03,                    //   Report Size (3)
    0x91, 0x03,                    //   Output (Constant, Variable, Absolute) - LED Padding
    0x95, 0x06,                    //   Report Count (6)
    0x75, 0x08,                    //   Report Size (8)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0xff,                    //   Logical Maximum (255)
    0x05, 0x07,                    //   Usage Page (Key Codes)
    0x19, 0x00,                    //   Usage Minimum (0)
    0x29, 0xff,                    //   Usage Maximum (255)
    0x81, 0x00,                    //   Input (Data, Array) - 6 Keycodes
    0xc0,                          // End Collection

    // Report ID 2: Consumer Control (Media, Volume, Android TV Nav)
    0x05, 0x0c,                    // Usage Page (Consumer)
    0x09, 0x01,                    // Usage (Consumer Control)
    0xa1, 0x01,                    // Collection (Application)
    0x85, 0x02,                    //   Report ID (2)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x26, 0xff, 0x03,              //   Logical Maximum (1023)
    0x19, 0x00,                    //   Usage Minimum (0)
    0x2a, 0xff, 0x03,              //   Usage Maximum (1023)
    0x75, 0x10,                    //   Report Size (16)
    0x95, 0x01,                    //   Report Count (1)
    0x81, 0x00,                    //   Input (Data, Array) - 16-bit Consumer Code
    0xc0,                          // End Collection

    // Report ID 3: Mouse (Buttons + Relative dX, dY + Wheel)
    0x05, 0x01,                    // Usage Page (Generic Desktop)
    0x09, 0x02,                    // Usage (Mouse)
    0xa1, 0x01,                    // Collection (Application)
    0x85, 0x03,                    //   Report ID (3)
    0x09, 0x01,                    //   Usage (Pointer)
    0xa1, 0x00,                    //   Collection (Physical)
    0x05, 0x09,                    //     Usage Page (Button)
    0x19, 0x01,                    //     Usage Minimum (Button 1)
    0x29, 0x03,                    //     Usage Maximum (Button 3: Left, Right, Middle)
    0x15, 0x00,                    //     Logical Minimum (0)
    0x25, 0x01,                    //     Logical Maximum (1)
    0x95, 0x03,                    //     Report Count (3)
    0x75, 0x01,                    //     Report Size (1)
    0x81, 0x02,                    //     Input (Data, Variable, Absolute) - 3 Buttons
    0x95, 0x01,                    //     Report Count (1)
    0x75, 0x05,                    //     Report Size (5)
    0x81, 0x03,                    //     Input (Constant, Variable, Absolute) - Padding
    0x05, 0x01,                    //     Usage Page (Generic Desktop)
    0x09, 0x30,                    //     Usage (X)
    0x09, 0x31,                    //     Usage (Y)
    0x09, 0x38,                    //     Usage (Wheel)
    0x15, 0x81,                    //     Logical Minimum (-127)
    0x25, 0x7f,                    //     Logical Maximum (127)
    0x75, 0x08,                    //     Report Size (8)
    0x95, 0x03,                    //     Report Count (3)
    0x81, 0x06,                    //     Input (Data, Variable, Relative) - dX, dY, Wheel
    0xc0,                          //   End Collection (Physical)
    0xc0                           // End Collection (Application)
};

static char g_last_btn[64] = "";
static time_t g_last_seen = 0;
static volatile sig_atomic_t g_running = 1;

void log_msg(const char *msg) {
    struct stat lst;
    if (stat(LOG_FILE, &lst) == 0 && lst.st_size > LOG_MAX_BYTES) {
        FILE *tf = fopen(LOG_FILE, "w");
        if (tf) { fprintf(tf, "[truncated]\n"); fclose(tf); }
    }
    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        char tmbuf[32];
        strftime(tmbuf, sizeof(tmbuf), "%Y-%m-%d %H:%M:%S", tm_info);
        fprintf(f, "%s %s\n", tmbuf, msg);
        fclose(f);
    }
}

void codex_log_debug(const char *msg) {
    if (is_debug_log_enabled()) {
        log_msg(msg);
    }
}

static int parse_bd_addr(const char *str, bd_addr_t addr) {
    int b[6];
    if (sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
        for (int i = 0; i < 6; i++) addr[i] = (uint8_t)b[i];
        return 1;
    }
    return 0;
}

static int is_null_bd_addr(const bd_addr_t addr) {
    return addr[0] == 0 && addr[1] == 0 && addr[2] == 0 && addr[3] == 0 && addr[4] == 0 && addr[5] == 0;
}

static uint16_t get_target_host_hid_cid(void) {
    if (g_active_host_idx >= 0 && g_active_host_idx < MAX_HOST_CONNS) {
        if (g_hosts[g_active_host_idx].connected && g_hosts[g_active_host_idx].hid_cid) {
            return g_hosts[g_active_host_idx].hid_cid;
        }
    }
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (g_hosts[i].connected && g_hosts[i].hid_cid) {
            return g_hosts[i].hid_cid;
        }
    }
    return 0;
}

static bool is_any_host_connected(void) {
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (g_hosts[i].connected && g_hosts[i].hid_cid) return true;
    }
    return false;
}

static host_conn_t * register_host_target(const bd_addr_t addr, const char *name) {
    if (is_null_bd_addr(addr)) return NULL;
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (memcmp(g_hosts[i].addr, addr, 6) == 0) {
            if (name && name[0] && (!g_hosts[i].name[0] || strcmp(g_hosts[i].name, "Harmony Keyboard") == 0)) {
                strncpy(g_hosts[i].name, name, sizeof(g_hosts[i].name) - 1);
            }
            return &g_hosts[i];
        }
    }
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (is_null_bd_addr(g_hosts[i].addr)) {
            memcpy(g_hosts[i].addr, addr, 6);
            if (name && name[0]) {
                strncpy(g_hosts[i].name, name, sizeof(g_hosts[i].name) - 1);
            } else {
                snprintf(g_hosts[i].name, sizeof(g_hosts[i].name), "Host %s", bd_addr_to_str(addr));
            }
            return &g_hosts[i];
        }
    }
    return NULL;
}

static void update_bthid_status(void);

static void update_status(const char *state_str) {
    FILE *f = fopen(STATUS_FILE, "w");
    if (f) {
        fprintf(f, "{\n");
        fprintf(f, "  \"backend\": \"btstack\",\n");
        fprintf(f, "  \"pid\": %d,\n", (int)getpid());
        fprintf(f, "  \"state\": \"%s\",\n", state_str);
        fprintf(f, "  \"target\": \"%s\",\n", bd_addr_to_str(remote_device.addr));
        fprintf(f, "  \"last_button\": \"%s\",\n", g_last_btn);
        fprintf(f, "  \"last_seen\": %ld,\n", (long)g_last_seen);
        fprintf(f, "  \"remote\": {\n");
        fprintf(f, "    \"connected\": %s,\n", (connection_handle != HCI_CON_HANDLE_INVALID) ? "true" : "false");
        fprintf(f, "    \"addr\": \"%s\"\n", bd_addr_to_str(remote_device.addr));
        fprintf(f, "  },\n");
        fprintf(f, "  \"host\": {\n");
        uint16_t active_cid = get_target_host_hid_cid();
        host_conn_t *ah = (g_active_host_idx >= 0 && g_active_host_idx < MAX_HOST_CONNS) ? &g_hosts[g_active_host_idx] : NULL;
        bool host_conn = (active_cid != 0);
        fprintf(f, "    \"connected\": %s,\n", host_conn ? "true" : "false");
        fprintf(f, "    \"addr\": \"%s\",\n", ah ? bd_addr_to_str(ah->addr) : (g_host_connected ? bd_addr_to_str(g_host_addr) : ""));
        fprintf(f, "    \"name\": \"%s\",\n", (ah && ah->name[0]) ? ah->name : g_host_name);
        fprintf(f, "    \"pairing\": %s\n", g_host_pairing ? "true" : "false");
        fprintf(f, "  },\n");
        fprintf(f, "  \"hosts\": [\n");
        bool first = true;
        for (int i = 0; i < MAX_HOST_CONNS; i++) {
            if (is_null_bd_addr(g_hosts[i].addr)) continue;
            if (!first) fprintf(f, ",\n");
            first = false;
            fprintf(f, "    {\"addr\": \"%s\", \"name\": \"%s\", \"connected\": %s, \"cid\": %u}",
                    bd_addr_to_str(g_hosts[i].addr), g_hosts[i].name,
                    g_hosts[i].connected ? "true" : "false", g_hosts[i].hid_cid);
        }
        fprintf(f, "\n  ]\n");
        fprintf(f, "}\n");
        fclose(f);
    }
    update_bthid_status();
}

static void update_bthid_status(void) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddBoolToObject(root, "ok", 1);
    cJSON_AddBoolToObject(root, "runtime", 1);
    cJSON_AddNumberToObject(root, "pid", (double)getpid());
    cJSON_AddNumberToObject(root, "updated", (double)time(NULL));
    bool any_conn = is_any_host_connected();
    cJSON_AddStringToObject(root, "state", any_conn ? "listening" : (g_host_pairing ? "pairing" : "waiting_host"));
    host_conn_t *ah = (g_active_host_idx >= 0 && g_active_host_idx < MAX_HOST_CONNS) ? &g_hosts[g_active_host_idx] : NULL;
    cJSON_AddStringToObject(root, "target", ah ? bd_addr_to_str(ah->addr) : (any_conn ? bd_addr_to_str(g_host_addr) : ""));
    cJSON_AddNumberToObject(root, "sent", (double)g_keys_sent);
    cJSON_AddNumberToObject(root, "skipped", 0);
    cJSON_AddStringToObject(root, "error", "");

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return;

    FILE *f = fopen(BTHID_STATUS_FILE ".new", "w");
    if (f) {
        fputs(out, f);
        fputc('\n', f);
        fclose(f);
        rename(BTHID_STATUS_FILE ".new", BTHID_STATUS_FILE);
    }
    free(out);
}

static int get_pinned_target(bd_addr_t addr) {
    FILE *f = fopen(TARGET_FILE, "r");
    if (!f) f = fopen(MAP_FILE, "r");
    if (!f) return 0;
    char buf[1024];
    size_t rn = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[rn] = 0;
    if (parse_bd_addr(buf, addr)) return 1;
    char *p = strstr(buf, "bdaddr");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            char mac[32] = "";
            int i = 0;
            while (*p && i < 20) {
                if ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F') || *p == ':') {
                    mac[i++] = *p;
                } else if (i > 0) {
                    break;
                }
                p++;
            }
            mac[i] = 0;
            if (parse_bd_addr(mac, addr)) return 1;
        }
    }
    return 0;
}

static int get_pinned_host_target(bd_addr_t addr) {
    const char *files[] = {"/data/codex/bthid_host_target", "/data/codex/bt-devices.json", "/data/codex/config/devices.json", "/data/codex/devices.json", NULL};
    for (int fi = 0; files[fi]; fi++) {
        FILE *f = fopen(files[fi], "r");
        if (!f) continue;
        char buf[4096];
        size_t rn = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[rn] = 0;
        for (char *p = buf; *p; p++) {
            if (isxdigit((unsigned char)*p) && p[2] == ':' && p[5] == ':' && p[8] == ':' && p[11] == ':' && p[14] == ':') {
                char mac[18];
                strncpy(mac, p, 17);
                mac[17] = 0;
                if (parse_bd_addr(mac, addr) && !is_null_bd_addr(addr)) return 1;
            }
        }
    }
    return 0;
}

static void link_bt_device_to_addr(const char *dev_id, const char *addr_str) {
    if (!dev_id || !dev_id[0] || !addr_str || !addr_str[0]) return;
    char msg[128];
    snprintf(msg, sizeof(msg), "Linking device %s to Bluetooth address %s", dev_id, addr_str);
    log_msg(msg);

    FILE *bf = fopen("/data/codex/bt-devices.json", "r");
    cJSON *root = NULL;
    if (bf) {
        fseek(bf, 0, SEEK_END);
        long sz = ftell(bf);
        fseek(bf, 0, SEEK_SET);
        if (sz > 0 && sz < 65536) {
            char *buf = (char *)malloc(sz + 1);
            if (buf) {
                size_t r = fread(buf, 1, sz, bf);
                buf[r] = 0;
                root = cJSON_Parse(buf);
                free(buf);
            }
        }
        fclose(bf);
    }
    if (!root) {
        root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "version", 1);
        cJSON_AddItemToObject(root, "devices", cJSON_CreateArray());
    }
    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (!devs || !cJSON_IsArray(devs)) {
        cJSON_DeleteItemFromObject(root, "devices");
        devs = cJSON_CreateArray();
        cJSON_AddItemToObject(root, "devices", devs);
    }

    cJSON *found = NULL;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, devs) {
        cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (jid && cJSON_IsString(jid) && strcmp(jid->valuestring, dev_id) == 0) {
            found = item;
            break;
        }
    }
    if (found) {
        cJSON_ReplaceItemInObject(found, "bdaddr", cJSON_CreateString(addr_str));
    } else {
        cJSON *new_d = cJSON_CreateObject();
        cJSON_AddStringToObject(new_d, "id", dev_id);
        cJSON_AddStringToObject(new_d, "name", dev_id);
        cJSON_AddStringToObject(new_d, "type", "btkeyboard");
        cJSON_AddStringToObject(new_d, "bdaddr", addr_str);
        cJSON_AddItemToObject(new_d, "commands", cJSON_CreateArray());
        cJSON_AddItemToArray(devs, new_d);
    }

    char *out = cJSON_Print(root);
    cJSON_Delete(root);
    if (out) {
        FILE *wf = fopen("/data/codex/bt-devices.json.tmp", "w");
        if (wf) {
            fputs(out, wf);
            fputc('\n', wf);
            fclose(wf);
            rename("/data/codex/bt-devices.json.tmp", "/data/codex/bt-devices.json");
        }
        free(out);
    }
}

static const char *get_default_host_name(void) {
    static char s_host_name[64] = "";
    s_host_name[0] = '\0';
    FILE *f = fopen("/data/codex/config/devices.json", "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz > 0 && sz < 200000) {
        char *buf = (char *)malloc((size_t)sz + 1);
        if (buf) {
            size_t r = fread(buf, 1, sz, f);
            buf[r] = 0;
            cJSON *root = cJSON_Parse(buf);
            free(buf);
            if (root) {
                cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
                if (!devs) devs = cJSON_GetObjectItemCaseSensitive(root, "DevicesWithFeatures");
                if (devs && cJSON_IsArray(devs)) {
                    cJSON *di = NULL;
                    cJSON_ArrayForEach(di, devs) {
                        cJSON *d = cJSON_GetObjectItemCaseSensitive(di, "Device");
                        if (!d) d = di;
                        cJSON *t = cJSON_GetObjectItemCaseSensitive(d, "type");
                        cJSON *n = cJSON_GetObjectItemCaseSensitive(d, "name");
                        cJSON *tr = cJSON_GetObjectItemCaseSensitive(d, "transport");
                        const char *tstr = (t && t->valuestring) ? t->valuestring : "";
                        const char *nstr = (n && n->valuestring) ? n->valuestring : "";
                        int transport = (tr && cJSON_IsNumber(tr)) ? tr->valueint : 0;
                        if (transport == 32 || strcasestr(tstr, "game") || strcasestr(tstr, "bt") || strcasestr(tstr, "bluetooth")) {
                            if (nstr[0]) {
                                strncpy(s_host_name, nstr, sizeof(s_host_name) - 1);
                                s_host_name[sizeof(s_host_name) - 1] = 0;
                                cJSON_Delete(root);
                                fclose(f);
                                return s_host_name;
                            }
                        }
                    }
                }
                cJSON_Delete(root);
            }
        }
    }
    fclose(f);
    return s_host_name[0] ? s_host_name : NULL;
}

static void init_known_hosts(void) {
    bd_addr_t def_target;
    if (get_pinned_host_target(def_target)) {
        register_host_target(def_target, get_default_host_name());
    }

    FILE *bf = fopen("/data/codex/bt-devices.json", "r");
    if (bf) {
        fseek(bf, 0, SEEK_END);
        long sz = ftell(bf);
        fseek(bf, 0, SEEK_SET);
        if (sz > 0 && sz < 65536) {
            char *buf = (char *)malloc(sz + 1);
            if (buf) {
                size_t r = fread(buf, 1, sz, bf);
                buf[r] = 0;
                cJSON *root = cJSON_Parse(buf);
                free(buf);
                if (root) {
                    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
                    if (devs && cJSON_IsArray(devs)) {
                        cJSON *item = NULL;
                        cJSON_ArrayForEach(item, devs) {
                            cJSON *ja = cJSON_GetObjectItemCaseSensitive(item, "bdaddr");
                            cJSON *jn = cJSON_GetObjectItemCaseSensitive(item, "name");
                            if (ja && cJSON_IsString(ja) && ja->valuestring[0]) {
                                bd_addr_t ba;
                                if (parse_bd_addr(ja->valuestring, ba)) {
                                    const char *nm = (jn && cJSON_IsString(jn)) ? jn->valuestring : NULL;
                                    register_host_target(ba, nm);
                                }
                            }
                        }
                    }
                    cJSON_Delete(root);
                }
            }
        }
        fclose(bf);
    }

    const char *dpath = "/data/codex/config/devices.json";
    if (access(dpath, F_OK) != 0) dpath = "/data/codex/devices.json";
    FILE *df = fopen(dpath, "r");
    if (df) {
        fseek(df, 0, SEEK_END);
        long sz = ftell(df);
        fseek(df, 0, SEEK_SET);
        if (sz > 0 && sz < 1000000) {
            char *buf = (char *)malloc(sz + 1);
            if (buf) {
                size_t r = fread(buf, 1, sz, df);
                buf[r] = 0;
                cJSON *root = cJSON_Parse(buf);
                free(buf);
                if (root) {
                    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
                    if (devs && cJSON_IsArray(devs)) {
                        cJSON *item = NULL;
                        cJSON_ArrayForEach(item, devs) {
                            cJSON *ja = cJSON_GetObjectItemCaseSensitive(item, "bdaddr");
                            if (!ja) ja = cJSON_GetObjectItemCaseSensitive(item, "BTAddress");
                            cJSON *jn = cJSON_GetObjectItemCaseSensitive(item, "name");
                            if (!jn) jn = cJSON_GetObjectItemCaseSensitive(item, "Name");
                            if (ja && cJSON_IsString(ja) && ja->valuestring[0]) {
                                bd_addr_t ba;
                                if (parse_bd_addr(ja->valuestring, ba)) {
                                    const char *nm = (jn && cJSON_IsString(jn)) ? jn->valuestring : NULL;
                                    register_host_target(ba, nm);
                                }
                            }
                        }
                    } else {
                        cJSON *dwf = cJSON_GetObjectItemCaseSensitive(root, "DevicesWithFeatures");
                        if (dwf && cJSON_IsArray(dwf)) {
                            cJSON *item = NULL;
                            cJSON_ArrayForEach(item, dwf) {
                                cJSON *dev = cJSON_GetObjectItemCaseSensitive(item, "Device");
                                if (!dev) continue;
                                cJSON *ja = cJSON_GetObjectItemCaseSensitive(dev, "BTAddress");
                                cJSON *jn = cJSON_GetObjectItemCaseSensitive(dev, "Name");
                                if (ja && cJSON_IsString(ja) && ja->valuestring[0]) {
                                    bd_addr_t ba;
                                    if (parse_bd_addr(ja->valuestring, ba)) {
                                        const char *nm = (jn && cJSON_IsString(jn)) ? jn->valuestring : NULL;
                                        register_host_target(ba, nm);
                                    }
                                }
                            }
                        }
                    }
                    cJSON_Delete(root);
                }
            }
        }
        fclose(df);
    }
}


static void try_connect_host_addr(const bd_addr_t addr) {
    if (is_null_bd_addr(addr)) return;
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (g_hosts[i].connected && g_hosts[i].hid_cid != 0 && memcmp(g_hosts[i].addr, addr, 6) == 0) {
            return;
        }
    }
    // Prevent flooding: do not issue another connect while one is already pending within 15s
    if (g_host_connecting && (time(NULL) - g_host_connecting_start) < 15) {
        return;
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "Initiating connection to Classic HID Host: %s", bd_addr_to_str(addr));
    log_msg(msg);
    g_host_connecting = true;
    g_host_connecting_start = time(NULL);
    uint16_t cid = 0;
    uint8_t status = hid_device_connect((uint8_t*)addr, &cid);
    if (status != ERROR_CODE_SUCCESS || cid == 0) {
        g_host_connecting = false;
        snprintf(msg, sizeof(msg), "Classic HID Host connect request failed (status=0x%02x, cid=0x%04x)", status, cid);
        log_msg(msg);
        return;
    }
    host_conn_t *h = register_host_target(addr, NULL);
    if (h) {
        h->hid_cid = cid;
    }
}

static void try_connect_host(void) {
    bool found = false;
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (!is_null_bd_addr(g_hosts[i].addr)) {
            found = true;
            if (!g_hosts[i].connected) {
                try_connect_host_addr(g_hosts[i].addr);
            }
        }
    }
    if (!found) {
        if (!is_null_bd_addr(g_host_addr)) {
            try_connect_host_addr(g_host_addr);
        } else {
            bd_addr_t target;
            if (get_pinned_host_target(target)) {
                register_host_target(target, get_default_host_name());
                try_connect_host_addr(target);
            }
        }
    }
}

static void host_reconnect_timer_handler(btstack_timer_source_t *ts) {
    UNUSED(ts);
    if (!g_host_reconnect_active) return;

    if (is_any_host_connected()) {
        log_msg("Host reconnect: Host connected, stopping retry loop");
        g_host_reconnect_active = false;
        g_host_connecting = false;
        return;
    }

    if (g_host_reconnect_attempts >= MAX_HOST_RECONNECT_ATTEMPTS) {
        log_msg("Host reconnect: Maximum retry attempts reached");
        g_host_reconnect_active = false;
        g_host_connecting = false;
        return;
    }

    // If an in-flight connect attempt is still paging, wait before issuing another
    if (g_host_connecting && (time(NULL) - g_host_connecting_start) < 15) {
        btstack_run_loop_set_timer(&g_host_reconnect_timer, 4000);
        btstack_run_loop_add_timer(&g_host_reconnect_timer);
        return;
    }

    g_host_reconnect_attempts++;
    char msg[128];
    snprintf(msg, sizeof(msg), "Host reconnect: Attempt %d/%d to %s",
             g_host_reconnect_attempts, MAX_HOST_RECONNECT_ATTEMPTS,
             is_null_bd_addr(g_host_reconnect_target) ? "targets" : bd_addr_to_str(g_host_reconnect_target));
    log_msg(msg);

    if (!is_null_bd_addr(g_host_reconnect_target)) {
        try_connect_host_addr(g_host_reconnect_target);
    } else {
        try_connect_host();
    }

    btstack_run_loop_set_timer(&g_host_reconnect_timer, 7000);
    btstack_run_loop_add_timer(&g_host_reconnect_timer);
}

static void start_host_reconnect(const bd_addr_t target, int max_attempts) {
    (void)max_attempts;
    if (is_any_host_connected()) {
        log_msg("Host reconnect: Host already connected");
        return;
    }

    // If reconnect loop is already running for this target with in-flight connect, do not double-fire
    if (g_host_reconnect_active && !is_null_bd_addr(target) && memcmp(g_host_reconnect_target, target, 6) == 0 && g_host_connecting) {
        return;
    }

    if (!is_null_bd_addr(target)) {
        memcpy(g_host_reconnect_target, target, 6);
    } else {
        memset(g_host_reconnect_target, 0, 6);
    }
    g_host_reconnect_attempts = 0;
    g_host_reconnect_active = true;

    char msg[128];
    snprintf(msg, sizeof(msg), "Starting host reconnect loop for %s (105s window, 7s interval)",
             is_null_bd_addr(g_host_reconnect_target) ? "targets" : bd_addr_to_str(g_host_reconnect_target));
    log_msg(msg);

    if (!is_null_bd_addr(g_host_reconnect_target)) {
        try_connect_host_addr(g_host_reconnect_target);
    } else {
        try_connect_host();
    }

    btstack_run_loop_remove_timer(&g_host_reconnect_timer);
    btstack_run_loop_set_timer(&g_host_reconnect_timer, 7000);
    btstack_run_loop_set_timer_handler(&g_host_reconnect_timer, host_reconnect_timer_handler);
    btstack_run_loop_add_timer(&g_host_reconnect_timer);
}

static void stop_host_reconnect(void) {
    if (g_host_reconnect_active) {
        g_host_reconnect_active = false;
        btstack_run_loop_remove_timer(&g_host_reconnect_timer);
        log_msg("Host reconnect: Loop stopped");
    }
    g_host_connecting = false;
}

static void hog_connect(void);
static void hog_start_scan(void);

static void hog_reconnect_timeout(btstack_timer_source_t *ts) {
    UNUSED(ts);
    if (app_state == APP_READY) return;
    if (app_state == APP_CONNECTING) {
        log_msg("Connection attempt timed out (10s); cancelling and resuming scan");
        gap_connect_cancel();
        hog_start_scan();
        return;
    }
    log_msg("Reconnect timer fired: starting scan");
    hog_start_scan();
}

static void schedule_reconnect(uint32_t delay_ms) {
    btstack_run_loop_remove_timer(&reconnect_timer);
    btstack_run_loop_set_timer(&reconnect_timer, delay_ms);
    btstack_run_loop_set_timer_handler(&reconnect_timer, &hog_reconnect_timeout);
    btstack_run_loop_add_timer(&reconnect_timer);
}

typedef struct {
    bd_addr_t addr;
    uint8_t addr_type;
    char name[64];
    int8_t rssi;
} discovered_remote_t;

#define MAX_DISCOVERED 16
static discovered_remote_t g_discovered[MAX_DISCOVERED];
static int g_discovered_count = 0;
static bool g_user_scan_active = false;
static time_t g_user_scan_end = 0;
static btstack_timer_source_t cmd_poll_timer;

static void write_scan_results(bool scanning) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddBoolToObject(root, "ok", 1);
    cJSON_AddBoolToObject(root, "scanning", scanning);
    cJSON *devs = cJSON_CreateArray();
    for (int i = 0; i < g_discovered_count; i++) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "addr", bd_addr_to_str(g_discovered[i].addr));
        cJSON_AddStringToObject(d, "name", g_discovered[i].name[0] ? g_discovered[i].name : "Bluetooth Remote");
        cJSON_AddNumberToObject(d, "rssi", g_discovered[i].rssi);
        cJSON_AddBoolToObject(d, "pairing", 1);
        cJSON_AddItemToArray(devs, d);
    }
    cJSON_AddItemToObject(root, "devices", devs);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return;

    FILE *f = fopen(SCAN_RESULT_FILE ".tmp", "w");
    if (f) {
        fputs(out, f);
        fputc('\n', f);
        fclose(f);
        rename(SCAN_RESULT_FILE ".tmp", SCAN_RESULT_FILE);
    }
    free(out);
}

static void hog_start_scan(void) {
    bd_addr_t pinned;
    if (get_pinned_target(pinned)) {
        update_status("standby");
        gap_set_scan_parameters(0, 48, 48); // passive scanning: no SCAN_REQ delay/collision
    } else {
        update_status("scanning");
        gap_set_scan_parameters(1, 48, 48); // active scanning for discovery
    }
    app_state = APP_SCANNING;
    gap_start_scan();
}

static void hog_connect(void) {
    log_msg("Connecting to target BLE remote...");
    app_state = APP_CONNECTING;
    update_status("connecting");
    gap_stop_scan();
    gap_connect(remote_device.addr, remote_device.addr_type);
    schedule_reconnect(10000); // 10s connection timeout
}

static void hog_start_connect(void) {
    // Always start scan first to discover current address type and presence
    hog_start_scan();
}

static void start_user_scan(void) {
    log_msg("User requested BLE scan for remotes in pairing mode...");
    g_discovered_count = 0;
    memset(g_discovered, 0, sizeof(g_discovered));
    g_user_scan_active = true;
    g_user_scan_end = time(NULL) + 8;

    btstack_run_loop_remove_timer(&reconnect_timer);
    if (connection_handle != HCI_CON_HANDLE_INVALID) {
        gap_disconnect(connection_handle);
    }
    write_scan_results(true);

    app_state = APP_SCANNING;
    update_status("scanning");
    gap_set_scan_parameters(1, 48, 48);
    gap_start_scan();
}

static void start_user_pair(const char *addr_str) {
    bd_addr_t new_addr;
    if (!parse_bd_addr(addr_str, new_addr)) {
        log_msg("Invalid pair address requested");
        return;
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "User pair requested for %s", addr_str);
    log_msg(msg);

    FILE *tf = fopen(TARGET_FILE, "w");
    if (tf) {
        fprintf(tf, "%s\n", addr_str);
        fclose(tf);
    }

    g_user_scan_active = false;
    btstack_run_loop_remove_timer(&reconnect_timer);
    if (connection_handle != HCI_CON_HANDLE_INVALID) {
        gap_disconnect(connection_handle);
    }
    gap_stop_scan();

    gap_delete_bonding(BD_ADDR_TYPE_LE_PUBLIC, new_addr);
    gap_delete_bonding(BD_ADDR_TYPE_LE_RANDOM, new_addr);

    memcpy(remote_device.addr, new_addr, 6);
    remote_device.addr_type = BD_ADDR_TYPE_LE_PUBLIC;
    for (int i = 0; i < g_discovered_count; i++) {
        if (memcmp(g_discovered[i].addr, new_addr, 6) == 0) {
            remote_device.addr_type = (bd_addr_type_t)g_discovered[i].addr_type;
            break;
        }
    }

    app_state = APP_SCANNING;
    update_status("pairing");
    gap_set_scan_parameters(1, 48, 48);
    gap_start_scan();
}

static btstack_timer_source_t g_host_key_release_timer;
static uint8_t g_host_pending_release_type = 0; // 1 = keyboard, 2 = consumer, 3 = custom
static uint8_t g_host_custom_release_buf[16];
static int     g_host_custom_release_len = 0;
static int     g_host_send_hold_mode = 0;
static uint8_t g_host_mouse_buttons = 0;

static void host_key_release_timer_handler(btstack_timer_source_t *ts) {
    UNUSED(ts);
    uint16_t cid = get_target_host_hid_cid();
    if (cid == 0) return;
    if (g_host_pending_release_type == 1) {
        uint8_t release[10] = {0xa1, 0x01, 0, 0, 0, 0, 0, 0, 0, 0};
        hid_device_send_interrupt_message(cid, release, sizeof(release));
    } else if (g_host_pending_release_type == 2) {
        uint8_t release[4] = {0xa1, 0x02, 0, 0};
        hid_device_send_interrupt_message(cid, release, sizeof(release));
    } else if (g_host_pending_release_type == 3 && g_host_custom_release_len > 0) {
        hid_device_send_interrupt_message(cid, g_host_custom_release_buf, g_host_custom_release_len);
    } else if (g_host_pending_release_type == 4) {
        uint8_t mrelease[6] = {0xa1, 0x03, 0, 0, 0, 0};
        hid_device_send_interrupt_message(cid, mrelease, sizeof(mrelease));
        g_host_mouse_buttons = 0;
    }
    g_host_pending_release_type = 0;
}

static btstack_timer_source_t g_host_crelease_timer;

static void host_crelease_timer_handler(btstack_timer_source_t *ts) {
    UNUSED(ts);
    uint16_t cid = get_target_host_hid_cid();
    if (cid != 0) {
        uint8_t crelease[4] = {0xa1, 0x02, 0, 0};
        hid_device_send_interrupt_message(cid, crelease, sizeof(crelease));
    }
}

static void send_host_release_all(void) {
    uint8_t prev_type = g_host_pending_release_type;
    if (g_host_pending_release_type != 0) {
        btstack_run_loop_remove_timer(&g_host_key_release_timer);
        g_host_pending_release_type = 0;
    }
    if (!is_any_host_connected()) return;

    if (prev_type == 2) {
        uint8_t crelease[4] = {0xa1, 0x02, 0, 0};
        for (int i = 0; i < MAX_HOST_CONNS; i++) {
            if (g_hosts[i].connected && g_hosts[i].hid_cid) {
                hid_device_send_interrupt_message(g_hosts[i].hid_cid, crelease, sizeof(crelease));
            }
        }
    } else if (prev_type == 1) {
        uint8_t release[10] = {0xa1, 0x01, 0, 0, 0, 0, 0, 0, 0, 0};
        for (int i = 0; i < MAX_HOST_CONNS; i++) {
            if (g_hosts[i].connected && g_hosts[i].hid_cid) {
                hid_device_send_interrupt_message(g_hosts[i].hid_cid, release, sizeof(release));
            }
        }
    } else if (prev_type == 3 && g_host_custom_release_len > 0) {
        for (int i = 0; i < MAX_HOST_CONNS; i++) {
            if (g_hosts[i].connected && g_hosts[i].hid_cid) {
                hid_device_send_interrupt_message(g_hosts[i].hid_cid, g_host_custom_release_buf, g_host_custom_release_len);
            }
        }
    } else if (prev_type == 4 || g_host_mouse_buttons != 0) {
        uint8_t mrelease[6] = {0xa1, 0x03, 0, 0, 0, 0};
        for (int i = 0; i < MAX_HOST_CONNS; i++) {
            if (g_hosts[i].connected && g_hosts[i].hid_cid) {
                hid_device_send_interrupt_message(g_hosts[i].hid_cid, mrelease, sizeof(mrelease));
            }
        }
        g_host_mouse_buttons = 0;
    } else {
        uint8_t release[10] = {0xa1, 0x01, 0, 0, 0, 0, 0, 0, 0, 0};
        for (int i = 0; i < MAX_HOST_CONNS; i++) {
            if (g_hosts[i].connected && g_hosts[i].hid_cid) {
                hid_device_send_interrupt_message(g_hosts[i].hid_cid, release, sizeof(release));
            }
        }
        btstack_run_loop_remove_timer(&g_host_crelease_timer);
        btstack_run_loop_set_timer_handler(&g_host_crelease_timer, host_crelease_timer_handler);
        btstack_run_loop_set_timer(&g_host_crelease_timer, 20);
        btstack_run_loop_add_timer(&g_host_crelease_timer);
    }
    if (g_host_mouse_buttons != 0) {
        uint8_t mrelease[6] = {0xa1, 0x03, 0, 0, 0, 0};
        for (int i = 0; i < MAX_HOST_CONNS; i++) {
            if (g_hosts[i].connected && g_hosts[i].hid_cid) {
                hid_device_send_interrupt_message(g_hosts[i].hid_cid, mrelease, sizeof(mrelease));
            }
        }
        g_host_mouse_buttons = 0;
    }
}

static void send_host_mouse_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel) {
    uint16_t cid = get_target_host_hid_cid();
    if (cid == 0) {
        if (!g_host_reconnect_active) {
            bd_addr_t tgt;
            if (g_active_host_idx >= 0 && g_active_host_idx < MAX_HOST_CONNS && !is_null_bd_addr(g_hosts[g_active_host_idx].addr)) {
                start_host_reconnect(g_hosts[g_active_host_idx].addr, 20);
            } else if (get_pinned_host_target(tgt)) {
                start_host_reconnect(tgt, 20);
            } else {
                try_connect_host();
            }
        }
        return;
    }
    uint8_t report[6] = {0xa1, 0x03, buttons, (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel};
    hid_device_send_interrupt_message(cid, report, sizeof(report));
    g_keys_sent++;
    update_bthid_status();
}

static void send_host_mouse_click(uint8_t button) {
    uint16_t cid = get_target_host_hid_cid();
    if (cid == 0) return;
    if (g_host_pending_release_type != 0) {
        btstack_run_loop_remove_timer(&g_host_key_release_timer);
        host_key_release_timer_handler(NULL);
    }
    g_host_mouse_buttons |= button;
    send_host_mouse_report(g_host_mouse_buttons, 0, 0, 0);
    g_host_pending_release_type = 4;
    btstack_run_loop_remove_timer(&g_host_key_release_timer);
    btstack_run_loop_set_timer_handler(&g_host_key_release_timer, host_key_release_timer_handler);
    btstack_run_loop_set_timer(&g_host_key_release_timer, 40);
    btstack_run_loop_add_timer(&g_host_key_release_timer);
}

static void send_host_keyboard_report(uint8_t modifier, uint8_t keycode) {
    uint16_t cid = get_target_host_hid_cid();
    if (cid == 0) {
        if (!g_pending_wake_btn[0]) {
            snprintf(g_pending_wake_btn, sizeof(g_pending_wake_btn), "KEY %02x %02x", modifier, keycode);
            g_pending_wake_time = time(NULL);
        }
        if (!g_host_reconnect_active) {
            bd_addr_t tgt;
            if (g_active_host_idx >= 0 && g_active_host_idx < MAX_HOST_CONNS && !is_null_bd_addr(g_hosts[g_active_host_idx].addr)) {
                start_host_reconnect(g_hosts[g_active_host_idx].addr, 20);
            } else if (get_pinned_host_target(tgt)) {
                start_host_reconnect(tgt, 20);
            } else {
                try_connect_host();
            }
        }
        return;
    }
    if (g_host_pending_release_type != 0) {
        btstack_run_loop_remove_timer(&g_host_key_release_timer);
        host_key_release_timer_handler(NULL);
    }
    uint8_t press[10] = {0xa1, 0x01, modifier, 0, keycode, 0, 0, 0, 0, 0};
    hid_device_send_interrupt_message(cid, press, sizeof(press));
    g_host_pending_release_type = 1;
    btstack_run_loop_remove_timer(&g_host_key_release_timer);
    btstack_run_loop_set_timer_handler(&g_host_key_release_timer, host_key_release_timer_handler);
    btstack_run_loop_set_timer(&g_host_key_release_timer, g_host_send_hold_mode ? 10000 : 40);
    btstack_run_loop_add_timer(&g_host_key_release_timer);
    g_keys_sent++;
    update_bthid_status();
}

static void send_host_consumer_report(uint16_t usage_id) {
    uint16_t cid = get_target_host_hid_cid();
    if (cid == 0) {
        if (!g_pending_wake_btn[0]) {
            snprintf(g_pending_wake_btn, sizeof(g_pending_wake_btn), "CONSUMER %04x", usage_id);
            g_pending_wake_time = time(NULL);
        }
        if (!g_host_reconnect_active) {
            bd_addr_t tgt;
            if (g_active_host_idx >= 0 && g_active_host_idx < MAX_HOST_CONNS && !is_null_bd_addr(g_hosts[g_active_host_idx].addr)) {
                start_host_reconnect(g_hosts[g_active_host_idx].addr, 20);
            } else if (get_pinned_host_target(tgt)) {
                start_host_reconnect(tgt, 20);
            } else {
                try_connect_host();
            }
        }
        return;
    }
    if (g_host_pending_release_type != 0) {
        btstack_run_loop_remove_timer(&g_host_key_release_timer);
        host_key_release_timer_handler(NULL);
    }
    uint8_t press[4] = {0xa1, 0x02, (uint8_t)(usage_id & 0xff), (uint8_t)((usage_id >> 8) & 0xff)};
    hid_device_send_interrupt_message(cid, press, sizeof(press));
    g_host_pending_release_type = 2;
    btstack_run_loop_remove_timer(&g_host_key_release_timer);
    btstack_run_loop_set_timer_handler(&g_host_key_release_timer, host_key_release_timer_handler);
    btstack_run_loop_set_timer(&g_host_key_release_timer, g_host_send_hold_mode ? 10000 : 40);
    btstack_run_loop_add_timer(&g_host_key_release_timer);
    g_keys_sent++;
    update_bthid_status();
}

static int parse_hex_bytes(const char *hex, uint8_t *out, int max_len) {
    int len = 0;
    while (*hex && len < max_len) {
        while (*hex == ' ') hex++;
        if (!*hex || *hex == '|' || *hex == '\r' || *hex == '\n') break;
        unsigned int b = 0;
        if (sscanf(hex, "%02x", &b) == 1 || sscanf(hex, "%02X", &b) == 1) {
            out[len++] = (uint8_t)b;
            hex += 2;
        } else {
            break;
        }
    }
    return len;
}

static bool send_host_named_key(const char *name) {
    if (!name || !name[0]) return false;
    char msg[128];
    snprintf(msg, sizeof(msg), "Host key send: %s", name);
    codex_log_debug(msg);

    if (strcasecmp(name, "release_all") == 0 || strcasecmp(name, "release") == 0) {
        send_host_release_all();
        return true;
    }

    uint8_t mod = 0;
    const char *p = name;
    while (*p) {
        if (strncasecmp(p, "ctrl+", 5) == 0) { mod |= 0x01; p += 5; }
        else if (strncasecmp(p, "ctrl", 4) == 0 && strlen(p) > 4) { mod |= 0x01; p += 4; }
        else if (strncasecmp(p, "shift+", 6) == 0) { mod |= 0x02; p += 6; }
        else if (strncasecmp(p, "shift", 5) == 0 && strlen(p) > 5) { mod |= 0x02; p += 5; }
        else if (strncasecmp(p, "alt+", 4) == 0) { mod |= 0x04; p += 4; }
        else if (strncasecmp(p, "alt", 3) == 0 && strlen(p) > 3) { mod |= 0x04; p += 3; }
        else if (strncasecmp(p, "win+", 4) == 0 || strncasecmp(p, "gui+", 4) == 0 || strncasecmp(p, "cmd+", 4) == 0) { mod |= 0x08; p += 4; }
        else if ((strncasecmp(p, "win", 3) == 0 || strncasecmp(p, "gui", 3) == 0 || strncasecmp(p, "cmd", 3) == 0) && strlen(p) > 3) { mod |= 0x08; p += 3; }
        else break;
    }
    name = p;

    // D-Pad / Navigation
    if (strcasecmp(name, "up") == 0 || strcasecmp(name, "DirectionUp") == 0 || strcasecmp(name, "ArrowUp") == 0) { send_host_keyboard_report(mod, 0x52); return true; }
    if (strcasecmp(name, "down") == 0 || strcasecmp(name, "DirectionDown") == 0 || strcasecmp(name, "ArrowDown") == 0) { send_host_keyboard_report(mod, 0x51); return true; }
    if (strcasecmp(name, "left") == 0 || strcasecmp(name, "DirectionLeft") == 0 || strcasecmp(name, "ArrowLeft") == 0) { send_host_keyboard_report(mod, 0x50); return true; }
    if (strcasecmp(name, "right") == 0 || strcasecmp(name, "DirectionRight") == 0 || strcasecmp(name, "ArrowRight") == 0) { send_host_keyboard_report(mod, 0x4f); return true; }
    if (strcasecmp(name, "select") == 0 || strcasecmp(name, "enter") == 0 || strcasecmp(name, "OK") == 0 || strcasecmp(name, "Enter") == 0 || strcasecmp(name, "Select") == 0) { send_host_keyboard_report(mod, 0x28); return true; }
    if (strcasecmp(name, "back") == 0 || strcasecmp(name, "Return") == 0 || strcasecmp(name, "Exit") == 0 || strcasecmp(name, "Back") == 0) { send_host_consumer_report(0x0224); return true; } // Android Back
    if (strcasecmp(name, "home") == 0 || strcasecmp(name, "Home") == 0) { send_host_consumer_report(0x0223); return true; } // Android Home
    if (strcasecmp(name, "menu") == 0 || strcasecmp(name, "Menu") == 0 || strcasecmp(name, "settings") == 0 || strcasecmp(name, "Settings") == 0 || strcasecmp(name, "guide") == 0 || strcasecmp(name, "Guide") == 0) { send_host_consumer_report(0x008D); return true; }
    if (strcasecmp(name, "info") == 0 || strcasecmp(name, "Info") == 0) { send_host_consumer_report(0x01BD); return true; }

    // Media Controls
    if (strcasecmp(name, "play") == 0 || strcasecmp(name, "Play") == 0) { send_host_consumer_report(0x00B0); return true; }
    if (strcasecmp(name, "pause") == 0 || strcasecmp(name, "Pause") == 0) { send_host_consumer_report(0x00B1); return true; }
    if (strcasecmp(name, "play_pause") == 0 || strcasecmp(name, "PlayPause") == 0) { send_host_consumer_report(0x00CD); return true; }
    if (strcasecmp(name, "stop") == 0 || strcasecmp(name, "Stop") == 0) { send_host_consumer_report(0x00B7); return true; }
    if (strcasecmp(name, "next") == 0 || strcasecmp(name, "SkipForward") == 0 || strcasecmp(name, "NextTrack") == 0) { send_host_consumer_report(0x00B5); return true; }
    if (strcasecmp(name, "prev") == 0 || strcasecmp(name, "SkipBack") == 0 || strcasecmp(name, "PreviousTrack") == 0) { send_host_consumer_report(0x00B6); return true; }
    if (strcasecmp(name, "rewind") == 0 || strcasecmp(name, "Rewind") == 0) { send_host_consumer_report(0x00B4); return true; }
    if (strcasecmp(name, "fastforward") == 0 || strcasecmp(name, "FastForward") == 0) { send_host_consumer_report(0x00B3); return true; }

    // Audio & Power & Channels
    if (strcasecmp(name, "vol_up") == 0 || strcasecmp(name, "VolumeUp") == 0) { send_host_consumer_report(0x00E9); return true; }
    if (strcasecmp(name, "vol_down") == 0 || strcasecmp(name, "VolumeDown") == 0) { send_host_consumer_report(0x00EA); return true; }
    if (strcasecmp(name, "mute") == 0 || strcasecmp(name, "Mute") == 0) { send_host_consumer_report(0x00E2); return true; }
    if (strcasecmp(name, "power") == 0 || strcasecmp(name, "PowerToggle") == 0) { send_host_consumer_report(0x0030); return true; }
    if (strcasecmp(name, "ch_up") == 0 || strcasecmp(name, "ChannelUp") == 0) { send_host_consumer_report(0x009C); return true; }
    if (strcasecmp(name, "ch_down") == 0 || strcasecmp(name, "ChannelDown") == 0) { send_host_consumer_report(0x009D); return true; }

    // Color Keys (Android TV standard / Red, Green, Yellow, Blue)
    if (strcasecmp(name, "red") == 0) { send_host_consumer_report(0x0069); return true; }
    if (strcasecmp(name, "green") == 0) { send_host_consumer_report(0x006A); return true; }
    if (strcasecmp(name, "yellow") == 0) { send_host_consumer_report(0x006C); return true; }
    if (strcasecmp(name, "blue") == 0) { send_host_consumer_report(0x006B); return true; }

    // Digits
    if (strcmp(name, "0") == 0 || strcasecmp(name, "Number0") == 0) { send_host_keyboard_report(mod, 0x27); return true; }
    if (strcmp(name, "1") == 0 || strcasecmp(name, "Number1") == 0) { send_host_keyboard_report(mod, 0x1E); return true; }
    if (strcmp(name, "2") == 0 || strcasecmp(name, "Number2") == 0) { send_host_keyboard_report(mod, 0x1F); return true; }
    if (strcmp(name, "3") == 0 || strcasecmp(name, "Number3") == 0) { send_host_keyboard_report(mod, 0x20); return true; }
    if (strcmp(name, "4") == 0 || strcasecmp(name, "Number4") == 0) { send_host_keyboard_report(mod, 0x21); return true; }
    if (strcmp(name, "5") == 0 || strcasecmp(name, "Number5") == 0) { send_host_keyboard_report(mod, 0x22); return true; }
    if (strcmp(name, "6") == 0 || strcasecmp(name, "Number6") == 0) { send_host_keyboard_report(mod, 0x23); return true; }
    if (strcmp(name, "7") == 0 || strcasecmp(name, "Number7") == 0) { send_host_keyboard_report(mod, 0x24); return true; }
    if (strcmp(name, "8") == 0 || strcasecmp(name, "Number8") == 0) { send_host_keyboard_report(mod, 0x25); return true; }
    if (strcmp(name, "9") == 0 || strcasecmp(name, "Number9") == 0) { send_host_keyboard_report(mod, 0x26); return true; }

    // Keyboard keys
    if (strcasecmp(name, "escape") == 0 || strcasecmp(name, "esc") == 0) { send_host_keyboard_report(mod, 0x29); return true; }
    if (strcasecmp(name, "backspace") == 0) { send_host_keyboard_report(mod, 0x2a); return true; }
    if (strcasecmp(name, "tab") == 0) { send_host_keyboard_report(mod, 0x2b); return true; }
    if (strcasecmp(name, "space") == 0) { send_host_keyboard_report(mod, 0x2c); return true; }
    if (strcasecmp(name, "insert") == 0) { send_host_keyboard_report(mod, 0x49); return true; }
    if (strcasecmp(name, "delete") == 0) { send_host_keyboard_report(mod, 0x4c); return true; }
    if (strcasecmp(name, "pageup") == 0) { send_host_keyboard_report(mod, 0x4b); return true; }
    if (strcasecmp(name, "pagedown") == 0) { send_host_keyboard_report(mod, 0x4e); return true; }
    if (strcasecmp(name, "end") == 0) { send_host_keyboard_report(mod, 0x4d); return true; }
    if (strcasecmp(name, "capslock") == 0) { send_host_keyboard_report(mod, 0x39); return true; }

    // Symbols & Punctuation
    if (strcasecmp(name, "minus") == 0) { send_host_keyboard_report(mod, 0x2d); return true; }
    if (strcasecmp(name, "equal") == 0) { send_host_keyboard_report(mod, 0x2e); return true; }
    if (strcasecmp(name, "leftbracket") == 0) { send_host_keyboard_report(mod, 0x2f); return true; }
    if (strcasecmp(name, "rightbracket") == 0) { send_host_keyboard_report(mod, 0x30); return true; }
    if (strcasecmp(name, "backslash") == 0) { send_host_keyboard_report(mod, 0x31); return true; }
    if (strcasecmp(name, "semicolon") == 0) { send_host_keyboard_report(mod, 0x33); return true; }
    if (strcasecmp(name, "apostrophe") == 0) { send_host_keyboard_report(mod, 0x34); return true; }
    if (strcasecmp(name, "grave") == 0) { send_host_keyboard_report(mod, 0x35); return true; }
    if (strcasecmp(name, "comma") == 0) { send_host_keyboard_report(mod, 0x36); return true; }
    if (strcasecmp(name, "period") == 0) { send_host_keyboard_report(mod, 0x37); return true; }
    if (strcasecmp(name, "slash") == 0) { send_host_keyboard_report(mod, 0x38); return true; }

    // Function keys F1-F12
    if ((name[0] == 'f' || name[0] == 'F') && strlen(name) <= 3) {
        int fnum = atoi(name + 1);
        if (fnum >= 1 && fnum <= 12) {
            send_host_keyboard_report(mod, (uint8_t)(0x3a + (fnum - 1)));
            return true;
        }
    }

    // Single character
    if (strlen(name) == 1 && name[0] >= 'a' && name[0] <= 'z') { send_host_keyboard_report(mod, 0x04 + (name[0] - 'a')); return true; }
    if (strlen(name) == 1 && name[0] >= 'A' && name[0] <= 'Z') { send_host_keyboard_report(mod | 0x02, 0x04 + (name[0] - 'A')); return true; }

    unsigned int hex = 0;
    if (sscanf(name, "0x%x", &hex) == 1 || sscanf(name, "%x", &hex) == 1) {
        send_host_consumer_report((uint16_t)hex);
        return true;
    }
    return false;
}

static bool send_host_named_key_ex(const char *name, int hold_mode) {
    g_host_send_hold_mode = hold_mode;
    bool ret = send_host_named_key(name);
    g_host_send_hold_mode = 0;
    return ret;
}

static void send_host_text(const char *text) {
    if (!text || get_target_host_hid_cid() == 0) return;
    for (size_t i = 0; text[i]; i++) {
        uint8_t mod = 0, keycode = 0;
        unsigned char c = (unsigned char)text[i];
        if (c >= 'a' && c <= 'z') { keycode = 0x04 + (c - 'a'); }
        else if (c >= 'A' && c <= 'Z') { mod = 0x02; keycode = 0x04 + (c - 'A'); }
        else if (c >= '1' && c <= '9') { keycode = 0x1E + (c - '1'); }
        else if (c == '0') { keycode = 0x27; }
        else if (c == ' ') { keycode = 0x2c; }
        else if (c == '\n' || c == '\r') { keycode = 0x28; }
        else if (c == '\t') { keycode = 0x2b; }
        else if (c == '-') { keycode = 0x2d; }
        else if (c == '_') { mod = 0x02; keycode = 0x2d; }
        else if (c == '=') { keycode = 0x2e; }
        else if (c == '+') { mod = 0x02; keycode = 0x2e; }
        else if (c == '.') { keycode = 0x37; }
        else if (c == '/') { keycode = 0x38; }
        else if (c == ':') { mod = 0x02; keycode = 0x33; }
        else if (c == ';') { keycode = 0x33; }
        else if (c == ',') { keycode = 0x36; }
        else if (c == '<') { mod = 0x02; keycode = 0x36; }
        else if (c == '>') { mod = 0x02; keycode = 0x37; }
        else if (c == '?') { mod = 0x02; keycode = 0x38; }
        else if (c == '!') { mod = 0x02; keycode = 0x1e; }
        else if (c == '@') { mod = 0x02; keycode = 0x1f; }
        else if (c == '#') { mod = 0x02; keycode = 0x20; }
        else if (c == '$') { mod = 0x02; keycode = 0x21; }
        else if (c == '%') { mod = 0x02; keycode = 0x22; }
        if (keycode) {
            send_host_keyboard_report(mod, keycode);
            usleep(25000);
        }
    }
}

static void execute_btstack_cmd(const char *cmd_in);

static void poll_bthid_fifo(void) {
    if (g_bthid_fifo_fd < 0) {
        struct stat st;
        if (stat(BTHID_FIFO, &st) == 0 && !S_ISFIFO(st.st_mode)) unlink(BTHID_FIFO);
        if (stat(BTHID_FIFO, &st) != 0) mkfifo(BTHID_FIFO, 0666);
        chmod(BTHID_FIFO, 0666);
        g_bthid_fifo_fd = open(BTHID_FIFO, O_RDWR | O_NONBLOCK);
        if (g_bthid_fifo_fd < 0) return;
    }

    char buf[1024];
    ssize_t n = read(g_bthid_fifo_fd, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = 0;

    char *save = NULL;
    char *line = strtok_r(buf, "\r\n", &save);
    while (line) {
        while (*line == ' ') line++;
        if (*line) {
            if (strncmp(line, "pair_host_on", 12) == 0 ||
                strncmp(line, "pair_host_off", 13) == 0 ||
                strncmp(line, "connect_host", 12) == 0 ||
                strncmp(line, "disconnect_host", 15) == 0 ||
                strncmp(line, "scan", 4) == 0 ||
                strncmp(line, "pair ", 5) == 0) {
                execute_btstack_cmd(line);
                line = strtok_r(NULL, "\r\n", &save);
                continue;
            }
            if (strncmp(line, "TARGET ", 7) == 0) {
                bd_addr_t t_addr;
                if (parse_bd_addr(line + 7, t_addr)) {
                    register_host_target(t_addr, NULL);
                    for (int i = 0; i < MAX_HOST_CONNS; i++) {
                        if (memcmp(g_hosts[i].addr, t_addr, 6) == 0) {
                            g_active_host_idx = i;
                            if (!g_hosts[i].connected || g_hosts[i].hid_cid == 0) {
                                if (!g_host_reconnect_active) {
                                    start_host_reconnect(t_addr, MAX_HOST_RECONNECT_ATTEMPTS);
                                } else {
                                    try_connect_host_addr(t_addr);
                                }
                            }
                            break;
                        }
                    }
                }
                line = strtok_r(NULL, "\r\n", &save);
                continue;
            }
            uint16_t cur_cid = get_target_host_hid_cid();
            if (cur_cid == 0) {
                if (!g_host_reconnect_active) {
                    bd_addr_t tgt;
                    if (g_active_host_idx >= 0 && g_active_host_idx < MAX_HOST_CONNS && !is_null_bd_addr(g_hosts[g_active_host_idx].addr)) {
                        start_host_reconnect(g_hosts[g_active_host_idx].addr, 20);
                    } else if (get_pinned_host_target(tgt)) {
                        start_host_reconnect(tgt, 20);
                    } else {
                        try_connect_host();
                    }
                }
            }
            if (strncmp(line, "TEXT ", 5) == 0) {
                send_host_text(line + 5);
            } else if (strncmp(line, "KEYDOWN ", 8) == 0) {
                send_host_named_key_ex(line + 8, 1);
            } else if (strncmp(line, "KEYUP ", 6) == 0) {
                send_host_release_all();
            } else if (strncmp(line, "KEY ", 4) == 0) {
                send_host_named_key_ex(line + 4, 0);
            } else if (strcasecmp(line, "RELEASE") == 0 || strcasecmp(line, "RELEASE_ALL") == 0) {
                send_host_release_all();
            } else if (strncmp(line, "CONSUMER_DOWN ", 14) == 0) {
                unsigned int ccode = 0;
                if (sscanf(line + 14, "%x", &ccode) == 1) {
                    g_host_send_hold_mode = 1;
                    send_host_consumer_report((uint16_t)ccode);
                    g_host_send_hold_mode = 0;
                }
            } else if (strncmp(line, "CONSUMER_UP", 11) == 0) {
                send_host_release_all();
            } else if (strncmp(line, "CONSUMER ", 9) == 0) {
                unsigned int ccode = 0;
                if (sscanf(line + 9, "%x", &ccode) == 1) {
                    send_host_consumer_report((uint16_t)ccode);
                }
            } else if (strncmp(line, "MOUSE ", 6) == 0) {
                int btn = 0, dx = 0, dy = 0, wheel = 0;
                int n = sscanf(line + 6, "%d %d %d %d", &btn, &dx, &dy, &wheel);
                if (n >= 3) {
                    g_host_mouse_buttons = (uint8_t)btn;
                    send_host_mouse_report(g_host_mouse_buttons, (int8_t)dx, (int8_t)dy, (int8_t)wheel);
                }
            } else if (strncmp(line, "MOUSE_MOVE ", 11) == 0) {
                int dx = 0, dy = 0;
                if (sscanf(line + 11, "%d %d", &dx, &dy) == 2) {
                    send_host_mouse_report(g_host_mouse_buttons, (int8_t)dx, (int8_t)dy, 0);
                }
            } else if (strncmp(line, "MOUSE_CLICK ", 12) == 0) {
                const char *p = line + 12;
                while (*p == ' ') p++;
                uint8_t b = 1;
                if (strcasecmp(p, "right") == 0 || strcmp(p, "2") == 0) b = 2;
                else if (strcasecmp(p, "middle") == 0 || strcmp(p, "3") == 0) b = 4;
                send_host_mouse_click(b);
            } else if (strncmp(line, "MOUSE_DOWN ", 11) == 0) {
                const char *p = line + 11;
                while (*p == ' ') p++;
                uint8_t b = 1;
                if (strcasecmp(p, "right") == 0 || strcmp(p, "2") == 0) b = 2;
                else if (strcasecmp(p, "middle") == 0 || strcmp(p, "3") == 0) b = 4;
                else if (atoi(p) > 0) b = (uint8_t)atoi(p);
                g_host_mouse_buttons |= b;
                send_host_mouse_report(g_host_mouse_buttons, 0, 0, 0);
            } else if (strncmp(line, "MOUSE_UP ", 9) == 0) {
                const char *p = line + 9;
                while (*p == ' ') p++;
                uint8_t b = 1;
                if (strcasecmp(p, "right") == 0 || strcmp(p, "2") == 0) b = 2;
                else if (strcasecmp(p, "middle") == 0 || strcmp(p, "3") == 0) b = 4;
                else if (atoi(p) > 0) b = (uint8_t)atoi(p);
                g_host_mouse_buttons &= ~b;
                send_host_mouse_report(g_host_mouse_buttons, 0, 0, 0);
            } else if (strncmp(line, "MOUSE_WHEEL ", 12) == 0) {
                int w = 0;
                if (sscanf(line + 12, "%d", &w) == 1) {
                    send_host_mouse_report(g_host_mouse_buttons, 0, 0, (int8_t)w);
                }
            } else if (strncmp(line, "RAW ", 4) == 0) {
                uint8_t raw[16];
                int rlen = parse_hex_bytes(line + 4, raw, sizeof(raw));
                cur_cid = get_target_host_hid_cid();
                if (rlen > 0 && cur_cid != 0) {
                    hid_device_send_interrupt_message(cur_cid, raw, rlen);
                    g_keys_sent++;
                    update_bthid_status();
                }
            } else if (strchr(line, '|')) {
                // Pipe-separated report sequence: press|release
                char *pipe = strchr(line, '|');
                *pipe = 0;
                char *rel_str = pipe + 1;
                uint8_t press_buf[16];
                int press_len = parse_hex_bytes(line, press_buf, sizeof(press_buf));
                uint8_t rel_buf[16];
                int rel_len = parse_hex_bytes(rel_str, rel_buf, sizeof(rel_buf));
                cur_cid = get_target_host_hid_cid();
                if (press_len > 0 && cur_cid != 0) {
                    if (g_host_pending_release_type != 0) {
                        btstack_run_loop_remove_timer(&g_host_key_release_timer);
                        host_key_release_timer_handler(NULL);
                    }
                    hid_device_send_interrupt_message(cur_cid, press_buf, press_len);
                    if (rel_len > 0) {
                        memcpy(g_host_custom_release_buf, rel_buf, rel_len);
                        g_host_custom_release_len = rel_len;
                        g_host_pending_release_type = 3;
                        btstack_run_loop_remove_timer(&g_host_key_release_timer);
                        btstack_run_loop_set_timer_handler(&g_host_key_release_timer, host_key_release_timer_handler);
                        btstack_run_loop_set_timer(&g_host_key_release_timer, 30);
                        btstack_run_loop_add_timer(&g_host_key_release_timer);
                    }
                    g_keys_sent++;
                    update_bthid_status();
                }
            } else if ((strncasecmp(line, "A101", 4) == 0 || strncasecmp(line, "A102", 4) == 0 || strncasecmp(line, "A103", 4) == 0) && strlen(line) >= 8) {
                // Single hex report
                uint8_t rep[16];
                int rlen = parse_hex_bytes(line, rep, sizeof(rep));
                cur_cid = get_target_host_hid_cid();
                if (rlen > 0 && cur_cid != 0) {
                    if (g_host_pending_release_type != 0) {
                        btstack_run_loop_remove_timer(&g_host_key_release_timer);
                        host_key_release_timer_handler(NULL);
                    }
                    hid_device_send_interrupt_message(cur_cid, rep, rlen);
                    if (rep[1] == 0x01 && (rlen > 4 && rep[4] != 0)) {
                        g_host_pending_release_type = 1;
                        btstack_run_loop_remove_timer(&g_host_key_release_timer);
                        btstack_run_loop_set_timer_handler(&g_host_key_release_timer, host_key_release_timer_handler);
                        btstack_run_loop_set_timer(&g_host_key_release_timer, 30);
                        btstack_run_loop_add_timer(&g_host_key_release_timer);
                    } else if (rep[1] == 0x02 && (rep[2] != 0 || rep[3] != 0)) {
                        g_host_pending_release_type = 2;
                        btstack_run_loop_remove_timer(&g_host_key_release_timer);
                        btstack_run_loop_set_timer_handler(&g_host_key_release_timer, host_key_release_timer_handler);
                        btstack_run_loop_set_timer(&g_host_key_release_timer, 30);
                        btstack_run_loop_add_timer(&g_host_key_release_timer);
                    } else if (rep[1] == 0x03 && rlen > 2 && rep[2] != 0) {
                        g_host_pending_release_type = 4;
                        btstack_run_loop_remove_timer(&g_host_key_release_timer);
                        btstack_run_loop_set_timer_handler(&g_host_key_release_timer, host_key_release_timer_handler);
                        btstack_run_loop_set_timer(&g_host_key_release_timer, 30);
                        btstack_run_loop_add_timer(&g_host_key_release_timer);
                    }
                    g_keys_sent++;
                    update_bthid_status();
                }
            } else {
                if (!send_host_named_key(line)) {
                    send_host_text(line);
                }
            }
        }
        line = strtok_r(NULL, "\r\n", &save);
    }
}

static void execute_btstack_cmd(const char *cmd_in) {
    if (!cmd_in) return;
    char cmd[128] = "";
    strncpy(cmd, cmd_in, sizeof(cmd) - 1);
    char *p = cmd + strlen(cmd) - 1;
    while (p >= cmd && (*p == '\r' || *p == '\n' || *p == ' ')) *p-- = 0;
    if (!cmd[0]) return;

    if (strncmp(cmd, "scan", 4) == 0) {
        start_user_scan();
    } else if (strncmp(cmd, "pair ", 5) == 0) {
        char *addr_str = cmd + 5;
        while (*addr_str == ' ') addr_str++;
        start_user_pair(addr_str);
    } else if (strncmp(cmd, "pair_host_on", 12) == 0) {
        stop_host_reconnect();
        for (int i = 0; i < MAX_HOST_CONNS; i++) {
            if (g_hosts[i].hid_cid) {
                hid_device_disconnect(g_hosts[i].hid_cid);
                g_hosts[i].hid_cid = 0;
            }
            if (g_hosts[i].acl_handle != HCI_CON_HANDLE_INVALID) {
                gap_disconnect(g_hosts[i].acl_handle);
                g_hosts[i].acl_handle = HCI_CON_HANDLE_INVALID;
            }
            if (!is_null_bd_addr(g_hosts[i].addr)) {
                gap_drop_link_key_for_bd_addr(g_hosts[i].addr);
            }
            g_hosts[i].connected = false;
        }
        if (g_host_hid_cid) {
            hid_device_disconnect(g_host_hid_cid);
            g_host_hid_cid = 0;
        }
        if (!is_null_bd_addr(g_host_addr)) {
            gap_drop_link_key_for_bd_addr(g_host_addr);
        }
        g_host_connected = false;
        char *name = cmd + 12;
        while (*name == ' ') name++;
        if (name[0]) {
            strncpy(g_host_name, name, sizeof(g_host_name) - 1);
            gap_set_local_name(g_host_name);
        }
        g_host_pairing = true;
        gap_set_class_of_device(0x0025C0); // Combo: Keyboard + Pointing Device
        gap_connectable_control(1);
        gap_discoverable_control(1);
        char msg[128];
        snprintf(msg, sizeof(msg), "Classic HID Host pairing enabled as '%s'", g_host_name);
        log_msg(msg);
        update_status(app_state == APP_READY ? "ready" : "standby");
        update_bthid_status();
    } else if (strncmp(cmd, "pair_host_off", 13) == 0) {
        g_host_pairing = false;
        gap_discoverable_control(0);
        log_msg("Classic HID Host pairing disabled");
        update_status(app_state == APP_READY ? "ready" : "standby");
        update_bthid_status();
    } else if (strncmp(cmd, "connect_host", 12) == 0) {
        char *addr_str = cmd + 12;
        while (*addr_str == ' ') addr_str++;
        bd_addr_t tgt;
        stop_host_reconnect();
        g_host_connecting = false;
        if (parse_bd_addr(addr_str, tgt)) {
            register_host_target(tgt, NULL);
            start_host_reconnect(tgt, MAX_HOST_RECONNECT_ATTEMPTS);
        } else {
            bd_addr_t def_tgt;
            if (get_pinned_host_target(def_tgt)) {
                start_host_reconnect(def_tgt, MAX_HOST_RECONNECT_ATTEMPTS);
            } else {
                start_host_reconnect(g_host_addr, MAX_HOST_RECONNECT_ATTEMPTS);
            }
        }
    } else if (strncmp(cmd, "disconnect_host", 15) == 0) {
        stop_host_reconnect();
        char *addr_str = cmd + 15;
        while (*addr_str == ' ') addr_str++;
        bd_addr_t tgt;
        if (parse_bd_addr(addr_str, tgt)) {
            for (int i = 0; i < MAX_HOST_CONNS; i++) {
                if (memcmp(g_hosts[i].addr, tgt, 6) == 0 && g_hosts[i].hid_cid) {
                    hid_device_disconnect(g_hosts[i].hid_cid);
                    char dmsg[64];
                    snprintf(dmsg, sizeof(dmsg), "Host %s disconnect requested", addr_str);
                    log_msg(dmsg);
                }
            }
        } else {
            for (int i = 0; i < MAX_HOST_CONNS; i++) {
                if (g_hosts[i].connected && g_hosts[i].hid_cid) {
                    hid_device_disconnect(g_hosts[i].hid_cid);
                }
            }
            if (g_host_connected && g_host_hid_cid) {
                hid_device_disconnect(g_host_hid_cid);
            }
            log_msg("Host disconnect requested for all");
        }
    } else if (strncmp(cmd, "key ", 4) == 0) {
        send_host_named_key(cmd + 4);
    } else if (strncmp(cmd, "text ", 5) == 0) {
        send_host_text(cmd + 5);
    }
}

static void cmd_sock_process(btstack_data_source_t *ds, btstack_data_source_callback_type_t callback_type) {
    (void)ds;
    if (callback_type == DATA_SOURCE_CALLBACK_READ && g_cmd_sock_fd >= 0) {
        char buf[256];
        ssize_t n = recvfrom(g_cmd_sock_fd, buf, sizeof(buf) - 1, 0, NULL, NULL);
        if (n > 0) {
            buf[n] = 0;
            execute_btstack_cmd(buf);
        }
    }
}

static void init_cmd_socket(void) {
    unlink(CMD_SOCK);
    g_cmd_sock_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (g_cmd_sock_fd < 0) {
        fprintf(stderr, "[codex_btstack] Failed to create IPC socket: %s\n", strerror(errno));
        return;
    }
    fcntl(g_cmd_sock_fd, F_SETFD, FD_CLOEXEC);
    fcntl(g_cmd_sock_fd, F_SETFL, fcntl(g_cmd_sock_fd, F_GETFL, 0) | O_NONBLOCK);

    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, CMD_SOCK, sizeof(sun.sun_path) - 1);
    if (bind(g_cmd_sock_fd, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
        fprintf(stderr, "[codex_btstack] Failed to bind IPC socket %s: %s\n", CMD_SOCK, strerror(errno));
        close(g_cmd_sock_fd);
        g_cmd_sock_fd = -1;
        return;
    }
    chmod(CMD_SOCK, 0666);

    btstack_run_loop_set_data_source_fd(&g_cmd_sock_ds, g_cmd_sock_fd);
    btstack_run_loop_set_data_source_handler(&g_cmd_sock_ds, &cmd_sock_process);
    btstack_run_loop_add_data_source(&g_cmd_sock_ds);
    btstack_run_loop_enable_data_source_callbacks(&g_cmd_sock_ds, DATA_SOURCE_CALLBACK_READ);
    log_msg("IPC command socket initialized: " CMD_SOCK);
}

static void cleanup_cmd_socket(void) {
    if (g_cmd_sock_fd >= 0) {
        btstack_run_loop_remove_data_source(&g_cmd_sock_ds);
        close(g_cmd_sock_fd);
        g_cmd_sock_fd = -1;
        unlink(CMD_SOCK);
    }
}

static void check_command_file(void) {
    poll_bthid_fifo();

    if (g_user_scan_active && time(NULL) >= g_user_scan_end) {
        g_user_scan_active = false;
        log_msg("BLE remote scan window ended");
        write_scan_results(false);
        gap_stop_scan();
        hog_start_connect();
        return;
    }

    if (access(CMD_FILE, F_OK) != 0) return;
    FILE *f = fopen(CMD_FILE, "r");
    if (!f) return;
    char cmd[128] = "";
    if (fgets(cmd, sizeof(cmd), f)) {
        char *p = cmd + strlen(cmd) - 1;
        while (p >= cmd && (*p == '\r' || *p == '\n' || *p == ' ')) *p-- = 0;
    }
    fclose(f);
    unlink(CMD_FILE);

    if (cmd[0]) {
        execute_btstack_cmd(cmd);
    }
}

static void cmd_poll_timeout(btstack_timer_source_t *ts) {
    if (!g_running) {
        btstack_run_loop_trigger_exit();
        return;
    }
    check_command_file();
    btstack_run_loop_remove_timer(ts);
    btstack_run_loop_set_timer(ts, 100);
    btstack_run_loop_add_timer(ts);
}

/* Button mapping parser for Homatics B25 / BLE remotes */
static const char *parse_button(const uint8_t *data, uint16_t len) {
    if (len < 1) return NULL;

    /* Report ID 0x01: Keyboard report [0x01, mod, res, k1, ...] */
    if (data[0] == 0x01 && len >= 3) {
        uint8_t k = (len >= 4 && data[1] == 0) ? data[3] : data[2];
        if (k == 0x00) return NULL; // Release
        switch (k) {
            case 0x52: return "up";
            case 0x51: return "down";
            case 0x50: return "left";
            case 0x4F: return "right";
            case 0x28: return "select";
            case 0x29: return "back";
            case 0x4A: return "home";
            case 0x80: return "vol_up";
            case 0x81: return "vol_down";
            case 0x1E: return "1"; case 0x1F: return "2"; case 0x20: return "3";
            case 0x21: return "4"; case 0x22: return "5"; case 0x23: return "6";
            case 0x24: return "7"; case 0x25: return "8"; case 0x26: return "9";
            case 0x27: return "0";
            default: return NULL;
        }
    }

    /* Raw keyboard report without Report ID [mod, res, k1, ...] (len == 8) */
    if (len == 8) {
        uint8_t k = (data[1] == 0 && data[3] != 0) ? data[3] : data[2];
        if (k != 0x00) {
            switch (k) {
                case 0x52: return "up";
                case 0x51: return "down";
                case 0x50: return "left";
                case 0x4F: return "right";
                case 0x28: return "select";
                case 0x29: return "back";
                case 0x4A: return "home";
                case 0x80: return "vol_up";
                case 0x81: return "vol_down";
                case 0x1E: return "1"; case 0x1F: return "2"; case 0x20: return "3";
                case 0x21: return "4"; case 0x22: return "5"; case 0x23: return "6";
                case 0x24: return "7"; case 0x25: return "8"; case 0x26: return "9";
                case 0x27: return "0";
                default: break;
            }
        }
    }

    /* Report ID 0x02 or direct 16-bit Consumer Control */
    uint16_t cc = 0;
    if (data[0] == 0x02 && len >= 3) {
        cc = data[1] | (data[2] << 8);
    } else if (len >= 2) {
        cc = data[0] | (data[1] << 8);
    }
    if (cc == 0x0000) return NULL; // Release

    switch (cc) {
        case 0x0042: return "up";
        case 0x0043: return "down";
        case 0x0044: return "left";
        case 0x0045: return "right";
        case 0x0041: return "select";
        case 0x0223: return "home";
        case 0x0224: return "back";
        case 0x0030: return "power";
        case 0x01BB: return "input";
        case 0x00E2: return "mute";
        case 0x00E9: return "vol_up";
        case 0x00EA: return "vol_down";
        case 0x0069: return "red";
        case 0x006A: return "green";
        case 0x006C: return "yellow";
        case 0x006B:
        case 0x006D: return "blue";
        case 0x0061: return "guide";
        case 0x008D: return "live_tv";
        case 0x01BD: return "info";
        case 0x009C: return "ch_up";
        case 0x009D: return "ch_down";
        case 0x0077: return "youtube";
        case 0x0078: return "netflix";
        case 0x0079: return "prime_video";
        case 0x007A: return "google_play";
        case 0x0096: return "settings";
        case 0x0221: return "assistant";
        case 0x022A: return "watchlist";
        default: return NULL;
    }
}

static void trigger_http_post(const char *endpoint, const char *json_body) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    struct sockaddr_in addr;
    struct timeval tv = { .tv_sec = 1, .tv_usec = 500000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return;
    }
    char req[512];
    size_t blen = json_body ? strlen(json_body) : 0;
    snprintf(req, sizeof(req),
             "POST %s HTTP/1.1\r\n"
             "Host: 127.0.0.1:8080\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n\r\n%s",
             endpoint, blen, json_body ? json_body : "");
    write(fd, req, strlen(req));
    close(fd);
}

static char *read_entire_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 500000) { fclose(f); return NULL; }
    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, sz, f);
    buf[rd] = 0;
    fclose(f);
    return buf;
}

static cJSON *g_cached_map_json = NULL;
static time_t g_map_mtime = 0;

static cJSON *get_cached_map(void) {
    struct stat st;
    if (stat(MAP_FILE, &st) != 0) return NULL;
    if (g_cached_map_json && st.st_mtime == g_map_mtime) {
        return g_cached_map_json;
    }
    if (g_cached_map_json) {
        cJSON_Delete(g_cached_map_json);
        g_cached_map_json = NULL;
    }
    char *buf = read_entire_file(MAP_FILE);
    if (buf) {
        g_cached_map_json = cJSON_Parse(buf);
        free(buf);
    }
    g_map_mtime = st.st_mtime;
    return g_cached_map_json;
}

static void read_current_activity(char *out, size_t outlen) {
    FILE *f = fopen(CURRENT_ACT_FILE, "r");
    if (f) {
        if (fgets(out, outlen, f)) {
            fclose(f);
            char *p = out + strlen(out) - 1;
            while (p >= out && (*p == '\r' || *p == '\n' || *p == ' ')) *p-- = 0;
            if (out[0]) return;
        } else {
            fclose(f);
        }
    }
    snprintf(out, outlen, "-1");
}

static int is_bt_device_id(const char *target) {
    if (!target || !target[0]) return 0;
    if (strncmp(target, "bt", 2) == 0 || strcasecmp(target, "bluetooth") == 0 || strcasecmp(target, "bthid") == 0) return 1;

    static char s_cached_id[64] = "";
    static int s_cached_is_bt = 0;
    if (strcmp(s_cached_id, target) == 0) return s_cached_is_bt;

    int is_bt = 0;
    const char *dpath = "/data/codex/config/devices.json";
    if (access(dpath, F_OK) != 0) dpath = "/data/codex/devices.json";
    FILE *f = fopen(dpath, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz <= 1000000) {
            char *buf = (char *)malloc(sz + 1);
            if (buf) {
                size_t n = fread(buf, 1, sz, f);
                buf[n] = 0;
                cJSON *root = cJSON_Parse(buf);
                free(buf);
                if (root) {
                    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
                    if (devs && cJSON_IsArray(devs)) {
                        cJSON *item = NULL;
                        cJSON_ArrayForEach(item, devs) {
                            cJSON *j_id = cJSON_GetObjectItemCaseSensitive(item, "id");
                            if (!j_id) j_id = cJSON_GetObjectItemCaseSensitive(item, "Id");
                            char id_str[64] = {0};
                            if (j_id) {
                                if (cJSON_IsNumber(j_id)) snprintf(id_str, sizeof(id_str), "%ld", (long)j_id->valuedouble);
                                else if (cJSON_IsString(j_id) && j_id->valuestring) strncpy(id_str, j_id->valuestring, sizeof(id_str) - 1);
                            }
                            cJSON *j_name = cJSON_GetObjectItemCaseSensitive(item, "name");
                            if (!j_name) j_name = cJSON_GetObjectItemCaseSensitive(item, "Name");
                            const char *name_str = (j_name && cJSON_IsString(j_name)) ? j_name->valuestring : "";
                            if ((id_str[0] && strcmp(id_str, target) == 0) ||
                                (name_str && name_str[0] && strcasecmp(name_str, target) == 0)) {
                                cJSON *j_type = cJSON_GetObjectItemCaseSensitive(item, "type");
                                cJSON *j_cp = cJSON_GetObjectItemCaseSensitive(item, "controlPort");
                                if (j_type && cJSON_IsString(j_type) && strcasecmp(j_type->valuestring, "BT") == 0) {
                                    is_bt = 1;
                                } else if (j_cp && cJSON_IsNumber(j_cp) && (int)j_cp->valuedouble == 32) {
                                    is_bt = 1;
                                }
                                break;
                            }
                        }
                    } else {
                        cJSON *dwf = cJSON_GetObjectItemCaseSensitive(root, "DevicesWithFeatures");
                        if (dwf && cJSON_IsArray(dwf)) {
                            cJSON *item = NULL;
                            cJSON_ArrayForEach(item, dwf) {
                                cJSON *dev = cJSON_GetObjectItemCaseSensitive(item, "Device");
                                if (!dev) continue;
                                cJSON *j_id = cJSON_GetObjectItemCaseSensitive(dev, "Id-");
                                if (!j_id) j_id = cJSON_GetObjectItemCaseSensitive(dev, "Id");
                                if (!j_id) j_id = cJSON_GetObjectItemCaseSensitive(dev, "id");
                                char id_str[64] = {0};
                                if (j_id) {
                                    if (cJSON_IsNumber(j_id)) snprintf(id_str, sizeof(id_str), "%ld", (long)j_id->valuedouble);
                                    else if (cJSON_IsString(j_id) && j_id->valuestring) strncpy(id_str, j_id->valuestring, sizeof(id_str) - 1);
                                }
                                cJSON *j_name = cJSON_GetObjectItemCaseSensitive(dev, "Name");
                                const char *name_str = (j_name && cJSON_IsString(j_name)) ? j_name->valuestring : "";
                                if ((id_str[0] && strcmp(id_str, target) == 0) ||
                                    (name_str && name_str[0] && strcasecmp(name_str, target) == 0)) {
                                    cJSON *j_trans = cJSON_GetObjectItemCaseSensitive(dev, "Transport");
                                    if (!j_trans) j_trans = cJSON_GetObjectItemCaseSensitive(dev, "TransportType");
                                    if (j_trans && cJSON_IsNumber(j_trans) && (int)j_trans->valuedouble == 32) {
                                        is_bt = 1;
                                    }
                                    break;
                                }
                            }
                        }
                    }
                    cJSON_Delete(root);
                }
            }
        }
        fclose(f);
    }

    strncpy(s_cached_id, target, sizeof(s_cached_id) - 1);
    s_cached_is_bt = is_bt;
    return is_bt;
}

static bool is_remote_release_packet(const uint8_t *data, uint16_t len) {
    if (!data || len < 1) return false;
    if (data[0] == 0x01 && len >= 3) {
        uint8_t k = (len >= 4 && data[1] == 0) ? data[3] : data[2];
        return (k == 0x00 && data[1] == 0);
    }
    if (len == 8) {
        uint8_t k = (data[1] == 0 && data[3] != 0) ? data[3] : data[2];
        return (k == 0x00 && data[0] == 0);
    }
    if (data[0] == 0x02 && len >= 3) {
        uint16_t cc = (uint16_t)data[1] | ((uint16_t)data[2] << 8);
        return (cc == 0x0000);
    }
    if (len >= 2 && len < 8 && data[0] != 0x01) {
        uint16_t cc = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
        return (cc == 0x0000);
    }
    return false;
}

static btstack_timer_source_t g_remote_watchdog_timer;
static bool g_remote_watchdog_active = false;
static char g_active_remote_btn[64] = "";

static btstack_timer_source_t g_remote_wake_grace_timer;
static bool g_remote_wake_grace_active = true;

static void remote_wake_grace_timeout_handler(btstack_timer_source_t *ts) {
    UNUSED(ts);
    g_remote_wake_grace_active = false;
    codex_log_debug("Remote wake grace window closed; full hold-mode active");
}

static void dispatch_button_event(const char *btn, int is_press);

static void remote_watchdog_timeout_handler(btstack_timer_source_t *ts) {
    UNUSED(ts);
    g_remote_wake_grace_active = false;
    g_remote_watchdog_active = false;
    if (g_active_remote_btn[0]) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Remote hold watchdog expired (auto-release): %s", g_active_remote_btn);
        codex_log_debug(msg);
        dispatch_button_event(g_active_remote_btn, 0 /* release */);
        g_active_remote_btn[0] = 0;
    }
}

static void dispatch_button_event(const char *btn, int is_press) {
    if (!btn || !btn[0]) return;
    if (is_press) {
        strncpy(g_last_btn, btn, sizeof(g_last_btn) - 1);
        g_last_seen = time(NULL);
        update_status("ready");

        // 1. Emit event to /tmp/bt_remote_events.log for WebUI & MQTT
        struct stat est;
        if (stat(BT_EVENT_FILE, &est) == 0 && est.st_size > LOG_MAX_BYTES) {
            unlink(BT_EVENT_FILE ".1");
            rename(BT_EVENT_FILE, BT_EVENT_FILE ".1");
        }
        FILE *ef = fopen(BT_EVENT_FILE, "a");
        if (ef) {
            fprintf(ef, "{\"timestamp\":%ld,\"button\":\"%s\",\"bdaddr\":\"%s\",\"event\":\"down\"}\n",
                    (long)g_last_seen, btn, bd_addr_to_str(remote_device.addr));
            fclose(ef);
        }

        char msg[160];
        snprintf(msg, sizeof(msg), "Button pressed: %s", btn);
        codex_log_debug(msg);
    } else {
        FILE *ef = fopen(BT_EVENT_FILE, "a");
        if (ef) {
            fprintf(ef, "{\"timestamp\":%ld,\"button\":\"%s\",\"bdaddr\":\"%s\",\"event\":\"up\"}\n",
                    (long)time(NULL), btn, bd_addr_to_str(remote_device.addr));
            fclose(ef);
        }
        char msg[160];
        snprintf(msg, sizeof(msg), "Button released: %s", btn);
        codex_log_debug(msg);
        if (g_remote_wake_grace_active) {
            g_remote_wake_grace_active = false;
            btstack_run_loop_remove_timer(&g_remote_wake_grace_timer);
            codex_log_debug("Remote first button released; wake grace window ended");
        }
    }

    // 2. Read current activity
    char act_id[32] = "-1";
    read_current_activity(act_id, sizeof(act_id));

    // 3. Lookup mapping in cached bt_remote_map.json
    cJSON *map = get_cached_map();
    if (!map) {
        if (!is_press) send_host_release_all();
        log_msg("Mapping file not available or invalid JSON");
        return;
    }

    cJSON *activities = cJSON_GetObjectItemCaseSensitive(map, "activities");
    if (!activities || !cJSON_IsObject(activities)) activities = map; // fallback if flat

    cJSON *btn_obj = NULL;
    if (act_id[0] && strcmp(act_id, "-1") != 0) {
        cJSON *act_obj = cJSON_GetObjectItemCaseSensitive(activities, act_id);
        if (act_obj && cJSON_IsObject(act_obj)) {
            btn_obj = cJSON_GetObjectItemCaseSensitive(act_obj, btn);
        }
    }
    if (!btn_obj) {
        cJSON *def_obj = cJSON_GetObjectItemCaseSensitive(activities, "-1");
        if (!def_obj) def_obj = cJSON_GetObjectItemCaseSensitive(activities, "default");
        if (def_obj && cJSON_IsObject(def_obj)) {
            btn_obj = cJSON_GetObjectItemCaseSensitive(def_obj, btn);
        }
    }

    if (!btn_obj || !cJSON_IsObject(btn_obj)) {
        if (!is_press) send_host_release_all();
        char msg[160];
        snprintf(msg, sizeof(msg), "Button '%s' unmapped for activity %s", btn, act_id);
        codex_log_debug(msg);
        return;
    }

    cJSON *j_act = cJSON_GetObjectItemCaseSensitive(btn_obj, "action");
    if (!j_act) j_act = cJSON_GetObjectItemCaseSensitive(btn_obj, "type");
    const char *action = (j_act && cJSON_IsString(j_act)) ? j_act->valuestring : "";

    cJSON *j_tgt = cJSON_GetObjectItemCaseSensitive(btn_obj, "targetDevice");
    if (!j_tgt) j_tgt = cJSON_GetObjectItemCaseSensitive(btn_obj, "deviceId");
    if (!j_tgt) j_tgt = cJSON_GetObjectItemCaseSensitive(btn_obj, "activityId");
    const char *target = (j_tgt && cJSON_IsString(j_tgt)) ? j_tgt->valuestring : "";

    cJSON *j_cmd = cJSON_GetObjectItemCaseSensitive(btn_obj, "command");
    const char *cmd = (j_cmd && cJSON_IsString(j_cmd)) ? j_cmd->valuestring : "";

    char msg[160];
    if (strcmp(action, "passthrough_bt") == 0 && cmd[0]) {
        if (!g_host_connected || !g_host_hid_cid) {
            if (!g_host_reconnect_active) {
                bd_addr_t tgt;
                if (get_pinned_host_target(tgt)) start_host_reconnect(tgt, 20);
                else try_connect_host();
            }
        }
        if (g_host_connected && g_host_hid_cid) {
            if (is_press) {
                int hold_mode = g_remote_wake_grace_active ? 0 : 1;
                send_host_named_key_ex(cmd, hold_mode);
                snprintf(msg, sizeof(msg), "Dispatched passthrough_bt %s: %s%s", hold_mode ? "hold" : "tap", cmd, g_remote_wake_grace_active ? " (wake-grace tap)" : "");
            } else {
                send_host_release_all();
                snprintf(msg, sizeof(msg), "Dispatched passthrough_bt release: %s", cmd);
            }
        } else {
            snprintf(msg, sizeof(msg), "passthrough_bt dropped (host connecting/offline): %s", cmd);
        }
        codex_log_debug(msg);
    } else if (strcmp(action, "device_cmd") == 0 && target[0] && cmd[0]) {
        if (is_bt_device_id(target)) {
            if (!g_host_connected || !g_host_hid_cid) {
                if (!g_host_reconnect_active) {
                    bd_addr_t tgt;
                    if (get_pinned_host_target(tgt)) start_host_reconnect(tgt, 20);
                    else try_connect_host();
                }
            }
            if (g_host_connected && g_host_hid_cid) {
                if (is_press) {
                    int hold_mode = g_remote_wake_grace_active ? 0 : 1;
                    if (send_host_named_key_ex(cmd, hold_mode)) {
                        snprintf(msg, sizeof(msg), "Dispatched device_cmd %s to BT host: dev=%s cmd=%s%s", hold_mode ? "hold" : "tap", target, cmd, g_remote_wake_grace_active ? " (wake-grace tap)" : "");
                        codex_log_debug(msg);
                        return;
                    }
                } else {
                    send_host_release_all();
                    snprintf(msg, sizeof(msg), "Dispatched device_cmd release to BT host: dev=%s cmd=%s", target, cmd);
                    codex_log_debug(msg);
                    return;
                }
            } else {
                snprintf(msg, sizeof(msg), "device_cmd for BT host dev=%s cmd=%s dropped (host connecting/offline)", target, cmd);
                codex_log_debug(msg);
                return;
            }
        }
        if (is_press) {
            char body[256];
            snprintf(body, sizeof(body), "{\"deviceId\":\"%s\",\"command\":\"%s\"}", target, cmd);
            trigger_http_post("/api/ir-send", body);
            snprintf(msg, sizeof(msg), "Dispatched device_cmd: dev=%s cmd=%s", target, cmd);
            codex_log_debug(msg);
        }
    } else if (is_press) {
        if (strcmp(action, "activity_start") == 0 && target[0]) {
            char body[64];
            snprintf(body, sizeof(body), "{\"id\":\"%s\"}", target);
            trigger_http_post("/api/activity-start", body);
            snprintf(msg, sizeof(msg), "Dispatched activity_start: %s", target);
            codex_log_debug(msg);
        } else if (strcmp(action, "activity_stop") == 0) {
            trigger_http_post("/api/activity-stop", "{}");
            codex_log_debug("Dispatched activity_stop");
        } else {
            snprintf(msg, sizeof(msg), "Unhandled mapping: act=%s tgt=%s cmd=%s", action, target, cmd);
            codex_log_debug(msg);
        }
    }
}

static void dispatch_button(const char *btn) {
    dispatch_button_event(btn, 1);
}

static void handle_gatt_notification_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != GATT_EVENT_NOTIFICATION) return;

    uint16_t val_len = gatt_event_notification_get_value_length(packet);
    const uint8_t *val = gatt_event_notification_get_value(packet);
    if (!val || val_len == 0) return;

    if (access(BT_DEBUG_FLAG, F_OK) == 0) {
        char hex_buf[128];
        int hpos = snprintf(hex_buf, sizeof(hex_buf), "GATT notify len=%u: ", val_len);
        for (uint16_t i = 0; i < val_len && i < 16; i++) {
            hpos += snprintf(hex_buf + hpos, sizeof(hex_buf) - hpos, "%02X ", val[i]);
        }
        log_msg(hex_buf);
    }

    if (is_remote_release_packet(val, val_len)) {
        if (g_remote_watchdog_active) {
            btstack_run_loop_remove_timer(&g_remote_watchdog_timer);
            g_remote_watchdog_active = false;
        }
        if (g_active_remote_btn[0]) {
            dispatch_button_event(g_active_remote_btn, 0 /* release */);
            g_active_remote_btn[0] = 0;
        } else {
            send_host_release_all();
        }
        return;
    }

    const char *btn = parse_button(val, val_len);
    if (btn) {
        if (app_state != APP_READY) {
            app_state = APP_READY;
            update_status("ready");
        }
        if (g_active_remote_btn[0] && strcmp(g_active_remote_btn, btn) == 0) {
            // Repeat packet while holding - refresh watchdog timer
            if (g_remote_watchdog_active) {
                btstack_run_loop_remove_timer(&g_remote_watchdog_timer);
            }
            btstack_run_loop_set_timer_handler(&g_remote_watchdog_timer, remote_watchdog_timeout_handler);
            btstack_run_loop_set_timer(&g_remote_watchdog_timer, 10000);
            btstack_run_loop_add_timer(&g_remote_watchdog_timer);
            g_remote_watchdog_active = true;
            return;
        }
        if (g_active_remote_btn[0]) {
            dispatch_button_event(g_active_remote_btn, 0);
            g_active_remote_btn[0] = 0;
        }
        strncpy(g_active_remote_btn, btn, sizeof(g_active_remote_btn) - 1);
        dispatch_button_event(btn, 1 /* press/hold */);

        if (g_remote_watchdog_active) {
            btstack_run_loop_remove_timer(&g_remote_watchdog_timer);
        }
        btstack_run_loop_set_timer_handler(&g_remote_watchdog_timer, remote_watchdog_timeout_handler);
        btstack_run_loop_set_timer(&g_remote_watchdog_timer, 10000);
        btstack_run_loop_add_timer(&g_remote_watchdog_timer);
        g_remote_watchdog_active = true;
    }
}

static void handle_gatt_client_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(size);

    if (hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META) return;

    switch (hci_event_gattservice_meta_get_subevent_code(packet)) {
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED: {
            uint8_t status = gattservice_subevent_hid_service_connected_get_status(packet);
            if (status == ERROR_CODE_SUCCESS) {
                log_msg("HIDS client connected successfully");
                g_remote_wake_grace_active = true;
                btstack_run_loop_remove_timer(&g_remote_wake_grace_timer);
                btstack_run_loop_set_timer_handler(&g_remote_wake_grace_timer, remote_wake_grace_timeout_handler);
                btstack_run_loop_set_timer(&g_remote_wake_grace_timer, 2500);
                btstack_run_loop_add_timer(&g_remote_wake_grace_timer);
                app_state = APP_READY;
                update_status("ready");
                if (tlv_impl) {
                    tlv_impl->store_tag(&tlv_context, TLV_TAG_HOGD, (const uint8_t *)&remote_device, sizeof(remote_device));
                }
            } else {
                char msg[64];
                snprintf(msg, sizeof(msg), "HIDS client connection failed (status=0x%02x)", status);
                log_msg(msg);
                gap_disconnect(connection_handle);
                schedule_reconnect(2000);
            }
            break;
        }
        case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED:
            log_msg("HIDS client disconnected");
            g_remote_wake_grace_active = true;
            btstack_run_loop_remove_timer(&g_remote_wake_grace_timer);
            if (g_remote_watchdog_active) {
                btstack_run_loop_remove_timer(&g_remote_watchdog_timer);
                g_remote_watchdog_active = false;
            }
            if (g_active_remote_btn[0]) {
                dispatch_button_event(g_active_remote_btn, 0);
                g_active_remote_btn[0] = 0;
            } else {
                send_host_release_all();
            }
            app_state = APP_IDLE;
            update_status("disconnected");
            schedule_reconnect(1000);
            break;

        case GATTSERVICE_SUBEVENT_HID_REPORT: {
            const uint8_t *report = gattservice_subevent_hid_report_get_report(packet);
            uint16_t report_len = gattservice_subevent_hid_report_get_report_len(packet);
            if (access(BT_DEBUG_FLAG, F_OK) == 0) {
                char hex_buf[128];
                int hpos = snprintf(hex_buf, sizeof(hex_buf), "HID report len=%u: ", report_len);
                for (uint16_t i = 0; i < report_len && i < 16; i++) {
                    hpos += snprintf(hex_buf + hpos, sizeof(hex_buf) - hpos, "%02X ", report[i]);
                }
                log_msg(hex_buf);
            }

            if (is_remote_release_packet(report, report_len)) {
                if (g_remote_watchdog_active) {
                    btstack_run_loop_remove_timer(&g_remote_watchdog_timer);
                    g_remote_watchdog_active = false;
                }
                if (g_active_remote_btn[0]) {
                    dispatch_button_event(g_active_remote_btn, 0 /* release */);
                    g_active_remote_btn[0] = 0;
                } else {
                    send_host_release_all();
                }
                break;
            }

            const char *btn = parse_button(report, report_len);
            if (btn) {
                if (g_active_remote_btn[0]) {
                    dispatch_button_event(g_active_remote_btn, 0);
                    g_active_remote_btn[0] = 0;
                }
                strncpy(g_active_remote_btn, btn, sizeof(g_active_remote_btn) - 1);
                dispatch_button(btn);
            }
            break;
        }
        default:
            break;
    }
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;

    uint8_t event = hci_event_packet_get_type(packet);
    switch (event) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                log_msg("BTstack HCI state: WORKING");

                // Classic GAP configuration
                gap_set_local_name(g_host_name);
                gap_set_class_of_device(0x0025C0); // Peripheral: Keyboard + Pointing Device
                gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH | LM_LINK_POLICY_ENABLE_SNIFF_MODE);
                gap_set_allow_role_switch(true);
                gap_ssp_set_enable(1);
                gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
                gap_set_security_mode(GAP_SECURITY_MODE_2);
                gap_connectable_control(1);
                gap_discoverable_control(0); // Off until requested via WebUI or command
                hci_disable_l2cap_timeout_check();

                // Fast scan (30ms/30ms), 7.5-15ms conn interval, latency 0, 2.0s supervision timeout
                gap_set_connection_parameters(48, 48, 6, 12, 0, 200, 0, 0);
                init_known_hosts();
                update_status("initializing");
                hog_start_connect();

                char cur_act[64] = "-1";
                read_current_activity(cur_act, sizeof(cur_act));
                if (cur_act[0] && strcmp(cur_act, "-1") != 0 && strcasecmp(cur_act, "poweroff") != 0) {
                    bd_addr_t tgt;
                    if (get_pinned_host_target(tgt)) {
                        char bmsg[128];
                        snprintf(bmsg, sizeof(bmsg), "Boot reconnect: reconnecting to host %s (activity=%s)",
                                 bd_addr_to_str(tgt), cur_act);
                        log_msg(bmsg);
                        start_host_reconnect(tgt, MAX_HOST_RECONNECT_ATTEMPTS);
                    } else {
                        log_msg("Boot reconnect: resuming connect for active activity");
                        try_connect_host();
                    }
                } else {
                    log_msg("Boot: Hub idle/poweroff, skipping Bluetooth host connect");
                }
            }
            break;

        case GAP_EVENT_ADVERTISING_REPORT: {
            if (app_state != APP_SCANNING) break;
            bd_addr_t adv_addr;
            gap_event_advertising_report_get_address(packet, adv_addr);
            uint8_t adv_addr_type = gap_event_advertising_report_get_address_type(packet);
            const uint8_t *ad_data = gap_event_advertising_report_get_data(packet);
            uint8_t ad_len = gap_event_advertising_report_get_data_length(packet);
            int8_t rssi = gap_event_advertising_report_get_rssi(packet);

            char dev_name[64] = "";
            uint8_t flags = 0;
            uint16_t appearance = 0;
            bool has_hid = ad_data_contains_uuid16(ad_len, ad_data, ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE);

            // Extract advertising items
            ad_context_t context;
            for (ad_iterator_init(&context, ad_len, ad_data); ad_iterator_has_more(&context); ad_iterator_next(&context)) {
                uint8_t data_type = ad_iterator_get_data_type(&context);
                uint8_t data_len  = ad_iterator_get_data_len(&context);
                const uint8_t *data = ad_iterator_get_data(&context);
                if (data_type == 0x01 && data_len >= 1) {
                    flags = data[0];
                } else if (data_type == 0x08 || data_type == 0x09) {
                    int copy_len = data_len < (uint8_t)(sizeof(dev_name) - 1) ? data_len : (uint8_t)(sizeof(dev_name) - 1);
                    memcpy(dev_name, data, copy_len);
                    dev_name[copy_len] = 0;
                } else if (data_type == 0x19 && data_len >= 2) {
                    appearance = data[0] | (data[1] << 8);
                }
            }

            bool is_hid = has_hid || (appearance >= 0x03C0 && appearance <= 0x03C4);
            if (!is_hid && dev_name[0]) {
                if (strstr(dev_name, "Remote") || strstr(dev_name, "B25") ||
                    strstr(dev_name, "Voice") || strstr(dev_name, "Google") ||
                    strstr(dev_name, "Homatics") || strstr(dev_name, "BleRemote") ||
                    strstr(dev_name, "Keyboard") || strstr(dev_name, "Gamepad") ||
                    strcasestr(dev_name, "harmony") || strcasestr(dev_name, "smart") ||
                    strcasestr(dev_name, "logitech")) {
                    is_hid = true;
                }
            }
            bool in_pairing_mode = ((flags & 0x03) != 0);

            if (g_user_scan_active) {
                // In active user scan: discover all nearby BLE devices/remotes
                int idx = -1;
                for (int i = 0; i < g_discovered_count; i++) {
                    if (memcmp(g_discovered[i].addr, adv_addr, 6) == 0) {
                        idx = i;
                        break;
                    }
                }
                if (idx >= 0) {
                    if (dev_name[0]) strncpy(g_discovered[idx].name, dev_name, sizeof(g_discovered[idx].name) - 1);
                    g_discovered[idx].rssi = rssi;
                } else if (g_discovered_count < MAX_DISCOVERED) {
                    idx = g_discovered_count++;
                    memcpy(g_discovered[idx].addr, adv_addr, 6);
                    g_discovered[idx].addr_type = adv_addr_type;
                    if (dev_name[0]) {
                        strncpy(g_discovered[idx].name, dev_name, sizeof(g_discovered[idx].name) - 1);
                    } else if (is_hid) {
                        strcpy(g_discovered[idx].name, "Bluetooth Remote (HID)");
                    } else {
                        snprintf(g_discovered[idx].name, sizeof(g_discovered[idx].name),
                                 "BLE Device (%s)", bd_addr_to_str(adv_addr));
                    }
                    g_discovered[idx].rssi = rssi;
                    char sbuf[128];
                    snprintf(sbuf, sizeof(sbuf), "Discovered BLE device: %s ('%s') rssi=%d",
                             bd_addr_to_str(adv_addr), g_discovered[idx].name, rssi);
                    log_msg(sbuf);
                    write_scan_results(true);
                }
                break;
            }

            bool match = false;
            bd_addr_t target_mac;
            if (get_pinned_target(target_mac)) {
                if (memcmp(adv_addr, target_mac, 6) == 0) {
                    match = true;
                }
            } else if (is_hid && (in_pairing_mode || dev_name[0])) {
                match = true;
            }

            if (match) {
                memcpy(remote_device.addr, adv_addr, 6);
                remote_device.addr_type = adv_addr_type;
                char log_buf[160];
                snprintf(log_buf, sizeof(log_buf), "Connecting to BLE remote: %s ('%s')",
                         bd_addr_to_str(remote_device.addr), dev_name);
                log_msg(log_buf);
                hog_connect();
            }
            break;
        }

        case HCI_EVENT_META_GAP:
            if (hci_event_gap_meta_get_subevent_code(packet) == GAP_SUBEVENT_LE_CONNECTION_COMPLETE) {
                btstack_run_loop_remove_timer(&reconnect_timer);
                connection_handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
                char msg[64];
                snprintf(msg, sizeof(msg), "LE Connection complete (handle=0x%04x)", connection_handle);
                log_msg(msg);
                // Listen for GATT notifications immediately
                gatt_client_listen_for_characteristic_value_updates(&gatt_notification_listener,
                                                                   handle_gatt_notification_event,
                                                                   connection_handle, NULL);
                app_state = APP_ENCRYPTING;
                update_status("connecting");
                gap_request_connection_parameter_update(connection_handle, 6, 12, 0, 200);
                sm_request_pairing(connection_handle);
            }
            break;

        case HCI_EVENT_CONNECTION_REQUEST: {
            bd_addr_t req_addr;
            hci_event_connection_request_get_bd_addr(packet, req_addr);
            uint8_t link_type = hci_event_connection_request_get_link_type(packet);
            char msg[96];
            snprintf(msg, sizeof(msg), "Classic BT: Connection Request from %s (link_type=%u)", bd_addr_to_str(req_addr), link_type);
            log_msg(msg);
            break;
        }

        case HCI_EVENT_CONNECTION_COMPLETE: {
            uint8_t status = hci_event_connection_complete_get_status(packet);
            hci_con_handle_t handle = hci_event_connection_complete_get_connection_handle(packet);
            bd_addr_t conn_addr;
            hci_event_connection_complete_get_bd_addr(packet, conn_addr);
            char msg[128];
            snprintf(msg, sizeof(msg), "Classic BT: Connection Complete: %s handle=0x%04x status=0x%02x", bd_addr_to_str(conn_addr), handle, status);
            log_msg(msg);
            if (status == 0) {
                host_conn_t *h = register_host_target(conn_addr, NULL);
                if (h) {
                    h->acl_handle = handle;
                }
            } else {
                for (int i = 0; i < MAX_HOST_CONNS; i++) {
                    if (memcmp(g_hosts[i].addr, conn_addr, 6) == 0) {
                        g_hosts[i].connected = false;
                        g_hosts[i].hid_cid = 0;
                        g_hosts[i].acl_handle = HCI_CON_HANDLE_INVALID;
                        break;
                    }
                }
                g_host_connected = is_any_host_connected();
                g_host_hid_cid = get_target_host_hid_cid();
                update_status(app_state == APP_READY ? "ready" : "standby");
                update_bthid_status();
            }
            break;
        }

        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            hci_con_handle_t disconn_handle = hci_event_disconnection_complete_get_connection_handle(packet);
            uint8_t disconn_reason = hci_event_disconnection_complete_get_reason(packet);
            if (disconn_handle == connection_handle && connection_handle != HCI_CON_HANDLE_INVALID) {
                log_msg("LE Remote Disconnection complete");
                g_remote_wake_grace_active = true;
                btstack_run_loop_remove_timer(&g_remote_wake_grace_timer);
                if (g_remote_watchdog_active) {
                    btstack_run_loop_remove_timer(&g_remote_watchdog_timer);
                    g_remote_watchdog_active = false;
                }
                if (g_active_remote_btn[0]) {
                    dispatch_button_event(g_active_remote_btn, 0);
                    g_active_remote_btn[0] = 0;
                } else {
                    send_host_release_all();
                }
                gatt_client_stop_listening_for_characteristic_value_updates(&gatt_notification_listener);
                connection_handle = HCI_CON_HANDLE_INVALID;
                app_state = APP_IDLE;
                update_status("disconnected");
                schedule_reconnect(10); // Instant scan resume
            } else {
                g_host_connecting = false;
                char msg[128];
                snprintf(msg, sizeof(msg), "Classic ACL disconnect: handle=0x%04x reason=0x%02x", disconn_handle, disconn_reason);
                log_msg(msg);
                bool host_cleared = false;
                for (int i = 0; i < MAX_HOST_CONNS; i++) {
                    if (g_hosts[i].acl_handle == disconn_handle || (disconn_handle != HCI_CON_HANDLE_INVALID && g_hosts[i].connected)) {
                        g_hosts[i].connected = false;
                        g_hosts[i].hid_cid = 0;
                        g_hosts[i].acl_handle = HCI_CON_HANDLE_INVALID;
                        host_cleared = true;
                    }
                }
                if (host_cleared) {
                    g_host_connected = is_any_host_connected();
                    g_host_hid_cid = get_target_host_hid_cid();
                    update_status(app_state == APP_READY ? "ready" : "standby");
                    update_bthid_status();
                }
            }
            break;
        }

        case HCI_EVENT_PIN_CODE_REQUEST: {
            bd_addr_t pin_addr;
            hci_event_pin_code_request_get_bd_addr(packet, pin_addr);
            log_msg("Classic BT: PIN code requested -> sending default '0000'");
            gap_pin_code_response(pin_addr, "0000");
            break;
        }

        case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
            bd_addr_t conf_addr;
            hci_event_user_confirmation_request_get_bd_addr(packet, conf_addr);
            log_msg("Classic BT: SSP User Confirmation Request -> auto-accepting");
            gap_ssp_confirmation_response(conf_addr);
            break;
        }

        case HCI_EVENT_USER_PASSKEY_REQUEST: {
            bd_addr_t pass_addr;
            hci_event_user_passkey_request_get_bd_addr(packet, pass_addr);
            log_msg("Classic BT: SSP Passkey Request -> auto-replying 0");
            gap_ssp_passkey_response(pass_addr, 0);
            break;
        }

        case HCI_EVENT_LINK_KEY_NOTIFICATION: {
            bd_addr_t lk_addr;
            reverse_bd_addr(&packet[2], lk_addr);
            memcpy(g_host_addr, lk_addr, sizeof(bd_addr_t));
            if (tlv_impl) {
                tlv_impl->store_tag(&tlv_context, TLV_TAG_HIDR, (const uint8_t *)lk_addr, sizeof(bd_addr_t));
            }
            char msg[96];
            snprintf(msg, sizeof(msg), "Classic BT: Link Key saved for %s", bd_addr_to_str(lk_addr));
            log_msg(msg);
            break;
        }

        case HCI_EVENT_SIMPLE_PAIRING_COMPLETE: {
            uint8_t status = hci_event_simple_pairing_complete_get_status(packet);
            bd_addr_t pair_addr;
            hci_event_simple_pairing_complete_get_bd_addr(packet, pair_addr);
            char msg[96];
            snprintf(msg, sizeof(msg), "Classic BT: Simple Pairing Complete for %s (status=0x%02x)", bd_addr_to_str(pair_addr), status);
            log_msg(msg);
            break;
        }

        case GAP_EVENT_DEDICATED_BONDING_COMPLETED: {
            uint8_t status = packet[2];
            bd_addr_t b_addr;
            reverse_bd_addr(&packet[3], b_addr);
            char msg[96];
            snprintf(msg, sizeof(msg), "Classic BT: Dedicated Bonding Complete for %s (status=0x%02x)", bd_addr_to_str(b_addr), status);
            log_msg(msg);
            break;
        }

        case HCI_EVENT_HID_META: {
            uint8_t subevent = hci_event_hid_meta_get_subevent_code(packet);
            switch (subevent) {
                case HID_SUBEVENT_CONNECTION_OPENED: {
                    uint8_t status = hid_subevent_connection_opened_get_status(packet);
                    uint16_t cid = hid_subevent_connection_opened_get_hid_cid(packet);
                    bd_addr_t conn_addr;
                    hid_subevent_connection_opened_get_bd_addr(packet, conn_addr);

                    if (status != ERROR_CODE_SUCCESS) {
                        g_host_connecting = false;
                        char msg[128];
                        snprintf(msg, sizeof(msg), "Classic HID Host connection to %s failed (0x%02x)",
                                 bd_addr_to_str(conn_addr), status);
                        log_msg(msg);
                        for (int i = 0; i < MAX_HOST_CONNS; i++) {
                            if (memcmp(g_hosts[i].addr, conn_addr, 6) == 0 || g_hosts[i].hid_cid == cid) {
                                g_hosts[i].connected = false;
                                g_hosts[i].hid_cid = 0;
                                if (g_hosts[i].acl_handle != HCI_CON_HANDLE_INVALID) {
                                    gap_disconnect(g_hosts[i].acl_handle);
                                    g_hosts[i].acl_handle = HCI_CON_HANDLE_INVALID;
                                }
                            }
                        }
                        g_host_connected = is_any_host_connected();
                        g_host_hid_cid = get_target_host_hid_cid();
                        update_status(app_state == APP_READY ? "ready" : "standby");
                        update_bthid_status();
                        char cur_act[64] = "-1";
                        read_current_activity(cur_act, sizeof(cur_act));
                        if (cur_act[0] && strcmp(cur_act, "-1") != 0 && strcasecmp(cur_act, "poweroff") != 0) {
                            if (!g_host_reconnect_active && !is_null_bd_addr(conn_addr)) {
                                start_host_reconnect(conn_addr, MAX_HOST_RECONNECT_ATTEMPTS);
                            }
                        } else {
                            log_msg("Host disconnected while Activity is PowerOff, suppressing reconnect");
                        }
                        break;
                    }

                    g_host_connecting = false;
                    stop_host_reconnect();
                    g_host_connected = true;
                    g_host_pairing = false;
                    gap_discoverable_control(0);
                    g_host_hid_cid = cid;
                    memcpy(g_host_addr, conn_addr, 6);

                    host_conn_t *h = register_host_target(conn_addr, NULL);
                    if (h) {
                        h->connected = true;
                        h->hid_cid = cid;
                    }

                    char msg[128];
                    snprintf(msg, sizeof(msg), "Classic HID Host CONNECTED: %s (cid=0x%04x)",
                             bd_addr_to_str(conn_addr), cid);
                    log_msg(msg);

                    FILE *pf = fopen("/tmp/bt_pending_pair_device", "r");
                    if (pf) {
                        char link_dev_id[64] = {0};
                        if (fgets(link_dev_id, sizeof(link_dev_id), pf)) {
                            char *nl = strpbrk(link_dev_id, "\r\n");
                            if (nl) *nl = 0;
                            if (link_dev_id[0]) {
                                link_bt_device_to_addr(link_dev_id, bd_addr_to_str(conn_addr));
                            }
                        }
                        fclose(pf);
                        unlink("/tmp/bt_pending_pair_device");
                    }

                    if (tlv_impl) {
                        tlv_impl->store_tag(&tlv_context, TLV_TAG_HIDR, (const uint8_t *)conn_addr, sizeof(bd_addr_t));
                    }
                    FILE *hf = fopen("/data/codex/bthid_host_target", "w");
                    if (hf) {
                        fprintf(hf, "bdaddr=%s\n", bd_addr_to_str(conn_addr));
                        fclose(hf);
                    }
                    FILE *btf = fopen("/data/codex/bthid_target", "w");
                    if (btf) {
                        fprintf(btf, "type=btkeyboard\nbdaddr=%s\n", bd_addr_to_str(conn_addr));
                        fclose(btf);
                    }
                    update_status(app_state == APP_READY ? "ready" : "standby");
                    update_bthid_status();

                    if (g_pending_wake_btn[0] && (time(NULL) - g_pending_wake_time) < 10) {
                        char pbtn[32];
                        strncpy(pbtn, g_pending_wake_btn, sizeof(pbtn) - 1);
                        pbtn[sizeof(pbtn) - 1] = 0;
                        g_pending_wake_btn[0] = 0;
                        uint8_t mod = 0, kc = 0;
                        if (sscanf(pbtn, "KEY %hhx %hhx", &mod, &kc) == 2) {
                            send_host_keyboard_report(mod, kc);
                            log_msg("Dispatched buffered wake keyboard report to host");
                        } else if (strncmp(pbtn, "CONSUMER ", 9) == 0) {
                            unsigned int usage = 0;
                            if (sscanf(pbtn + 9, "%x", &usage) == 1) {
                                send_host_consumer_report((uint16_t)usage);
                                log_msg("Dispatched buffered wake consumer report to host");
                            }
                        }
                    } else {
                        g_pending_wake_btn[0] = 0;
                    }
                    break;
                }
                case HID_SUBEVENT_CONNECTION_CLOSED: {
                    g_host_connecting = false;
                    uint16_t cid = hid_subevent_connection_closed_get_hid_cid(packet);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "Classic HID Host DISCONNECTED (cid=0x%04x)", cid);
                    log_msg(msg);
                    bool matched = false;
                    for (int i = 0; i < MAX_HOST_CONNS; i++) {
                        if (cid != 0 && g_hosts[i].hid_cid == cid) {
                            g_hosts[i].connected = false;
                            g_hosts[i].hid_cid = 0;
                            g_hosts[i].acl_handle = HCI_CON_HANDLE_INVALID;
                            matched = true;
                            break;
                        }
                    }
                    if (!matched && cid == 0) {
                        for (int i = 0; i < MAX_HOST_CONNS; i++) {
                            g_hosts[i].connected = false;
                            g_hosts[i].hid_cid = 0;
                            g_hosts[i].acl_handle = HCI_CON_HANDLE_INVALID;
                        }
                    }
                    g_host_connected = is_any_host_connected();
                    g_host_hid_cid = get_target_host_hid_cid();
                    update_status(app_state == APP_READY ? "ready" : "standby");
                    update_bthid_status();
                    break;
                }
                default:
                    break;
            }
            break;
        }

        default:
            break;
    }
}

static void sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;

    bd_addr_t addr;
    uint8_t addr_type;

    switch (hci_event_packet_get_type(packet)) {
        case SM_EVENT_JUST_WORKS_REQUEST:
            log_msg("SM: Confirming Just Works pairing request");
            sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
            break;
        case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
            sm_numeric_comparison_confirm(sm_event_numeric_comparison_request_get_handle(packet));
            break;
        case SM_EVENT_PAIRING_COMPLETE: {
            uint8_t status = sm_event_pairing_complete_get_status(packet);
            if (status == ERROR_CODE_SUCCESS) {
                log_msg("SMP pairing & bonding complete! Connecting HIDS...");
                app_state = APP_W4_HIDS;
                hids_host_connect(connection_handle, handle_gatt_client_event, protocol_mode, &hids_cid);
            } else {
                uint8_t reason = sm_event_pairing_complete_get_reason(packet);
                char msg[64];
                snprintf(msg, sizeof(msg), "SMP pairing failed (status=0x%02x, reason=0x%02x)", status, reason);
                log_msg(msg);
                gap_disconnect(connection_handle);
                schedule_reconnect(2000);
            }
            break;
        }
        case SM_EVENT_REENCRYPTION_STARTED: {
            sm_event_reencryption_complete_get_address(packet, addr);
            char msg[128];
            snprintf(msg, sizeof(msg), "Bonding info exists for %s -> re-encrypting link", bd_addr_to_str(addr));
            log_msg(msg);
            break;
        }
        case SM_EVENT_REENCRYPTION_COMPLETE: {
            uint8_t status = sm_event_reencryption_complete_get_status(packet);
            if (status == ERROR_CODE_SUCCESS) {
                log_msg("Link re-encryption complete -> Remote READY");
                app_state = APP_READY;
                update_status("ready");
            } else if (status == ERROR_CODE_PIN_OR_KEY_MISSING) {
                log_msg("Re-encryption failed: key missing on remote. Deleting local bond and re-pairing...");
                sm_event_reencryption_complete_get_address(packet, addr);
                addr_type = sm_event_reencryption_started_get_addr_type(packet);
                gap_delete_bonding(addr_type, addr);
                sm_request_pairing(sm_event_reencryption_complete_get_handle(packet));
            } else {
                char msg[64];
                snprintf(msg, sizeof(msg), "Re-encryption failed (status=0x%02x)", status);
                log_msg(msg);
                gap_disconnect(connection_handle);
                schedule_reconnect(2000);
            }
            break;
        }
        default:
            break;
    }
}

static void sig_handler(int sig) {
    UNUSED(sig);
    g_running = 0;
    btstack_run_loop_trigger_exit();
}

int main(int argc, const char *argv[]) {
    UNUSED(argc);
    UNUSED(argv);

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    FILE *pf = fopen(PID_FILE, "w");
    if (pf) { fprintf(pf, "%d\n", (int)getpid()); fclose(pf); }

    log_msg("=== Starting Codex BTstack Daemon ===");
    update_status("starting");
    update_bthid_status();

    // 1. Initialize BTstack memory and run loop
    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_posix_get_instance());

    // 2. Initialize Linux HCI Transport & Stack FIRST
    hci_transport_config_linux_t transport_config = {
        .type = HCI_TRANSPORT_CONFIG_LINUX,
        .device_id = 0
    };
    hci_init(hci_transport_linux_instance(), &transport_config);

    // 3. Set up TLV database for persistent bonding (LE + Classic Link Keys) AFTER hci_init
    tlv_impl = btstack_tlv_posix_init_instance(&tlv_context, TLV_DB_PATH);
    btstack_tlv_set_instance(tlv_impl, &tlv_context);
    le_device_db_tlv_configure(tlv_impl, &tlv_context);
    hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(tlv_impl, &tlv_context));

    if (tlv_impl) {
        int len = tlv_impl->get_tag(&tlv_context, TLV_TAG_HIDR, (uint8_t *)g_host_addr, sizeof(bd_addr_t));
        if (len == (int)sizeof(bd_addr_t)) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Loaded paired Host BD_ADDR from TLV: %s", bd_addr_to_str(g_host_addr));
            log_msg(msg);
        }
    }

    // Enable packet dump only if explicit packet dump flag exists
    if (access("/data/codex/bt_packet_dump.conf", F_OK) == 0) {
        hci_dump_init(hci_dump_posix_stdout_get_instance());
    }

    // 4. Initialize L2CAP and GATT Client
    l2cap_init();
    gatt_client_init();

    // 6. Security Manager (SMP Legacy Just Works / Bonding for BLE)
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_BONDING);
    sm_set_encryption_key_size_range(7, 16);

    // 7. SDP Server & Records
    sdp_init();

    // 7a. HID Device SDP record
    memset(hid_service_buffer, 0, sizeof(hid_service_buffer));
    hid_sdp_record_t hid_params = {
        .hid_device_subclass = 0x25C0,
        .hid_country_code = 33, // US
        .hid_virtual_cable = 0,
        .hid_remote_wake = 1,
        .hid_reconnect_initiate = 1,
        .hid_normally_connectable = 1,
        .hid_boot_device = 1,
        .hid_ssr_host_max_latency = 1600,
        .hid_ssr_host_min_timeout = 3200,
        .hid_supervision_timeout = 3200,
        .hid_descriptor = hid_descriptor_composite,
        .hid_descriptor_size = sizeof(hid_descriptor_composite),
        .device_name = g_host_name
    };
    hid_create_sdp_record(hid_service_buffer, sdp_create_service_record_handle(), &hid_params);
    btstack_assert(de_get_len(hid_service_buffer) <= sizeof(hid_service_buffer));
    sdp_register_service(hid_service_buffer);

    // 7b. Device ID SDP record (Logitech 0x046D)
    memset(device_id_sdp_service_buffer, 0, sizeof(device_id_sdp_service_buffer));
    device_id_create_sdp_record(device_id_sdp_service_buffer, sdp_create_service_record_handle(),
                               DEVICE_ID_VENDOR_ID_SOURCE_USB, 0x046D, 0xC52B, 0x0100);
    btstack_assert(de_get_len(device_id_sdp_service_buffer) <= sizeof(device_id_sdp_service_buffer));
    sdp_register_service(device_id_sdp_service_buffer);

    char sdp_msg[96];
    snprintf(sdp_msg, sizeof(sdp_msg), "SDP registered: HID %u bytes, DeviceID %u bytes",
             de_get_len(hid_service_buffer), de_get_len(device_id_sdp_service_buffer));
    log_msg(sdp_msg);

    // 8. Classic HID Device service
    hid_device_init(1, sizeof(hid_descriptor_composite), hid_descriptor_composite);
    hid_device_register_packet_handler(&packet_handler);

    // 9. BLE HID Service Host (for physical remote)
    hids_host_init(hid_descriptor_storage, sizeof(hid_descriptor_storage));

    // 10. Packet callbacks
    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    sm_event_callback_registration.callback = &sm_packet_handler;
    sm_add_event_handler(&sm_event_callback_registration);

    // 11. Command & FIFO polling timer (100ms) and IPC socket
    poll_bthid_fifo();
    update_status("initializing");
    update_bthid_status();
    btstack_run_loop_set_timer(&cmd_poll_timer, 100);
    btstack_run_loop_set_timer_handler(&cmd_poll_timer, &cmd_poll_timeout);
    btstack_run_loop_add_timer(&cmd_poll_timer);

    init_cmd_socket();
    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);

    // 12. Power on
    app_state = APP_W4_WORKING;
    int pwr_err = hci_power_control(HCI_POWER_ON);
    if (pwr_err != 0) {
        log_msg("FATAL: hci_power_control failed, exiting");
        fprintf(stderr, "[codex_btstack] FATAL: hci_power_control failed (%d), exiting\n", pwr_err);
        cleanup_cmd_socket();
        if (g_bthid_fifo_fd >= 0) {
            close(g_bthid_fifo_fd);
            g_bthid_fifo_fd = -1;
        }
        unlink(BTHID_STATUS_FILE);
        unlink(PID_FILE);
        return 1;
    }

    // 13. Run loop
    btstack_run_loop_execute();

    cleanup_cmd_socket();
    if (g_bthid_fifo_fd >= 0) {
        close(g_bthid_fifo_fd);
        g_bthid_fifo_fd = -1;
    }
    unlink(BTHID_STATUS_FILE);
    hci_power_control(HCI_POWER_OFF);
    unlink(PID_FILE);
    return 0;
}
