#ifndef BT_RECONNECT_QUEUE_H
#define BT_RECONNECT_QUEUE_H

/*
 * Hosts waiting for their turn to reconnect, and the list of connected hosts kept in /tmp.
 *
 * hub_btstack runs one reconnect loop at a time. A loop for another host used to replace the
 * running one, and the replaced host was never tried again (after a restart the activity's host
 * replaced the other paired host). Replaced, deferred and restored hosts wait here instead; the
 * next one starts when a loop ends. No BTstack dependency, so it is unit tested on the host.
 */

#include <stdbool.h>
#include <stdint.h>

#define RQ_MAX 4

void rq_clear(void);
/* Adds a host at the end; false for a null address, a host already queued, or a full queue */
bool rq_push(const uint8_t addr[6]);
/* Removes a host if queued; true when it was */
bool rq_remove(const uint8_t addr[6]);
int rq_count(void);
/* Takes the next host for which skip() is false (hosts skipped on the way are dropped);
 * true and the address in out, or false when none is left */
bool rq_pop(uint8_t out[6], bool (*skip)(const uint8_t addr[6]));

/* The connected-hosts file: one address per line (XX:XX:XX:XX:XX:XX). It is in /tmp, so it
 * survives a hub_btstack restart (the hosts connected before it are reconnected) but not a
 * reboot. Write returns 0 or -1; read returns the number of addresses (0 when no file). */
int bt_hosts_file_write(const char *path, const uint8_t (*addrs)[6], int n);
int bt_hosts_file_read(const char *path, uint8_t (*out)[6], int max);

#endif
