# Logitech Harmony Elite Remote Hacking & Support

> [!NOTE]
> **Hardware Family Compatibility**:
> The **Harmony Pro 2400 Remote** (Skin 116), Harmony Ultimate (Home), and Harmony Touch share the same underlying architecture (TI DaVinci ARM, CC2544 RF, ClearPad touch, 240x320 RGB565 LCD). Pro 2400 remote runs Dropbear SSH out of the box in stock firmware 10.0.215. All are supported using the same tooling and `codex_elite` binary (*untested on physical Pro 2400 remote hardware*).

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
python tools/bootstrap_serial.py --port COM3 --ssh-pubkey ~/.ssh/harmony_owner_ed25519.pub
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

---

## Sleep Behavior & Expected Run Times

The Harmony Elite remote features a **1,300 mAh (3.7V Li-ion, ~4.81 Wh)** battery. In custom firmware (`codex_elite`), sleep is managed via a two-tier hardware power state architecture: **Light Standby** and **Deep Standby**.

### Standby Power States & Wake Triggers

| Standby Tier | Timeout Range | Hardware State | Wake Triggers |
| :--- | :--- | :--- | :--- |
| **Active** | While interacting | Screen ON (PWM 1-5), CPU @ 456 MHz, Clearpad touch active, gyro polling @ 10-50 Hz | N/A |
| **Light Standby** | **0 – 30 minutes** after screen blank | Screen OFF (`FB_BLANK_POWERDOWN`), Backlight 0, Clearpad Touch ASLEEP, CPU in WFI, LIS3DH INT1 IRQ active (0 I2C reads) | • **Pickup / Motion** (LIS3DH INT1 hardware IRQ)<br>• **Any Physical Button** |
| **Deep Standby** | **> 30 minutes** after screen blank | Gyro I2C polling **completely suspended (0 Hz)**, `select()` timeout scaled to 30s, CPU in deep WFI | • **Any Physical Button ONLY** (Power, Activities, Devices, OK, Vol, D-pad, etc.) |

> [!NOTE]
> Touchscreen taps do **not** wake the remote in either standby mode. The Synaptics Clearpad sensor is put into hardware sleep mode (`0x35 = 0x01`) upon display blanking to prevent accidental touches and save ~10.5 mA.

### Power Optimizations & Measured Run Times

Live hardware telemetry was captured via battery ADC moving-average tracking (`/sys/devices/platform/i2c_davinci.1/i2c-1/1-0048/tps6507x-juniper/vbat`):

| Firmware State | Hourly Battery Drain | Standby Current | Projected Standby Run Time | Measured Improvement |
| :--- | :---: | :---: | :---: | :---: |
| **Baseline (Unpatched)** | **7.8% / hour** | ~101 mA | **~12.8 hours** | Baseline (`FB_BLANK_NORMAL` failed with `EINVAL`, LCD DMA active) |
| **Stage 1 (LCD Controller Shutdown)** | **2.5% / hour** | ~32.5 mA | **~40.0 hours (1.7 days)** | **3.1x longer** (Verified: 13% drop across 5h 08m uninterrupted sleep) |
| **Stage 2 (Clearpad Sleep + Deep Standby)** | **~1.31% / hour** | **~17.0 mA** | **~76.5 hours (~3.2 days)** | **~6.0x longer** (>30m gyro suspended, 30s select loop, touch matrix off) |

### Key Power Mechanisms in `codex_elite`
1. **`FB_BLANK_POWERDOWN` (4)**: The TI DaVinci DM365 framebuffer driver rejects standard `FB_BLANK_NORMAL` (1) with `EINVAL`. Calling `ioctl(fb_fd, FBIOBLANK, 4)` completely powers down the DaVinci LCD raster controller and halts DDR memory DMA transfers.
2. **Clearpad Touch Controller Sleep**: Upon screen blank, `clearpad_sleep()` writes `0x01` (Sleep Mode) to F01 Control register `0x35` at I2C address `0x20` on `/dev/i2c-1`, reducing touchscreen draw to < 0.5 mA.
3. **Hardware Interrupt-Driven Motion Wake (LIS3DH INT1)**:
   - Configures LIS3DH hardware motion threshold (`int1_threshold=10`, `int1_duration=3`).
   - Motion assertions trigger Linux GPIO IRQ 108 directly on `/dev/input/event1`.
   - Main thread `select()` multiplexes `/dev/input/event1` with physical buttons during Light Standby (0-30m).
   - Eliminates 100% of I2C bus traffic while stationary (0 I2C reads/hour, saving ~4.0 mA).
   - After 30 minutes (Deep Standby), `/dev/input/event1` is detached to prevent accidental motion wakes.
