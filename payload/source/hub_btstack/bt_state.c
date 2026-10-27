#include "bt_state.h"

app_state_t app_state = APP_IDLE;
le_device_addr_t remote_device;
hci_con_handle_t connection_handle = HCI_CON_HANDLE_INVALID;
uint16_t hids_cid = 0;
hid_protocol_mode_t protocol_mode = HID_PROTOCOL_MODE_REPORT;
uint8_t hid_descriptor_storage[1000];

btstack_timer_source_t reconnect_timer;
btstack_tlv_posix_t tlv_context;
const btstack_tlv_t * tlv_impl = NULL;

btstack_packet_callback_registration_t hci_event_callback_registration;
btstack_packet_callback_registration_t sm_event_callback_registration;
gatt_client_notification_t gatt_notification_listener;

host_conn_t g_hosts[MAX_HOST_CONNS];
int g_active_host_idx = 0;
uint16_t g_host_hid_cid = 0;
bd_addr_t g_host_addr;
bool g_host_connected = false;
bool g_host_pairing = false;
char g_host_name[64] = "Harmony Keyboard";
uint8_t hid_service_buffer[1024];
uint8_t device_id_sdp_service_buffer[512];
int g_bthid_fifo_fd = -1;
unsigned long g_keys_sent = 0;

btstack_timer_source_t g_host_reconnect_timer;
bool g_host_reconnect_active = false;
int g_host_reconnect_attempts = 0;
bd_addr_t g_host_reconnect_target;
bool g_host_connecting = false;
time_t g_host_connecting_start = 0;
char g_pending_wake_btn[32] = {0};
time_t g_pending_wake_time = 0;

char g_last_btn[64] = "";
time_t g_last_seen = 0;
volatile sig_atomic_t g_running = 1;

const uint8_t hid_descriptor_composite[] = {
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
const size_t hid_descriptor_composite_len = sizeof(hid_descriptor_composite);

bool is_debug_log_enabled(void) {
    return (access(DEBUG_LOG_CONFIG, F_OK) == 0 || access(BT_DEBUG_FLAG, F_OK) == 0);
}

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

void hub_bt_log_debug(const char *msg) {
    if (is_debug_log_enabled()) {
        log_msg(msg);
    }
}

int parse_bd_addr(const char *str, bd_addr_t addr) {
    int b[6];
    if (sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
        for (int i = 0; i < 6; i++) addr[i] = (uint8_t)b[i];
        return 0;
    }
    return -1;
}

int is_null_bd_addr(const bd_addr_t addr) {
    return (addr[0] == 0 && addr[1] == 0 && addr[2] == 0 &&
            addr[3] == 0 && addr[4] == 0 && addr[5] == 0);
}
