# Harmony Hub: Complete Offline Unbrick, Recovery & Cloud-Free Root Guide

This guide documents the complete hardware and software procedure for recovering a bricked, boot-looping, or factory-reset Logitech Harmony Hub and restoring persistent root SSH access **without requiring the Logitech Cloud, the MyHarmony desktop app, or the Harmony mobile app**.

---

> [!IMPORTANT]
> ### ⚠️ Factory Reset is ALWAYS Preferred Above Serial Console Recovery
> **Do NOT open the Hub casing or solder UART wires unless a hardware factory reset fails!**
> 
> - **Zero Risk & No Disassembly**: The Hub's read-only factory filesystem (`mtd3`, SquashFS) is physically immutable. Custom software, boot scripts (`rcS.local`), and configuration files only live on the writable UnionFS overlay (`mtd4` /mnt/data and `mtd5` /cache).
> - **Instant Recovery**: Performing a **Hardware Factory Reset** (holding the Pair/Reset button while plugging USB into your PC) completely wipes corrupted writable partitions back to pristine factory state in 30 seconds.
> - **Automated Re-Root & Full Configuration Restore**: After factory resetting, you can immediately restore your Hub with full configuration intact using `python tools/restore_stock.py`, or re-root offline over USB HID in 60 seconds using `python tools/root_hub_usb.py`.
> - **Serial Console UART Recovery** (Sections 2–8 below) is an emergency last-resort procedure intended solely for rare bootloader (U-Boot) corruption or hardware diagnostic debugging.

---

## 0. Overview & Architecture

### The Hardware
- **SoC**: Atheros AR9344 (MIPS 74Kc big-endian, ~533 MHz).
- **RAM**: 64 MB DDR.
- **Flash**: 16 MB SPI Flash.
  - `mtd0`: U-Boot bootloader (`ar7240`).
  - `mtd1`: Kernel image 1 (`0x9f010000`).
  - `mtd2`: Kernel image 2 (`0x9f100000`).
  - `mtd3`: Root filesystem (SquashFS, read-only factory image).
  - `mtd4`: User data partition (`/mnt/data`, JFFS2, read-write overlay).
  - `mtd5`: Cache partition (`/cache`, JFFS2).

### The UnionFS Layer
The Hub boots with a UnionFS overlay where `/` is composed of:
```text
none on / type unionfs (rw,dirs=/mnt/data=rw:/mnt/root=ro)
```
- Any file modified in `/etc`, `/usr`, or `/data` is stored on `/mnt/data`.
- If an init script (such as `/etc/init.d/rcS` or `/etc/init.d/rcS.local`) is corrupted or causes a panic loop, the original untouched factory copy still exists in the read-only SquashFS `/mnt/root/etc/init.d/rcS`.
- The factory reset function simply formats and wipes `/mnt/data` and `/cache`, instantly reverting all modifications to stock.

### Why the Stock Firmware Blocks Root Without Cloud
When a hub is factory reset, `/data/resources/Context.json` is erased. In this state, Logitech's stock daemon (`luaworks`) on port 8088 rejects all local WebSocket connections with `401 Wrong hubId` because no account or device profile has been loaded yet. 
**Our USB HID root tools bypass the entire cloud provisioning step** and directly install persistent SSH into the system overlay over USB without needing serial console access.

---

## 1. Hardware Factory Reset via Pair Button (Preferred Method)

To revert any changes, fix a boot-loop, or recover an unresponsive Hub without opening the case:

### Step 1.1: Initiate Factory Reset
1. **Unplug the micro-USB cable** from the rear of the Harmony Hub (Hub powered OFF).
2. Locate the circular **Pair/Reset** button on the back panel (next to the micro-USB port and 3.5mm IR blaster jacks).
3. **Press and hold down the Pair/Reset button**.
4. While keeping the button pressed down firmly, **plug the micro-USB cable into a USB port on your PC** (or USB power adapter).
5. **Continue holding the Pair/Reset button for 15 to 20 seconds**.
6. Observe the front status LED:
   - The LED will initially turn off or flash briefly.
   - Once the front LED begins **flashing red rapidly**, release the Pair/Reset button.
7. The Hub will execute U-Boot factory recovery:
   - Formats and clears the JFFS2 writable overlay partitions (`/data` and `/cache`).
   - Removes all custom daemons (`hub_daemon`, `hub_rf`), startup scripts, and Dropbear keys.
   - Preserves the untouched, read-only factory SquashFS filesystem.
   - Automatically reboots into clean Logitech factory firmware (~30–45 seconds).
