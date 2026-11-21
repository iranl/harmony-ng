#include "bt_adv.h"
#include "ble_adv_payload.h"

#define ADV_QUEUE_LEN    8
#define ADV_DEFAULT_MS   1200
#define ADV_MIN_MS       200
#define ADV_MAX_MS       10000
// 100 ms in 0.625 ms units: the shortest interval legacy non-connectable advertising allows
#define ADV_INTERVAL     0x00A0
#define ADV_NONCONN_IND  0x03
#define ADV_ALL_CHANNELS 0x07

typedef struct {
    uint8_t data[BLE_ADV_MAX_LEN];
    uint8_t len;
    uint16_t ms;
    bool has_addr;   /* sent from this address (as the random address), not the Hub's */
    bd_addr_t addr;
} adv_entry_t;

// Broadcasts run one after another so each receiver hears every packet in an activity sequence
static adv_entry_t g_adv_queue[ADV_QUEUE_LEN];
static int g_adv_head = 0;
static int g_adv_count = 0;
static bool g_adv_active = false;
static bool g_adv_borrowed = false; /* the random address is set to an entry's address */
static btstack_timer_source_t g_adv_timer;

// BTstack keeps a pointer to the advertising data rather than a copy
static uint8_t g_adv_data[BLE_ADV_MAX_LEN];

static void adv_start_next(void);

bool bt_adv_borrowed_addr(void) {
    return g_adv_borrowed;
}

static void adv_timer_handler(btstack_timer_source_t *ts) {
    UNUSED(ts);
    gap_advertisements_enable(0);
    g_adv_active = false;
    if (g_adv_borrowed) {
        /* Back to the public address: LE connections and the next broadcasts use it */
        gap_random_address_set_mode(GAP_RANDOM_ADDRESS_TYPE_OFF);
        g_adv_borrowed = false;
    }
    adv_start_next();
}

static void adv_start_next(void) {
    if (g_adv_count == 0) return;
    adv_entry_t *e = &g_adv_queue[g_adv_head];
    g_adv_head = (g_adv_head + 1) % ADV_QUEUE_LEN;
    g_adv_count--;

    memcpy(g_adv_data, e->data, e->len);
    if (e->has_addr) {
        /* Not gap_random_address_set(): it forces the top two bits to 11 (a static address)
         * and would change a controller's public address */
        gap_random_address_set_mode(GAP_RANDOM_ADDRESS_TYPE_STATIC);
        hci_le_random_address_set(e->addr);
        g_adv_borrowed = true;
    }
    bd_addr_t null_addr = {0};
    gap_advertisements_set_params(ADV_INTERVAL, ADV_INTERVAL, ADV_NONCONN_IND, 0, null_addr, ADV_ALL_CHANNELS, 0);
    gap_advertisements_set_data(e->len, g_adv_data);
    gap_advertisements_enable(1);
    g_adv_active = true;

    btstack_run_loop_set_timer_handler(&g_adv_timer, &adv_timer_handler);
    btstack_run_loop_set_timer(&g_adv_timer, e->ms);
    btstack_run_loop_add_timer(&g_adv_timer);
}

void bt_adv_handle_cmd(const char *args) {
    if (!args) return;
    while (*args == ' ') args++;

    uint8_t data[BLE_ADV_MAX_LEN];
    const char *end = NULL;
    int len = ble_adv_parse_hex(args, data, &end);
    if (len < 0) {
        log_msg("BLE broadcast rejected: payload is not valid advertising data");
        return;
    }

    int ms = ADV_DEFAULT_MS;
    if (end && *end) {
        int req = atoi(end);
        if (req > 0) ms = req;
    }
    bd_addr_t addr;
    const char *a = end ? strstr(end, "addr=") : NULL;
    if (a && !sscanf_bd_addr(a + 5, addr)) {
        log_msg("BLE broadcast rejected: addr= is not a Bluetooth address");
        return;
    }
    if (ms < ADV_MIN_MS) ms = ADV_MIN_MS;
    if (ms > ADV_MAX_MS) ms = ADV_MAX_MS;

    if (g_adv_count == ADV_QUEUE_LEN) {
        log_msg("BLE broadcast dropped: queue full");
        return;
    }
    adv_entry_t *e = &g_adv_queue[(g_adv_head + g_adv_count) % ADV_QUEUE_LEN];
    memcpy(e->data, data, len);
    e->len = (uint8_t)len;
    e->ms = (uint16_t)ms;
    e->has_addr = a != NULL;
    if (a) memcpy(e->addr, addr, 6);
    g_adv_count++;

    char msg[96];
    snprintf(msg, sizeof(msg), "BLE broadcast queued: %d bytes for %d ms%s%s", len, ms,
             a ? " from " : "", a ? bd_addr_to_str(addr) : "");
    hub_bt_log_debug(msg);

    if (!g_adv_active) adv_start_next();
}
