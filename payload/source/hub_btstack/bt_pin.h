#ifndef HARMONY_BT_PIN_H
#define HARMONY_BT_PIN_H

/* Legacy (pre-2.1) PIN pairing as a host pairing the Hub as its keyboard or remote.
 *
 * The first PIN request from a host is answered with 0000 at once, as before: hosts that take
 * it are not delayed. A host that refuses it (the link drops after the 0000 without a link key;
 * a PlayStation 3 shows its own random code for a keyboard) is noted as wanting a PIN, and the
 * user is told to start the pairing again on it. A second PIN request after 0000 without a link
 * key in between is a refusal too. Its next PIN request is held until the user
 * sends the code the host shows (pin_reply), cancels, or PIN_WAIT_MS passes.
 *
 * Pure state, no BTstack: bt_main.c makes the HCI replies, the tests drive it directly. */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Under the LMP response timeout (30 s): the host must still be waiting when the reply goes */
#define PIN_WAIT_MS   25000
#define PIN_MAX_LEN   16
#define PIN_DEFAULT   "0000"
#define PIN_MAX_HOSTS 4

typedef enum {
    PIN_STATE_NONE = 0,
    PIN_STATE_REQUIRED,   /* a host refused 0000: pair again from the host */
    PIN_STATE_WAITING     /* a PIN request is held: send the code the host shows */
} pin_state_t;

typedef enum {
    PIN_ACTION_DEFAULT,   /* answer with PIN_DEFAULT now */
    PIN_ACTION_WAIT       /* hold the request until pin_take_reply / cancel / timeout */
} pin_action_t;

/* A PIN request from addr during host pairing */
pin_action_t pin_on_request(const uint8_t addr[6], uint32_t now_ms);
/* addr got a link key: it paired, with 0000 or with the code */
void pin_on_link_key(const uint8_t addr[6]);
/* addr's link dropped; true when that was a refusal of 0000 (state becomes REQUIRED) */
bool pin_on_disconnect(const uint8_t addr[6]);
/* The digits for the held request: true and the host's address when one waits and pin is
 * 1..PIN_MAX_LEN digits. The request is answered then (state back to NONE). */
bool pin_take_reply(const char *pin, uint8_t addr_out[6]);
/* The user cancelled (or pairing mode ended): true and the address when a request was held,
 * which then gets a negative reply */
bool pin_take_cancel(uint8_t addr_out[6]);
/* The held request's time is up: true and the address (negative reply) */
bool pin_take_timeout(uint32_t now_ms, uint8_t addr_out[6]);
/* Pairing mode ended: forget the hosts that took or refused 0000 and any REQUIRED notice */
void pin_session_end(void);
/* 1..PIN_MAX_LEN ASCII digits */
bool pin_valid(const char *pin);

pin_state_t pin_state(void);
/* The host the state is about (REQUIRED or WAITING) */
const uint8_t *pin_state_addr(void);
const char *pin_state_name(pin_state_t s);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_BT_PIN_H */