4. **Event Loop & Watcher Throttling**: The main `select()` event loop timeout scales from 100 ms (active) to 10 s (0-30m standby) to 30 s (>30m Deep Standby). Background `power_manager` sleeps for 10 seconds in standby, and battery ADC sampling throttles to every 300s (5 min).

---

## Physical Button Navigation & Nav Toggle Button

The Harmony Elite remote can be operated **100% via physical buttons**, allowing full control without touching the screen (e.g. if the touchscreen is disabled, broken, or when navigating blindly).

### Capacitive Fixed Buttons (Directly Below LCD)
- **Left Button (`KEY_FAVORITES`, code 364)**: Jumps immediately to the **ACTIVITIES** view.
- **Right Button (`KEY_HOME`, code 102)**: Jumps immediately to the **DEVICES** view.
*These buttons never transmit over RF to the Hub; they exclusively control UI navigation.*

### Bottom Navigation Row (Dimmer & Smart Home Buttons)
The lower physical keypad cluster controls on-screen cards when in **NAV Mode** or within the **SETTINGS** / **PAIRING** views:

| Button Name | Linux Input Code | Function in NAV Mode / Settings |
| :--- | :---: | :--- |
| **Rocker Up (Dimmer +)** | `78` (`KEY_KPPLUS`) | Cursor Up / Previous card or item |
| **Rocker Down (Dimmer -)** | `74` (`KEY_KPMINUS`) | Cursor Down / Next card or item |
| **Light On** | `148` (`KEY_PROG1`) | Select / OK / Execute / Confirm switch |
| **Light Off** | `149` (`KEY_PROG2`) | Tab Left / Previous Category |
| **Plug On** | `202` (`KEY_PROG3`) | Back / Cancel / Dismiss confirmation |
| **Plug Off** | `203` (`KEY_PROG4`) | Tab Right / Next Category |
| **Back Key** | `158` (`KEY_BACK`) | Back / Cancel / Dismiss confirmation |

### Operating Modes: `DEV` vs `NAV`
The remote maintains two distinct operating states for the physical navigation keys:
- **`DEV` Mode (Device Control)**: The bottom home automation buttons and D-Pad transmit IR/RF commands to external AV devices via the Hub.
- **`NAV` Mode (Screen Navigation)**: The bottom buttons control on-screen card selection and menu browsing.

### Mode Toggle Button (`g_nav_toggle_key`)
To toggle between `DEV` and `NAV` modes without touching the display, a physical button can be designated as the mode switch in the **TOUCH** settings menu:
- **Assigned Key**: Selectable among `MENU` (code 139), `INFO` (code 358), `LIST` (code 395), `GUIDE`/`LIVE TV` (code 389), or `EXIT` (code 174). Default: `MENU`.
- **Toggle Mode (`g_nav_toggle_mode`)**:
  - `0` (**SHORT**): Pressing the button toggles between `DEV` and `NAV` immediately.
  - `1` (**LONG**): Holding the button for $\ge 500\text{ ms}$ toggles between `DEV` and `NAV`. A quick tap (< 500 ms) sends the button's standard AV command over RF to the Hub.
- **On-Screen Mode Pill**: Tapping the mode pill in the bottom right corner of the touchscreen (labels `NAV` or `DEV`) also toggles mode.
- **Auto-Lock**: If the touchscreen is disabled in Settings (`touch=0`), the remote automatically locks into `NAV` mode for the bottom buttons.

---

## Device Browser

Every device configured on the Hub can be controlled directly, without starting an activity:

- The **ACTIVITIES** list ends with a **Devices** entry (after Power Off) once the Hub has pushed its devices.
- **Devices** opens the list of all Hub devices; choosing a device lists all of its commands, 6 per page.
- Choosing a command sends it to the Hub, which transmits it to the device.
- **Back**: swipe left-to-right, tap the `<` title, or press `Back` / `Plug On` / `Light Off` (in `NAV` mode). The Activities capacitive button returns to the activity list.

Command names are sent by the Hub during each config push. Remotes running older firmware ignore them and keep working without the device browser.

---

## Complete Remote Settings Menu Reference

The Elite settings menu is structured into 5 categories plus an RF sync push action:

```
[SETTINGS]
  ├── 1. ACTIVITIES  --> Confirm switch, remote mode
  ├── 2. DISPLAY     --> Brightness, timeout, backlight, night schedule
  ├── 3. TOUCH       --> Touchscreen on/off, sensitivity, nav toggle key & mode
  ├── 4. POWER       --> Gyro motion wake, sampling rate, battery health
  ├── 5. SYSTEM      --> USB ethernet, power debug log, pairing, reboot, factory reset
  └── 6. PUSH TO HUB --> Force immediate RF backup push to Hub
```

