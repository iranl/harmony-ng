#!/bin/sh
# Safe reboot script for the Harmony remote (Texas Instruments DaVinci ARM9)
PATH=/data:/bin:/sbin:/usr/bin:/usr/sbin
export PATH

# 1. Signal supervisor to stop and terminate all processes to avoid restart loop
touch /tmp/remote_stop /tmp/elite_stop 2>/dev/null || true
rm -f /data/reboot_counter 2>/dev/null || true
killall -9 init.sh remote_watchdog.sh elite_watchdog.sh codex_watchdog.sh remote_ui elite_ui codex_elite hal luaworks luadraws lua luajit 2>/dev/null || true

# 2. Wait for pending I2C / SPI transactions to finish and driver buffers to drain
sleep 1

# 3. Flush write buffers to NAND flash via remote_sync
/data/remote_sync 2>/dev/null || /data/sync 2>/dev/null || sync 2>/dev/null || true
sleep 1

# 4. Remount cache partition read-only to commit clean superblock
mount -o remount,ro /cache 2>/dev/null || true

# 5. Final sync and reset
/data/remote_sync 2>/dev/null || sync 2>/dev/null || true
sleep 1
/sbin/reboot -f 2>/dev/null || reboot -f 2>/dev/null || (sleep 1 && echo b > /proc/sysrq-trigger)
