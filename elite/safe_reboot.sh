#!/bin/sh
# Safe reboot script for Harmony Elite (Texas Instruments DaVinci ARM9)

# 1. Terminate watchdog and active processes to avoid restart loop
killall -9 elite_watchdog.sh codex_watchdog.sh elite_ui codex_elite hal luaworks luadraws lua luajit 2>/dev/null || true

# 2. Wait for pending I2C / SPI transactions to finish
sleep 1

# 3. Flush write buffers to NAND flash via elite_sync
/data/elite_sync 2>/dev/null || /data/sync 2>/dev/null || sync 2>/dev/null || true
sleep 2

# 4. Remount flash partition read-only to commit clean superblocks
mount -o remount,ro /data 2>/dev/null || true

# 5. Final sync and reset
/data/elite_sync 2>/dev/null || true
sleep 1
/sbin/reboot -f 2>/dev/null || reboot -f 2>/dev/null || (sleep 1 && echo b > /proc/sysrq-trigger)