### 1. `ACTIVITIES`
- **`Confirm Switch`**: `[ PROMPT ]` or `[ INSTANT ]`.
  - When set to `PROMPT`, initiating an activity change or power-off displays a confirmation dialog with a 3-second timeout before issuing commands.
- **`Remote Mode`**: `[ ACTIVITY ]` or `[ DIRECT ]`.
  - `ACTIVITY`: Smart activity-based remote control (AV commands routed according to active activity).
  - `DIRECT`: Direct device control mode (commands routed directly to selected device).

### 2. `DISPLAY`
- **`Display Brightness`**: `[ LVL 1 ]` to `[ LVL 5 ]`. Adjusts LCD backlight PWM duty cycle.
- **`Display Timeout`**: `[ 10s ]`, `[ 15s ]`, `[ 20s ]`, `[ 30s ]`, `[ 60s ]`, `[ 120s ]`. Screen idle blanking timer.
- **`Button Backlight`**: `[ OFF ]`, `[ ON ]`, `[ NIGHT ]`.
  - `OFF`: Keypad white LEDs disabled.
  - `ON`: Keypad LEDs illuminated while screen is awake.
  - `NIGHT`: Keypad LEDs illuminated only during configured night hours.
- **`Night Schedule`**: `[ HH-HH ]` (e.g. `[ 22-07 ]`). Configures the 24-hour window for night backlight behavior.

### 3. `TOUCH`
- **`Touchscreen`**: `[ ON ]` or `[ OFF ]`.
  - When `OFF`, the Synaptics Clearpad input device is ignored, and the remote operates exclusively via physical buttons with `NAV` mode permanently engaged.
- **`Touch Sensitivity`**: `[ LOW ]`, `[ NORMAL ]`, `[ HIGH ]`.
  - Adjusts tap slop tolerances (16px / 20px / 25px) and swipe velocity thresholds.
- **`Nav Toggle Button`**: `[ MENU ]`, `[ INFO ]`, `[ LIST ]`, `[ GUIDE ]`, `[ EXIT ]`.
  - Hardware button assigned to toggle between `DEV` and `NAV` modes.
- **`Nav Toggle Mode`**: `[ SHORT ]` or `[ LONG ]`.
  - `SHORT`: Instant toggle on key down.
  - `LONG`: Long press ($\ge 500\text{ ms}$) toggles; short tap sends original button function.

### 4. `POWER`
- **`Motion / Gyro Wake`**: `[ ON ]` or `[ OFF ]`.
  - Enables or disables LIS3DH lift-to-wake in Light Standby.
- **`Gyro Sampling Rate`**: `[ 10Hz ]`, `[ 25Hz ]`, `[ 50Hz ]`.
  - Sysfs accelerometer rate when remote is in active or near-standby state.
- **`Battery Status`**: `[ XX% XXXXmV ]`.
  - Refreshes live hardware voltage and battery percentage filtered via a 16-sample moving average window.

### 5. `SYSTEM`
- **`Software Version`**: Displays current firmware version string (e.g. `v1.3.0`).
- **`USB Ethernet Mode`**: `[ OFF ]`, `[ ACTIVE ]`, `[ UNPLUGGED ]`.
  - Controls `/usr/bin/usbeth`. When `ACTIVE`, USB gadget is up and plugged in. When `UNPLUGGED`, gadget is up awaiting USB connection.
- **`Power Debug Log`**: `[ ON ]` or `[ OFF ]`.
  - Toggles high-resolution power logging to `/tmp/power_debug.log` and `/data/power_debug.log` (logs standby transitions, voltage, CPU frequencies, and wake sources). Local setting, not pushed to Hub.
- **`RF Pairing Menu`**: `[ LINKED ]` or `[ OPEN ]`.
  - Opens CC2544 pairing management: inspect remote address, paired Hub address, link RSSI, request pairing, or unpair.
- **`Reboot Remote`**: Clean reboot confirmation -> flushes flash buffers and executes `reboot -f`.
- **`Factory Reset`**: Two-stage safety confirmation -> removes custom startup scripts, restores stock Logitech UI binaries, and reboots.

### 6. `PUSH TO HUB`
- Immediately sends an `RF_MSG_REMOTE_CONFIG_BACKUP` (`0x19`) frame to the Hub to back up all current remote settings into `/data/codex/elite_slot<slot>_config.json`.

---

## Hub-to-Remote Settings Configuration & Sync

Settings on the Elite remote can be inspected and updated remotely from the Harmony Hub via REST API and over-the-air RF frames.

### Hub REST API Endpoints

