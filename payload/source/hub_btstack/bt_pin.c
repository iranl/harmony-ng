#include "bt_pin.h"

#include <string.h>

/* Hosts that got 0000 in this pairing session and have no link key yet */
static uint8_t s_pin_tried[PIN_MAX_HOSTS][6];
static int s_pin_ntried;
/* Hosts that refused 0000; kept until hub_btstack restarts, so pairing such a host again (after
 * pairing mode timed out, or later) asks for its code at the first try */
static uint8_t s_pin_need[PIN_MAX_HOSTS][6];
static int s_pin_nneed;

static pin_state_t s_pin_state = PIN_STATE_NONE;
static uint8_t s_pin_addr[6];
static uint32_t s_pin_deadline_ms;

static int pin_list_find(uint8_t list[][6], int n, const uint8_t addr[6]) {
    for (int i = 0; i < n; i++) {
        if (memcmp(list[i], addr, 6) == 0) return i;
    }
    return -1;
}

/* Add to a small list; when full, the oldest entry makes room */
static void pin_list_add(uint8_t list[][6], int *n, const uint8_t addr[6]) {
    if (pin_list_find(list, *n, addr) >= 0) return;
    if (*n == PIN_MAX_HOSTS) {
        memmove(list[0], list[1], (size_t)(PIN_MAX_HOSTS - 1) * 6);
        (*n)--;
    }
    memcpy(list[(*n)++], addr, 6);
}

static void pin_list_drop(uint8_t list[][6], int *n, const uint8_t addr[6]) {
    int i = pin_list_find(list, *n, addr);
    if (i < 0) return;
    memmove(list[i], list[i + 1], (size_t)(*n - i - 1) * 6);
    (*n)--;
}

bool pin_valid(const char *pin) {
    if (!pin) return false;
    size_t n = strlen(pin);
    if (n < 1 || n > PIN_MAX_LEN) return false;
    for (size_t i = 0; i < n; i++) {
        if (pin[i] < '0' || pin[i] > '9') return false;
    }
    return true;
}

pin_action_t pin_on_request(const uint8_t addr[6], uint32_t now_ms) {
    if (pin_list_find(s_pin_tried, s_pin_ntried, addr) >= 0) {
        /* it got 0000 and asks again without a link key: it refused 0000 and tries again on the
         * same link (a host that dropped the link is caught by pin_on_disconnect) */
        pin_list_drop(s_pin_tried, &s_pin_ntried, addr);
        pin_list_add(s_pin_need, &s_pin_nneed, addr);
    }
    if (pin_list_find(s_pin_need, s_pin_nneed, addr) >= 0) {
        /* a second request while one is held replaces it: the host started over */
        s_pin_state = PIN_STATE_WAITING;
        memcpy(s_pin_addr, addr, 6);
        s_pin_deadline_ms = now_ms + PIN_WAIT_MS;
        return PIN_ACTION_WAIT;
    }
    pin_list_add(s_pin_tried, &s_pin_ntried, addr);
    return PIN_ACTION_DEFAULT;
}

void pin_on_link_key(const uint8_t addr[6]) {
    pin_list_drop(s_pin_tried, &s_pin_ntried, addr);
    if (s_pin_state != PIN_STATE_NONE && memcmp(s_pin_addr, addr, 6) == 0) s_pin_state = PIN_STATE_NONE;
}

bool pin_on_disconnect(const uint8_t addr[6]) {
    if (s_pin_state == PIN_STATE_WAITING && memcmp(s_pin_addr, addr, 6) == 0) {
        /* the held host gave up: a code sent now would reach nobody */
        s_pin_state = PIN_STATE_REQUIRED;
        return false;
    }
    if (pin_list_find(s_pin_tried, s_pin_ntried, addr) < 0) return false;
    pin_list_drop(s_pin_tried, &s_pin_ntried, addr);
    pin_list_add(s_pin_need, &s_pin_nneed, addr);
    /* a request held for another host stays the one shown */
    if (s_pin_state != PIN_STATE_WAITING) {
        s_pin_state = PIN_STATE_REQUIRED;
        memcpy(s_pin_addr, addr, 6);
    }
    return true;
}

bool pin_take_reply(const char *pin, uint8_t addr_out[6]) {
    if (s_pin_state != PIN_STATE_WAITING || !pin_valid(pin)) return false;
    memcpy(addr_out, s_pin_addr, 6);
    s_pin_state = PIN_STATE_NONE;
    return true;
}

bool pin_take_cancel(uint8_t addr_out[6]) {
    if (s_pin_state != PIN_STATE_WAITING) {
        s_pin_state = PIN_STATE_NONE;
        return false;
    }
    memcpy(addr_out, s_pin_addr, 6);
    s_pin_state = PIN_STATE_NONE;
    /* the user gave up on the code: the next pairing of that host starts with 0000 again */
    pin_list_drop(s_pin_need, &s_pin_nneed, addr_out);
    return true;
}

void pin_forget(const uint8_t addr[6]) {
    pin_list_drop(s_pin_tried, &s_pin_ntried, addr);
    pin_list_drop(s_pin_need, &s_pin_nneed, addr);
    if (s_pin_state != PIN_STATE_NONE && memcmp(s_pin_addr, addr, 6) == 0) s_pin_state = PIN_STATE_NONE;
}

bool pin_take_timeout(uint32_t now_ms, uint8_t addr_out[6]) {
    if (s_pin_state != PIN_STATE_WAITING || (int32_t)(now_ms - s_pin_deadline_ms) < 0) return false;
    memcpy(addr_out, s_pin_addr, 6);
    /* the host gave up too by now; it asks again when the user starts over on it */
    s_pin_state = PIN_STATE_REQUIRED;
    return true;
}

void pin_session_end(void) {
    s_pin_ntried = 0;
    s_pin_state = PIN_STATE_NONE;
}

pin_state_t pin_state(void) {
    return s_pin_state;
}

const uint8_t *pin_state_addr(void) {
    return s_pin_addr;
}

const char *pin_state_name(pin_state_t s) {
    switch (s) {
        case PIN_STATE_REQUIRED: return "required";
        case PIN_STATE_WAITING: return "waiting";
        default: return "none";
    }
}
