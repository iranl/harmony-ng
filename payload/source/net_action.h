#ifndef NET_ACTION_H
#define NET_ACTION_H

#include "cJSON.h"

/* Commands of HTTP and TCP devices (net_device.h). They run one at a time on a worker thread,
 * so a slow or dead device never holds the key path (hw_action.c's send lock) or the HTTP
 * server. Plain HTTP and TCP run in process; a TLS command runs hub_fetch (hub_daemon carries
 * no BearSSL: libbearssl.so.0 is on the Hub once, shared by hub_fetch and the plugins). */

/* Queues the command of dev and returns at once: 0 queued (or logged in dry run), -2 when the
 * device or command cannot be sent (no host or port), -3 when the queue is full. live_key: a key
 * pressed on a remote now; only such a send is dropped when it is late or its device just timed
 * out (an activity step, an API or MQTT send always goes out, in order), and a live key that
 * finds the queue full takes the place of the oldest live key over 1 s old. */
int net_action_send(const cJSON *dev, const cJSON *cmd, const char *dev_id, const char *dev_name,
                    const char *command, int live_key);

/* POST /api/device-net-test: runs cmd (NULL: connect and TLS handshake only) and waits.
 * Returns {ok, status?, error?, response?, tlsFingerprint?, pinned?}. */
cJSON *net_action_test(const cJSON *dev, const cJSON *cmd, const char *dev_id, const char *dev_name);

/* Removes the pinned key of the device's host:port. 0 removed, 1 none stored, -1 error. */
int net_action_forget_pin(const cJSON *dev);

#endif