#### 1. Query Remote Configuration
```http
GET /api/rf/remote-config?slot=1 HTTP/1.1
Host: 192.168.1.1:8088
```
**Response (`200 OK`)**:
```json
{
  "slot": 1,
  "brightness": 3,
  "timeout": 30,
  "touchEnabled": true,
  "capButtonsEnabled": true,
  "gyroEnabled": true,
  "gyroRate": 25,
  "navToggleKey": 139,
  "navToggleMode": 1,
  "confirmActivity": false,
  "touchSensitivity": 2,
  "remoteMode": 0,
  "btnBacklight": 1,
  "nightStart": 22,
  "nightEnd": 7,
  "usbEth": false
}
```

#### 2. Update Remote Configuration from Hub
```http
POST /api/rf/remote-config HTTP/1.1
Host: 192.168.1.1:8088
Content-Type: application/json

{
  "slot": 1,
  "brightness": 4,
  "timeout": 20,
  "touchEnabled": true,
  "capButtonsEnabled": true,
  "gyroEnabled": true,
  "gyroRate": 25,
  "navToggleKey": 139,
  "navToggleMode": 1,
  "confirmActivity": true,
  "btnBacklight": 2,
  "nightStart": 23,
  "nightEnd": 6,
  "usbEth": false
}
```
**Response (`200 OK`)**:
```json
{
  "status": "saved_and_pushed",
  "ok": true
}
```

### Bidirectional RF Protocol Synchronization
The Hub and Elite keep configuration synchronized using 17-byte binary payload structures:

1. **Hub -> Remote Push (`RF_MSG_SETTINGS_PUSH`, `0x0E`)**:
   - Sent when settings are POSTed to the Hub API, or during full config sync (`RF_MSG_CONFIG_END`).
   - Requires ACK from Remote (`RF_FLAG_ACK_REQ`).
   - The Remote applies all display, timeout, and sensor settings immediately, updates `/data/codex_elite.conf`, and repaints the screen.
2. **Remote -> Hub Backup (`RF_MSG_REMOTE_CONFIG_BACKUP`, `0x19`)**:
   - Sent whenever settings are modified in the Elite on-screen menu, or when the user selects `PUSH TO HUB`.
   - The Hub receives the frame and persists the configuration to `/data/codex/elite_slot<slot>_config.json`.
3. **17-Byte Payload Format**:
   - `Byte 0`: Display Brightness (1–5)
   - `Byte 1`: Screen Timeout low byte (seconds)
   - `Byte 2`: Touchscreen Enable flag (0/1)
   - `Bytes 3-4`: Nav Toggle Key Linux code (uint16 LE, e.g. 139 = MENU)
   - `Byte 5`: Nav Toggle Mode (0=Short, 1=Long)
   - `Byte 6`: USB Ethernet Enable flag (0/1)
   - `Byte 7`: Confirm Activity Switch flag (0/1)
   - `Byte 8`: Config Version number
   - `Byte 9`: Touch Sensitivity (1=Low, 2=Normal, 3=High)
   - `Byte 10`: Remote Mode (0=Activity, 1=Direct)
   - `Byte 11`: Motion/Gyro Wake Enable flag (0/1)
   - `Byte 12`: Gyro Sampling Rate in Hz (10, 25, 50)
   - `Byte 13`: Button Backlight Mode (0=Off, 1=On, 2=Night)
   - `Byte 14`: Night Start Hour (0–23)
   - `Byte 15`: Night End Hour (0–23)
   - `Byte 16`: Screen Timeout high byte (seconds)
   - `Byte 17`: Capacitive Buttons Enable flag (0/1)
   - `Byte 18`: Power Debug Log Enable flag (0/1)

---

## Official Firmware Management & Icon Extraction

The custom firmware environment includes a clean-room HID flashing tool (`tools/harmony_firmware.py`) that queries official Logitech SUS update servers, downloads official signed firmware bundles (`.hfw2`), flashes devices over USB HID without Logitech software, and extracts all remote UI assets.

### 1. Check & Flash Official Elite Firmware
```bash
# Query latest version from Logitech update servers:
python tools/harmony_firmware.py check

# Download and flash directly over USB HID:
python tools/harmony_firmware.py flash --target elite
```

### 2. Extract Official Remote UI Icons for Hub Web UI
The Elite firmware SquashFS filesystem contains 1,347 official UI icons (activity icons, command glyphs, power toggles, device branding). You can unpack and archive these directly on any platform with Python, without requiring an Elite remote connected over USB or SSH:

```bash
python tools/harmony_firmware.py extract-icons --archive build/icons.tar.gz
```
The resulting `build/icons.tar.gz` (1.55 MB compressed) is automatically staged to `/cache/icons.tar.gz` during `deploy_hub.ps1` or `deploy_hub.sh`.