/* hub_btstack's status file from the real writer (payload/source/hub_btstack/bt_status_json.c)
 * with dummy values: an LE remote 11:22:33:44:55:66, a connected host "Media Player"
 * AA:BB:CC:DD:EE:FF (the one keys go to, cid 6) and a second paired host "Television"
 * AA:BB:CC:DD:EE:01, not connected (cid 2 pending, as hub_btstack leaves it).
 * Options change one thing each, for the tests:
 *   --pid N            (default: this process)
 *   --state S          (default "connecting")
 *   --host-connected 0 the host keys go to drops (host.connected and its hosts[] entry)
 *   --pairing 1        pairing mode on
 *   --no-hosts         nothing paired: hosts [], host empty
 *   --host-name NAME   name of the connected host (escaping test)
 *   --last-button S, --last-seen N   the volatile fields
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "bt_status_json.h"

int main(int argc, char **argv) {
    bt_status_view_t v;
    memset(&v, 0, sizeof(v));
    v.pid = (int)getpid();
    v.state = "connecting";
    v.last_button = "";
    v.last_seen = 0;
    snprintf(v.target, sizeof(v.target), "11:22:33:44:55:66");
    snprintf(v.remote_addr, sizeof(v.remote_addr), "11:22:33:44:55:66");
    v.remote_connected = true;
    const char *host_name = "Media Player";
    int host_connected = 1, pairing = 0, no_hosts = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--pid") && i + 1 < argc) v.pid = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--state") && i + 1 < argc) v.state = argv[++i];
        else if (!strcmp(argv[i], "--host-connected") && i + 1 < argc) host_connected = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pairing") && i + 1 < argc) pairing = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--host-name") && i + 1 < argc) host_name = argv[++i];
        else if (!strcmp(argv[i], "--last-button") && i + 1 < argc) v.last_button = argv[++i];
        else if (!strcmp(argv[i], "--last-seen") && i + 1 < argc) v.last_seen = atol(argv[++i]);
        else if (!strcmp(argv[i], "--no-hosts")) no_hosts = 1;
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    v.host_pairing = pairing;
    if (no_hosts) {
        v.host_name = "";
    } else {
        v.host_connected = host_connected;
        snprintf(v.host_addr, sizeof(v.host_addr), "AA:BB:CC:DD:EE:FF");
        v.host_name = host_name;
        v.nhosts = 2;
        snprintf(v.hosts[0].addr, sizeof(v.hosts[0].addr), "AA:BB:CC:DD:EE:01");
        v.hosts[0].name = "Television";
        v.hosts[0].connected = false;
        v.hosts[0].paired = true;
        v.hosts[0].cid = 2;
        snprintf(v.hosts[1].addr, sizeof(v.hosts[1].addr), "AA:BB:CC:DD:EE:FF");
        v.hosts[1].name = host_name;
        v.hosts[1].connected = host_connected;
        v.hosts[1].paired = true;
        v.hosts[1].cid = host_connected ? 6 : 0;
    }
    bt_status_write(stdout, &v);
    return 0;
}
