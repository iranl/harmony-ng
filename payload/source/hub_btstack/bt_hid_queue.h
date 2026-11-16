#ifndef BT_HID_QUEUE_H
#define BT_HID_QUEUE_H

/* HID reports to a host, in order and never dropped while the host is connected.
 *
 * The Bluetooth stack refuses a report while the controller's ACL buffers are full (or its one
 * outgoing packet buffer is in use); a refused report used to be lost, and a lost key release
 * left the key pressed on the host, which repeated it until the next key. Here a report the stack
 * refuses waits in a small queue per host and goes out when the stack says that host can send
 * again (or the retry timer finds it can); later reports for that host queue behind it, so a
 * press always goes out before its release. Only the queue's logic is in this file; hub_btstack wires it to BTstack. */

#include <stdint.h>

#define HIDQ_HOSTS      4   /* HID host instances in hub_btstack (btstack.patch) */
#define HIDQ_DEPTH      16  /* reports waiting per host */
#define HIDQ_REPORT_MAX 16  /* largest report hub_btstack sends */

/* Sends one report now; returns 0 when the stack took it, anything else when it refused it */
typedef uint8_t (*hidq_send_fn)(uint16_t cid, const uint8_t *report, uint16_t len);
/* Asks the stack for a "can send now" notice for this host */
typedef void (*hidq_request_fn)(uint16_t cid);
/* One log line */
typedef void (*hidq_log_fn)(const char *line);

void hidq_init(hidq_send_fn send, hidq_request_fn request, hidq_log_fn log);

/* Sends the report, or queues it when the stack refuses it or reports are already waiting */
void hidq_send(uint16_t cid, const uint8_t *report, uint16_t len, uint32_t now_ms);

/* Sends what waits for this host, in order, as far as the stack takes it: on the stack's
 * "can send now" notice, and from a short timer while anything waits (a notice can be taken by a
 * control-channel request to the same host and never arrive) */
void hidq_service(uint16_t cid, uint32_t now_ms);
void hidq_service_all(uint32_t now_ms);

/* The host's link closed: its waiting reports go (a host releases all keys when the link drops) */
void hidq_drop(uint16_t cid);

int hidq_pending(uint16_t cid);
int hidq_pending_any(void);
unsigned hidq_waits(void);   /* reports that had to wait, since start */
unsigned hidq_dropped(void); /* reports dropped because a queue was full, since start */

/* A release report: everything after the report id is zero. Holds for the reports of the
 * Hub's HID descriptor (keyboard, consumer, mouse: no key and no button = all zero); a report
 * added later whose idle state is not all zero needs its own rule here. */
int hidq_is_release(const uint8_t *report, uint16_t len);

#endif
