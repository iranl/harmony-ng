/* Shared start check of the RF debug tools: one process at a time on /dev/rfspi.
 *
 * A second process on /dev/rfspi corrupts the SPI arbiter that the CC2544 shares with the NOR
 * flash and can hang the Hub until a power cycle (docs/HARMONY_RF_PROTOCOL.md). hub_rf holds an
 * exclusive flock on /var/run/hub_rf.lock while it runs; the tools take the same lock before they
 * touch the radio and refuse to run while it is held. Holding it also keeps hub_rf from starting
 * while a tool runs. Stop hub_rf first: /data/harmony/init.sh stop.
 *
 * On the remote, remote_ui owns /dev/rfspi and takes no such lock: stop it there first
 * (touch /tmp/remote_stop, then killall -9 remote_ui). */
#ifndef RF_DEBUG_LOCK_H
#define RF_DEBUG_LOCK_H

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#ifndef HUB_RF_LOCK
#define HUB_RF_LOCK "/var/run/hub_rf.lock"
#endif

/* 0 when the tool may use the radio (the lock fd stays open until exit), -1 when hub_rf runs */
static int rf_debug_take_lock(void) {
    int fd = open(HUB_RF_LOCK, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        fprintf(stderr, "[!] Cannot open %s (%s): make sure hub_rf is stopped\n", HUB_RF_LOCK, strerror(errno));
        return 0;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            fprintf(stderr, "[-] hub_rf is running (it holds %s); stop it first: /data/harmony/init.sh stop\n", HUB_RF_LOCK);
            close(fd);
            return -1;
        }
        fprintf(stderr, "[!] flock %s: %s\n", HUB_RF_LOCK, strerror(errno));
    }
    return 0;
}

#endif
