# Logitech Harmony Elite Remote Hacking & Support

## Serial Console & U-Boot (UART)
Baud rate: `115200 8N1` on serial pad.

To prevent kernel console suspend when remote idles:
```
printenv bootargs
setenv bootargs ${bootargs} no_console_suspend
saveenv
boot
```

### Serial Bootstrap (`elite/bootstrap_serial.py`)
To push Dropbear, generate SSH keys, enable USB Ethernet, and install the recovery boot script directly over serial:
```sh
pip install pyserial
python elite/bootstrap_serial.py --port COM3 --ssh-pubkey ~/.ssh/harmony_owner_ed25519.pub
```
This utility:
1. Starts `/usr/bin/usbeth start` on the remote.
2. Streams base64-encoded static binaries (`dropbear`, `dropbearkey`, `scp`, `codex_sync`) to `/data/`.
3. Generates Dropbear host keys in `/etc/dropbear/`.
4. Installs your SSH public key into `/home/root/.ssh/authorized_keys`.
5. Installs the emergency-recovery boot script `/etc/init.d/rcS.local`.
6. Starts Dropbear daemon (`/data/dropbear -R -B`).

---

## USB Ethernet & Dropbear SSH Setup
Architecture: ARMv5TE little-endian (`armv5tejl`, uClibc-0.9.30.1).

### Host Connection (PC)
- **USB Ethernet Interface**: `192.168.2.2 / 24`
- **Remote IP**: `192.168.2.1`
- **SSH Access**:
  ```sh
  ssh -i ~/.ssh/harmony_owner_ed25519 root@192.168.2.1
  ```
- **SCP File Transfer** (requires legacy `-O` flag):
  ```sh
  scp -O file.bin elite:/data/
  ```

---

## SSH Deployment Script (`elite/deploy_elite.py` / `deploy_elite.ps1`)
Once SSH is operational, deploy updated custom firmware (`codex_elite`), tools, and boot scripts:
```sh
# Linux/macOS
python3 elite/deploy_elite.py --ip 192.168.2.1 --key ~/.ssh/harmony_owner_ed25519

# Windows PowerShell
.\elite\deploy_elite.ps1 -Ip 192.168.2.1 -KeyPath ~/.ssh/harmony_owner_ed25519
```
This script uploads:
- `codex_elite` (custom framebuffer UI & CC2544 RF engine)
- `codex_sync` (ARM sync helper, symlinked to `/data/sync` and `/usr/bin/sync`)
- `codex_rf_query` & `codex_rf_sniff`
- `/etc/init.d/rcS.local` with panic recovery counter

---

## Emergency Recovery (Panic Loop Protection)
Elite `/etc/init.d/rcS.local` maintains a persistent reboot counter at `/data/reboot_counter`:
- On boot, counter increments and writes to `/data/reboot_counter`.
- If uptime reaches **5 minutes (300 seconds)** without crashing, counter resets to `0`.
- **Safe Mode**: If the device reboots more than **5 consecutive times** before reaching 5-minute stability (`COUNT > 5`), it enters Emergency Recovery Mode:
  - Enables **only** USB Ethernet (`/usr/bin/usbeth start`) and Dropbear SSH (`/data/dropbear -R -B`).
  - Logs alert to `/tmp/emergency_recovery.log`.
  - Exits boot script **without** launching `codex_elite` or any other software.
  - Allows full SSH root access at `192.168.2.1` to inspect logs, repair files, or reset the counter (`echo 0 > /data/reboot_counter`).

---

## Note on Busybox `sync`
BusyBox v1.13.4 on both the Hub and Elite remote lacks the `sync` applet. Never run raw `sync` in scripts. Always use:
```sh
sync 2>/dev/null || /data/codex_sync 2>/dev/null || /data/sync 2>/dev/null || true
```