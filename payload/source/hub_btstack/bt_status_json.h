#ifndef HARMONY_BT_STATUS_JSON_H
#define HARMONY_BT_STATUS_JSON_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/*
 * hub_btstack's status file (/tmp/hub_btstack_status.json), written by update_status() in
 * bt_pairing.c. The writer has no BTstack dependency so the tests can produce the exact file
 * from dummy values (tests/bt_status_sample.c). Shape:
 *   backend, pid, state (the LE remote link's state), target (the LE remote's address),
 *   last_button, last_seen, remote {connected, addr}, host {connected, addr, name, pairing}
 *   (the host keys go to), hosts [{addr, name, connected, paired, cid}] (every known host).
 * hub_daemon adds ok, running, connected and pairing when it serves it (state_watch.c).
 */

#define BT_STATUS_MAX_HOSTS 4
#define BT_STATUS_ADDR_LEN 18 /* "AA:BB:CC:DD:EE:FF" + NUL */

typedef struct {
    char addr[BT_STATUS_ADDR_LEN];
    const char *name;
    bool connected;
    bool paired;
    uint16_t cid;
} bt_status_host_t;

typedef struct {
    int pid;
    const char *state;
    char target[BT_STATUS_ADDR_LEN];
    const char *last_button;
    long last_seen;
    bool remote_connected;
    char remote_addr[BT_STATUS_ADDR_LEN];
    bool host_connected;
    char host_addr[BT_STATUS_ADDR_LEN];
    const char *host_name;
    bool host_pairing;
    int nhosts;
    bt_status_host_t hosts[BT_STATUS_MAX_HOSTS];
} bt_status_view_t;

/* Writes the status object (strings JSON-escaped); same layout as before the split */
void bt_status_write(FILE *f, const bt_status_view_t *v);

#endif