8. Factory reset is complete when the front LED transitions to a **slow, steady red pulse** (indicating unconfigured factory state, waiting for setup).

### Step 1.2: Post-Reset Recovery Options

After the factory reset completes, choose one of two paths:

#### Option A: Return to Stock with Full Configuration Restored (All-In-One Script)
If you previously backed up your Hub or gained root using `tools/root_hub_usb.py` (which creates an automatic stock backup of `/cache` and `/data` in plain `.tar` format), restore your original configuration, Wi-Fi credentials, activities, and devices in a single command:

```bash
# All-in-one restore (gains root over USB, stops daemons, restores /cache and /data, syncs, reboots)
python tools/restore_stock.py

# Or specify a custom backup file:
python tools/restore_stock.py --backup backups/stock_hub_backup_latest.tar

# Optional: To return to 100% pure factory stock with Dropbear SSH removed:
python tools/restore_stock.py --pure-stock
```

The script automatically:
1. Connects to the Hub over USB HID (`046D:C129`).
2. Configures Wi-Fi credentials over USB if needed.
3. Stages the Dropbear payload to `/sys/dropbear` and boots into root SSH.
4. Kills all running processes except Dropbear to release flash file locks.
5. Transfers and extracts your backed-up `/cache` and `/data` partitions (plain uncompressed tar, no gzip on Hub).
6. Safely flushes eraseblock buffers (`sync; sync; sleep 2; verify /bin/sh -c 'true'`).
7. Reboots the Hub into fully configured stock Logitech firmware.

#### Option B: Re-Root and Deploy HarmonyNG
If you wish to reinstall HarmonyNG after a clean reset:

```bash
# 1. Provision Wi-Fi and gain root over USB (with automatic stock backup):
python tools/root_hub_usb.py --wifi-ssid "YourNetwork" --wifi-pass "YourPassword"

# 2. Deploy HarmonyNG firmware:
# Windows (PowerShell):
.\build\deploy_hub.ps1 -HubIp <HUB_IP>
# Linux / macOS (Bash):
./build/deploy_hub.sh <HUB_IP>
```

---

## 2. Hardware Preparation & UART Pinout

### Disassembly
1. Peel back or remove the 2 rubber foot pads on the bottom of the Hub to expose 4 Phillips screws.
2. Remove all 4 screws.
3. The top and bottom plastic shells are held together by perimeter snap clips. Insert a plastic pry tool, guitar pick, or thin spudger into the seam along the rear (near the micro-USB and 3.5mm IR jacks).
4. Pry gently around the edges; the top cover will pop off cleanly.
5. Unscrew the 1 screw holding the PCB in place.
6. Lift the PCB out of the plastic casing.
7. Flip the board over.
8. Look for the J2 header

### UART Pinout
The J2 header is a 10-pin test point header:

```
  [ 2 ] [ 4 ] [ 6 ] [ 8 ] [ 10 ]
  [ 1 ] [ 3 ] [ 5 ] [ 7 ] [ 9 ]
```

| Pin | Label | Description | Connection to USB-UART Adapter |
|---|---|---|---|
| **2** | **RX** | Hub UART Receive | Connect to adapter **TX** |
| **4** | **TX** | Hub UART Transmit | Connect to adapter **RX** |
| **8** | **GND** | Ground | Connect to adapter **GND** |

> [!CAUTION]
> The AR9331 uses **3.3V TTL logic**. Ensure your USB-UART adapter is set to **3.3V** (never 5V). Do not connect VCC; power the Hub normally through its micro-USB port.

### Serial Terminal Settings
- **Baud Rate**: `115200`
- **Data Bits**: `8`
- **Parity**: `None`
- **Stop Bits**: `1`
- **Flow Control**: `None`

---

## 3. Entering Single-User Mode (U-Boot Interruption)

When powered on, U-Boot listens on serial for ~1 second before booting the kernel.

1. Open your serial terminal (PuTTY, Tera Term, or `picocom -b 115200 /dev/ttyUSB0`).
2. Start spamming the **Enter** key.
3. Plug in the micro-USB power cable.
4. U-Boot will halt autoboot and drop to the command prompt:
   ```text
   ar7240> 
   ```

5. In U-Boot, boot into a single-user root shell with active serial console:
   ```text
   setenv bootargs console=ttyS0,115200 init=/bin/sh
   bootm 0x9f010000
   ```
   *(Note: This does not modify the flash environment; it only applies to the current boot).*

