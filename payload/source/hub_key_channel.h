#ifndef HUB_KEY_CHANNEL_H
#define HUB_KEY_CHANNEL_H

/*
 * Key events from hub_rf to hub_daemon: one UNIX datagram per event (press, hold, release) on
 * HUB_KEY_SOCK, read in order by one thread of hub_daemon. A datagram is a JSON object:
 *
 *   {"seq": 12, "ts": 5839211, "slot": 1, "button": "VolumeUp", "action": "press",
 *    "deviceId": "3", "command": "VolumeUp"}
 *
 * seq counts the events hub_rf sent since it started; ts is CLOCK_MONOTONIC in ms when hub_rf
 * received the key from the remote (the same clock in both processes). deviceId and command
 * are present when hub_rf mapped the key to a device command.
 *
 * hub_daemon sends the device command and gives the button event, as POST /api/ir-send and
 * POST /api/button-press do. A press or hold older than HUB_KEY_STALE_MS is skipped: by then
 * the remote has sent the next repeat or the release, and running it late is what makes a
 * key run on after the user let go. A release always runs.
 */
#define HUB_KEY_SOCK      "/tmp/hub_daemon_keys.sock"
#define HUB_KEY_STALE_MS  300
#define HUB_KEY_MAX_DGRAM 512

#endif /* HUB_KEY_CHANNEL_H */
