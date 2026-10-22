#!/bin/sh
# Safe reboot script for Harmony Hub (Atheros AR9344)
# Prevents warm reset SPI arbiter / NOR flash bus lockups.

# 1. Terminate RF and background workers immediately to release /dev/rfspi and Bluetooth UART
killall -9 codex_rf codex_btstack codex_daemon codex_dhcpd codex_portal hal luaworks luadraws lua 2>/dev/null || true

# 2. Wait for in-flight SPI hardware transactions to complete
sleep 1

# 3. Flush write buffers to NOR flash (AR9344 has no bare 'sync', use codex_sync)
/data/codex/bin/codex_sync 2>/dev/null || /cache/bin/sync 2>/dev/null || /data/codex/bin/sync 2>/dev/null || true
sleep 2

# 4. Remount partitions read-only to commit clean JFFS2 superblocks
mount -o remount,ro /data 2>/dev/null || true
mount -o remount,ro /cache 2>/dev/null || true

# 5. Final sync and reset
/data/codex/bin/codex_sync 2>/dev/null || true
sleep 1
reboot -f