6. Linux will boot directly to a root prompt:
   ```text
   /# 
   ```

---

## 4. Initializing Filesystems in Single-User Mode

Because `/bin/sh` bypassed `/sbin/init`, standard filesystems must be mounted manually:

```sh
# Mount volatile ramfs and link /tmp
mkdir -p /var/volatile/tmp /var/volatile/run /var/volatile/log
ln -sf /var/volatile/tmp /tmp

# Mount all storage partitions and remount unionfs as read-write
mount -a
mount -o remount,rw /

# Verify write access
mkdir -p /usr/sbin
touch /usr/sbin/test && rm -f /usr/sbin/test
```

---

## 5. Bringing Up Wi-Fi in Single-User Mode

The Wi-Fi kernel modules must be inserted in their exact dependency order:

```sh
D=/lib/modules/$(uname -r)
insmod $D/asf.ko
insmod $D/adf.ko
insmod $D/ath_hal.ko
insmod $D/ath_rate_atheros.ko
insmod $D/ath_dev.ko
insmod $D/umac.ko

# Create the station interface and connect using existing credentials
wlanconfig ath0 create wlandev wifi0 wlanmode sta
wpa_supplicant -B -iath0 -c /etc/wpa_supplicant.conf
udhcpc -i ath0
```

Wait 3–5 seconds. You will see:
```text
Sending select for <Hub_IP>...
Lease of <Hub_IP> obtained, lease time ...
```

---

## 6. Transferring Dropbear SSH Without Cloud

The factory SquashFS on Harmony Hub does not include `dropbear`. We use a tiny 4 KB TCP receiver helper that can be pasted over serial, which then pulls the full 577 KB `dropbearmulti` binary over Wi-Fi in 1 second.

All required tools are stored in [`tools/unbrick_recovery/`](tools/unbrick_recovery):

### Step A: Start the TCP Server on your PC
On your computer (connected to the same Wi-Fi/LAN), run:
```bash
python tools/unbrick_recovery/serve_dropbear.py
```
This prints your PC's IP and begins listening on port `9999`.

### Step B: Create `/tmp/recv` on the Hub
In your serial terminal on the Hub, paste the contents of [`tools/unbrick_recovery/upload_recv.sh`](tools/unbrick_recovery/upload_recv.sh):
*(This script contains 71 lines of `echo -ne "\x..." >> /tmp/recv` and finishes with `chmod +x /tmp/recv`).*

Verify on the Hub:
```sh
ls -l /tmp/recv
# Output should show: 4332 bytes
```

### Step C: Pull `dropbearmulti` over Wi-Fi
On the Hub, run:
```sh
/tmp/recv <YOUR_PC_IP> 9999 > /usr/sbin/dropbearmulti
```
Verify the downloaded binary:
```sh
ls -l /usr/sbin/dropbearmulti
# Output should show: 577296 bytes
```

---

## 7. Installing Dropbear Permanently

Now configure the system so Dropbear starts on every normal boot:

```sh
# 1. Make executable and create symlinks
chmod 755 /usr/sbin/dropbearmulti
ln -sf /usr/sbin/dropbearmulti /usr/sbin/dropbear
ln -sf /usr/sbin/dropbearmulti /usr/sbin/dropbearkey

# 2. Generate RSA host key
mkdir -p /etc/dropbear /root/.ssh
/usr/sbin/dropbearkey -t rsa -f /etc/dropbear/dropbear_rsa_host_key

# 3. Create the boot trigger file
# Stock /etc/init.d/rcS checks: if [ -r /etc/tdeenable -a -x /usr/sbin/dropbear ]; then dropbear; fi
touch /etc/tdeenable

# 4. Install your SSH public key
# Dropbearmulti supports both ED25519 and RSA keys:
echo "<YOUR_SSH_PUBLIC_KEY>" > /etc/dropbear/authorized_keys
cp /etc/dropbear/authorized_keys /root/.ssh/authorized_keys

# 5. (Optional) Set blank root password for direct console access
sed -i 's/^root:[^:]*:/root::/' /etc/passwd

```

---

## 8. Rebooting into Production Multi-User Mode

Reboot the Hub cleanly:
```sh
reboot -f
```

The Hub will now boot normally into `/sbin/init`:

Verify from your PC:
```bash
ssh -i ~/.ssh/harmony_owner_ed25519 root@<Hub_IP> "uname -a; id"
```

You now have permanent root SSH access again when recovering from a bricked state.