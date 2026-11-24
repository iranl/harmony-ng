#include "bt_adv.h"
#include "ble_adv_payload.h"
#include "bt_le_pair_window.h"
#include <ctype.h>

#define ADV_QUEUE_LEN    8
#define ADV_DEFAULT_MS   1200
#define ADV_MIN_MS       200
#define ADV_MAX_MS       10000
// 100 ms in 0.625 ms units: the shortest interval legacy non-connectable advertising allows
#define ADV_INTERVAL     0x00A0
#define ADV_NONCONN_IND  0x03
#define ADV_ALL_CHANNELS 0x07
// a broadcast from another address waits this long, again, while the remote's link is set up
#define ADV_ADDR_RETRY_MS 500
// and is dropped after this: a wake sent two minutes late turns a device on at the wrong time
#define ADV_ADDR_WAIT_MAX_MS 10000

typedef struct {
    uint8_t data[BLE_ADV_MAX_LEN];
    uint8_t len;
    uint16_t ms;
    bool has_addr;   /* sent from this address (as the random address), not the Hub's */
    bd_addr_t addr;
    bool waiting;       /* it had to wait for the remote's link at the head of the queue */
    uint32_t wait_ms;   /* run loop time of its first wait (unsigned differences: no wrap issue) */
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

static void adv_retry_handler(btstack_timer_source_t *ts) {
    UNUSED(ts);
    if (!g_adv_active) adv_start_next();
}

/* Setting the random address while the remote's connection is set up (or its pairing window is
 * open) makes BTstack resend the connect from that address, and a pairing would hand it out as
 * the Hub's identity: a broadcast from another address waits until the link is up or given up */
static bool remote_link_setup(void) {
    return app_state == APP_CONNECTING || app_state == APP_ENCRYPTING || app_state == APP_W4_HIDS ||
           le_pair_window_allows(remote_device.addr, le_pair_now_s());
}

static void adv_start_next(void) {
    while (g_adv_count > 0 && g_adv_queue[g_adv_head].has_addr && remote_link_setup()) {
        adv_entry_t *h = &g_adv_queue[g_adv_head];
        /* the 10 s count from its first wait at the head of the queue, not from the queueing:
         * time spent behind other broadcasts before that is no wait for the remote. Once it has
         * waited, time spent while plain broadcasts that moved ahead of it are on air still
         * counts: the remote was connecting all that time too */
        if (!h->waiting) {
            h->waiting = true;
            h->wait_ms = btstack_run_loop_get_time_ms();
        }
        if ((uint32_t)(btstack_run_loop_get_time_ms() - h->wait_ms) > ADV_ADDR_WAIT_MAX_MS) {
            log_msg("BLE broadcast dropped: its address could not be used for 10 s (the remote was connecting or pairing)");
            g_adv_head = (g_adv_head + 1) % ADV_QUEUE_LEN;
            g_adv_count--;
            continue;
        }
        /* a broadcast without addr= behind it goes first (the others keep their order) */
        int k = 1;
        while (k < g_adv_count && g_adv_queue[(g_adv_head + k) % ADV_QUEUE_LEN].has_addr) k++;
        if (k < g_adv_count) {
            adv_entry_t plain = g_adv_queue[(g_adv_head + k) % ADV_QUEUE_LEN];
            for (int i = k; i > 0; i--)
                g_adv_queue[(g_adv_head + i) % ADV_QUEUE_LEN] = g_adv_queue[(g_adv_head + i - 1) % ADV_QUEUE_LEN];
            g_adv_queue[g_adv_head] = plain;
            break;
        }
        btstack_run_loop_remove_timer(&g_adv_timer);
        btstack_run_loop_set_timer_handler(&g_adv_timer, &adv_retry_handler);
        btstack_run_loop_set_timer(&g_adv_timer, ADV_ADDR_RETRY_MS);
        btstack_run_loop_add_timer(&g_adv_timer);
        return;
    }
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

    /* the retry timer of a waiting addr= broadcast may still be armed: BTstack's own add_timer
     * does not take a timer that is already in its list */
    btstack_run_loop_remove_timer(&g_adv_timer);
    btstack_run_loop_set_timer_handler(&g_adv_timer, &adv_timer_handler);
    btstack_run_loop_set_timer(&g_adv_timer, e->ms);
    btstack_run_loop_add_timer(&g_adv_timer);
}

/* Exactly six hex pairs with ':' between them, then the end or a space; all zero refused */
static bool addr_arg_valid(const char *s, bd_addr_t out) {
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2 ? s[i] != ':' : !isxdigit((unsigned char)s[i])) return false;
    }
    if (s[17] && s[17] != ' ') return false;
    if (!sscanf_bd_addr(s, out)) return false;
    static const bd_addr_t zero = {0};
    return memcmp(out, zero, 6) != 0;
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
    if (a && !addr_arg_valid(a + 5, addr)) {
        log_msg("BLE broadcast rejected: addr= is not a Bluetooth address (AA:BB:CC:DD:EE:FF, not all zero)");
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
    e->waiting = false;
    e->wait_ms = 0;
    g_adv_count++;

    char msg[96];
    snprintf(msg, sizeof(msg), "BLE broadcast queued: %d bytes for %d ms%s%s", len, ms,
             a ? " from " : "", a ? bd_addr_to_str(addr) : "");
    hub_bt_log_debug(msg);

    if (!g_adv_active) adv_start_next();
}
