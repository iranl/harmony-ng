# Logitech Harmony Remote Hacking & Support

> [!NOTE]
> **Hardware Family Compatibility**:
> The **Harmony Pro 2400 Remote** (Skin 116), Harmony Ultimate (Home), and Harmony Touch share the same underlying architecture (TI DaVinci ARM, CC2544 RF, ClearPad touch, 240x320 RGB565 LCD). Pro 2400 remote runs Dropbear SSH out of the box in stock firmware 10.0.215. All are supported using the same tooling and `remote_ui` binary (*untested on physical Pro 2400 remote hardware*). Tested on a Harmony Ultimate (2013, skin 100, firmware 4.15.330); its differences are listed in [FIRST_INSTALL_REMOTE.md](FIRST_INSTALL_REMOTE.md#harmony-ultimate-2013).

## Serial Console & U-Boot (UART)
Baud rate: `115200 8N1` on serial pad.

To prevent kernel console suspend when remote idles:
```
printenv bootargs
setenv bootargs ${bootargs} no_console_suspend
saveenv
boot
```

### Serial Bootstrap (`remote/bootstrap_serial.py`)
To push Dropbear, generate SSH keys, enable USB Ethernet, and install the recovery boot script directly over serial:
```sh
pip install pyserial
python tools/bootstrap_serial.py --port COM3 --ssh-pubkey ~/.ssh/harmony_owner_ed25519.pub
```
This utility:
1. Starts `/usr/bin/usbeth start` on the remote.
2. Streams base64-encoded static binaries (`dropbear`, `dropbearkey`, `scp`, `remote_sync`) to `/data/`.
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
  scp -O file.bin remote:/data/
  ```

---

## SSH Deployment Script (`remote/deploy_remote.py`)
Once SSH is operational, deploy updated custom firmware (`remote_ui`), tools, and boot scripts:
```sh
# Cross-platform (Linux/macOS/Windows)
python remote/deploy_remote.py --ip 192.168.2.1 --key ~/.ssh/harmony_owner_ed25519
```
Before it changes anything on the remote, `deploy_remote.py` runs a ready-to-go check and stops, with what to do, if any of these fails:
- `remote/bin/remote_ui` and `remote/bin/remote_sync` are built.
- The remote is an ARM touchscreen remote with the commands the deployment runs, including `scp`.
- A BusyBox with `tar` can unpack the themes. Stock BusyBox 1.13.4 has no `tar`, so `/data/busybox` must already have one, or `remote/bin/busybox` (from `./build/build_busybox.sh arm`) must be there to install first. The check runs a new `remote/bin/busybox` from `/tmp`.
- `/data` has room for the changed files and the themes, each written beside its old copy.

`--check` runs only the check. The deployment installs `busybox` before any other file and unpacks the themes with `/data/busybox tar`.

This script uploads:
- `remote_ui` (custom framebuffer UI & CC2544 RF engine)
- `remote_sync` (ARM sync helper, symlinked to `/data/sync` and `/usr/bin/sync`)
- `remote_rf_query` & `remote_rf_sniff`
- `/etc/init.d/rcS.local` with panic recovery counter

---

## Files Named `elite_*` (Older Builds)
Builds before the elite -> remote rename used `elite_*` names. Nothing has to be done by hand:

- On the remote, `remote_ui` renames them at start (`init.sh start` and `init.sh migrate` do the same): `/data/elite_ui`, `elite_sync`, `elite_watchdog.sh`, `elite_settings.conf`, `elite_ui.conf`, `elite_rf_config.json`, `elite_activity_usage.conf`, `elite_ui.bak` and `elite_conf_p/` become `remote_*`. A file that already exists under the new name is kept. The old binary, sync and watchdog names stay as symlinks, so an older supervisor or a rollback still finds them.
- An RF update from a remote on an older build works: the package also carries the binary as `elite_ui`, the name older installers look for.
- `deploy_remote.py` renames the files before it uploads.
- On the Hub, `hub_rf` renames `elite_slot<N>_version.json`, `elite_slot<N>_config.json` and a staged `elite_ui.ota.gz` / `elite_ui.version`. MQTT settings saved with `eliteRemoteTopic` / `eliteRemoteEvents` keep working (now `remoteTopic` / `remoteEvents`), and the button event source `elite_rf` is still accepted next to `remote_rf`.
- The tools still accept `--target elite` and `compile_all.sh elite` as aliases of `remote`.

---

## Emergency Recovery (Panic Loop Protection)
The remote's `/etc/init.d/rcS.local` maintains a persistent reboot counter at `/data/reboot_counter`:
- On boot, counter increments and writes to `/data/reboot_counter`.
- If uptime reaches **5 minutes (300 seconds)** without crashing, counter resets to `0`.
- **Safe Mode**: If the device reboots more than **5 consecutive times** before reaching 5-minute stability (`COUNT > 5`), it enters Emergency Recovery Mode:
  - Enables **only** USB Ethernet (`/usr/bin/usbeth start`) and Dropbear SSH (`/data/dropbear -R -B`).
  - Logs alert to `/tmp/emergency_recovery.log`.
  - Exits boot script **without** launching `remote_ui` or any other software.
  - Allows full SSH root access at `192.168.2.1` to inspect logs, repair files, or reset the counter (`echo 0 > /data/reboot_counter`).

---

## Note on Busybox `sync`
BusyBox v1.13.4 on both the Hub and the remote lacks the `sync` applet. Never run raw `sync` in scripts. Always use:
```sh
sync 2>/dev/null || /data/remote_sync 2>/dev/null || /data/sync 2>/dev/null || true
```

---

## Frame Timing

`remote_ui` can log how long each screen update takes, to check screens against the 27.8 ms animation frame budget. Over SSH:

```sh
touch /tmp/remote_frame_timing     # start logging, within a second
tail -f /tmp/frame_timing.log
rm /tmp/remote_frame_timing        # stop
```

Each line is `ms screen theme layout render_us present_us flipped decodes scales`:
- `ms`: monotonic time;
- `screen`: what the frame shows (`activities`, `activity`, `devices`, `settings/N`, `menu`, `help`, …);
- `theme`, `layout`: the theme's name and the home layout (`list` or `grid`);
- `render_us`: time to draw the frame;
- `present_us`: time to show it;
- `flipped`: `1` when the frame was shown by page flipping, `0` when it was copied into every framebuffer page;
- `decodes`, `scales`: PNG images decoded, and scaled to a new size, while drawing the frame. Both are 0 once a screen's images are cached.

A screen change that slides or crossfades (`transition` lines) logs each frame of the transition with the time to composite it as `render_us`. The frame that started it logs the whole transition as its `present_us`.

The log is in `/tmp`, so it is lost at reboot.

---

## Sleep Behavior & Expected Run Times

The Harmony Elite remote features a **1,300 mAh (3.7V Li-ion, ~4.81 Wh)** battery. In custom firmware (`remote_ui`), sleep is managed via a two-tier hardware power state architecture: **Light Standby** and **Deep Standby**.

### Standby Power States & Wake Triggers

| Standby Tier | Timeout Range | Hardware State | Wake Triggers |
| :--- | :--- | :--- | :--- |
| **Active** | While interacting | Screen ON (PWM 1-5), CPU @ 456 MHz, Clearpad touch active, gyro polling @ 10-50 Hz | N/A |
| **Light Standby** | **0 – 30 minutes** after screen blank | Screen OFF (`FB_BLANK_POWERDOWN`), Backlight 0, Clearpad Touch ASLEEP, CPU in WFI, LIS3DH INT1 IRQ active (0 I2C reads) | • **Pickup / Motion** (LIS3DH INT1 hardware IRQ)<br>• **Any Physical Button** |
| **Deep Standby** | **> 30 minutes** after screen blank | Gyro I2C polling **completely suspended (0 Hz)**, `select()` timeout scaled to 30s, CPU in deep WFI | • **Any Physical Button ONLY** (Power, Activities, Devices, OK, Vol, D-pad, etc.) |

Deep standby is the kernel's suspend to RAM, on battery only. The **Deep standby** setting picks
when it starts after the screen blanks: **at once** (the default since 2026-11, as the stock
firmware does: 2 s after the blank, the time for a key release and its acknowledgement), 15, 30,
60, 120 or 240 minutes, or never. In suspend only a physical button, the cradle and the hourly
MSP430 alarm wake the remote; RF wake requests from the Hub do not reach it. After an alarm wake
the remote stays up until it has linked and reported its battery (at most 8 s), then suspends
again, so a sleeping remote reports once an hour. **Powered (cradle, USB or charging) it never
suspends**: the Hub can wake it, push config and update it over RF at any time.

`CLOCK_MONOTONIC` stops during suspend and the remote's kernel (2.6.37) has no `CLOCK_BOOTTIME`,
so `remote_ui` measures each suspend on the wall clock (restored from the MSP430 RTC on resume)
and adds it in `get_boot_ms()`. The battery's timing (rested reading, slow drop, reports) and the
time since the screen blanked use that clock. After a resume the battery samples start fresh,
the first reading counts as rested when the suspend lasted 30 s or more (the cell rested all that
time), and a battery report is requested, for an alarm wake and a button wake alike.

> [!NOTE]
> Touchscreen taps do **not** wake the remote in either standby mode. The Synaptics Clearpad sensor is put into hardware sleep mode (`0x35 = 0x01`) upon display blanking to prevent accidental touches and save ~10.5 mA.

### Power Optimizations & Measured Run Times

Live hardware telemetry was captured via battery ADC moving-average tracking (`/sys/devices/platform/i2c_davinci.1/i2c-1/1-0048/tps6507x-juniper/vbat`):

| Firmware State | Hourly Battery Drain | Standby Current | Projected Standby Run Time | Measured Improvement |
| :--- | :---: | :---: | :---: | :---: |
| **Baseline (Unpatched)** | **7.8% / hour** | ~101 mA | **~12.8 hours** | Baseline (`FB_BLANK_NORMAL` failed with `EINVAL`, LCD DMA active) |
| **Stage 1 (LCD Controller Shutdown)** | **2.5% / hour** | ~32.5 mA | **~40.0 hours (1.7 days)** | **3.1x longer** (Verified: 13% drop across 5h 08m uninterrupted sleep) |
| **Stage 2 (Clearpad Sleep + Deep Standby)** | **~1.31% / hour** | **~17.0 mA** | **~76.5 hours (~3.2 days)** | **~6.0x longer** (>30m gyro suspended, 30s select loop, touch matrix off) |

### Key Power Mechanisms in `remote_ui`
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

## Physical Button Navigation

The remote can be operated **100% via physical buttons**, allowing full control without touching the screen (e.g. if the touchscreen is disabled, broken, or when navigating blindly).

### Capacitive Fixed Buttons (Directly Below LCD)
- **Left Button (`KEY_FAVORITES`, code 364)**: Jumps immediately to the **ACTIVITIES** view.
- **Right Button (`KEY_HOME`, code 102)**: Jumps immediately to the **DEVICES** view.
*These buttons never transmit over RF to the Hub; they exclusively control UI navigation.*

### Bottom Navigation Row (Dimmer & Smart Home Buttons)
With touch enabled, these keys go to the Hub like every other button, except in the **SETTINGS** and **PAIRING** views. With touch disabled in Settings (`touch=0`), they drive the screen everywhere, and a NAV indicator shows in the status bar:

| Button Name | Linux Input Code | Function when driving the screen |
| :--- | :---: | :--- |
| **Rocker Up (Dimmer +)** | `78` (`KEY_KPPLUS`) | Cursor Up / Previous card or item |
| **Rocker Down (Dimmer -)** | `74` (`KEY_KPMINUS`) | Cursor Down / Next card or item |
| **Light On** | `148` (`KEY_PROG1`) | Select / OK / Execute / Confirm switch |
| **Light Off** | `149` (`KEY_PROG2`) | Tab Left / Previous Category |
| **Plug On** | `202` (`KEY_PROG3`) | Back / Cancel / Dismiss confirmation |
| **Plug Off** | `203` (`KEY_PROG4`) | Tab Right / Next Category |
| **Back Key** | `158` (`KEY_BACK`) | Back / Cancel / Dismiss confirmation |

---

## Device Browser & Physical Button Control

Every device configured on the Hub can be controlled directly, without starting an activity:

- **Physical navigation keys**: the screen has no navigation dock; the keys below the display navigate:
  1. `Activities` key: the activity list. Holding it for 0.8 s opens the menu.
  2. `Devices` key: the Hub Device Browser, listing all devices configured on the Hub. Pressing it inside a device returns to the list.
  3. `Off` key: powers off the current activity.
  4. Tapping the running activity on the activity list opens its screen buttons, as does starting an activity. An activity without screen buttons shows a card with its icon, name and devices.
- **Status bar**: the gear at the left opens the menu (`Help` while an activity runs, and `Settings`); the time is at the right. A NAV indicator appears while touch is disabled.
- **Help**: re-sends the running activity's commands except power toggles, then asks "Is it on?" for each device whose only power command is a toggle; `No` sends the toggle. Answer by tapping `Yes` or `No`, or with the D-pad and `OK`; `Back`, `Activities` or `Devices` closes Help. A summary shows for 3 s, then the activity screen.
- **RF link indicator**: four bars show the link to the Hub over the last 30 s, from the share of CC2544 link reports that say "established" and the share of the remote's sync queries (every 800 ms while the screen is on) that the Hub answers within 1 s; the lower share sets the bars (≥ 90 % four, ≥ 70 % three, ≥ 40 % two, otherwise one amber bar). A red `NO HUB` shows when the link has been lost for 6 s, and a red `HUB?` when the link is up in at least 90 % of reports but the Hub has answered none of the last 10 s of queries. Tapping the indicator opens a panel with both shares and the time since the last frame from the Hub; any tap or key closes it.
- **Docking**: putting the remote on the dock or plugging in USB wakes the screen and shows a full-screen `Charging` page with the battery level for 4 s; any key or tap closes it. While charging, the status bar battery fills teal.
- **First start**: after the update, a card names the Activities, Devices and Off keys once; any key or tap dismisses it.
- **Hub Device Browser**: Selecting a device lists all of its commands (6 per page) for touch execution, below the device's name.
- **Physical Button Control in Device Browser**: When viewing any device in the Hub Device Browser, physical remote button presses (Volume, Mute, D-pad, Play/Pause, Channel, etc.) automatically transmit the target device ID to the Hub.
- **Hub Mapping & Auto-Fallback**:
  - The Hub WebUI provides a **Mapping Scope** toggle (`Per Activity` vs `Per Device (Device Browser)`) to configure custom physical button actions per device.
  - If a physical button is not explicitly mapped for a device, the Hub automatically matches standard button names (e.g. `VolumeUp`, `Mute`, `Play`, `DirectionUp`, `Select`) against available device commands in `devices.json`.
- **Shortened Device IDs**: Device IDs are automatically migrated and shortened (`"1"`, `"2"`, ...) across all config files, freeing RF payload space for longer device names. Names longer than the list chunk holds arrive in `RF_MSG_NAME_TAIL_CHUNK` frames, up to 39 characters.
- **Back**: Swipe left-to-right; with touch disabled, press `Prev Tab` (`KEY_PROG2`) or `Back`. At the root device list, going back opens the running activity's screen, or the activity list when none is running.

---

## Complete Remote Settings Menu Reference

The remote's settings menu is structured into 5 categories plus an RF sync push action:

```
[SETTINGS]
  ├── 1. ACTIVITIES  --> Confirm switch, remote mode
  ├── 2. DISPLAY     --> Brightness, timeout, backlight, night schedule
  ├── 3. TOUCH       --> Touchscreen on/off, capacitive keys, sensitivity
  ├── 4. POWER       --> Gyro motion wake, sampling rate, battery health
  ├── 5. SYSTEM      --> USB ethernet, power debug log, pairing, reboot, factory reset
  └── 6. PUSH TO HUB --> Force immediate RF backup push to Hub
```

Pages have no Back item: swipe left to right, or press `Back`, to return to the categories. From the categories the same goes back to the home screen.

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
- **`Theme`**: cycles through Classic and the installed themes.
- **`Home Layout`**: `[ THEME ]` (the theme's layout), `[ LIST ]`, `[ GRID ]` or `[ CAROUSEL ]` (one activity per page, dragged sideways).

### 3. `TOUCH`
- **`Touchscreen`**: `[ ON ]` or `[ OFF ]`.
  - When `OFF`, the Synaptics Clearpad input device is ignored, and the remote operates exclusively via physical buttons; the bottom keys drive the screen.
- **`Touch Sensitivity`**: `[ LOW ]`, `[ NORMAL ]`, `[ HIGH ]`.
  - Adjusts tap slop tolerances (16px / 20px / 25px) and swipe velocity thresholds.

### 4. `POWER`
- **`Motion / Gyro Wake`**: `[ ON ]` or `[ OFF ]`.
  - Enables or disables LIS3DH lift-to-wake in Light Standby.
- **`Gyro Sampling Rate`**: `[ 10Hz ]`, `[ 25Hz ]`, `[ 50Hz ]`.
  - Sysfs accelerometer rate when remote is in active or near-standby state.
- **`Battery Status`**: `[ XX% XXXXmV ]`.
  - Refreshes live hardware voltage and battery percentage filtered via a 16-sample moving average window.
  - The percentage (`remote_battery.c`): the voltage maps to a curve through the stock firmware's points for this remote (3.55 V shutdown = 0 %, 3.70 V = 30 %, 3.85 V = 75 %, 3.90 V = 90 %, 3.95 V and up = 100 %, linear between), minus the charging offset while charging (measured at each charging start as the first charging reading minus the last rested one on battery, 0.05 to 0.25 V: about 0.1 V on USB, 0.17 V on the cradle; 0.1 V when the remote starts while already charging). A rested reading (screen off for 30 s) sets it; under load it never rises and falls by at most one point per 2 minutes, because the cell sags 0.1 to 0.15 V within seconds of the screen turning on; while charging it only rises; a change of the charging state keeps the last value, and the state itself counts only after it held for 5 s (lifting the remote off the cradle flips it several times). The remote reports the battery every 5 minutes, at once on a charging change, with the screen on for a change of 2 points or more (at most once a minute), and at once when its link to the Hub comes up and with every version report, so a Hub that restarted gets it as soon as the remote is linked.

### 5. `SYSTEM`
- **`Software Version`**: Displays current firmware version string (e.g. `v1.3.0`).
- **`Automatic USB Ethernet`**: Handled automatically in hardware. When connected to a Micro-USB cable, USB Ethernet (`192.168.2.1`) and Dropbear SSH start automatically. When running on battery or in cradle, USB Ethernet and Dropbear are cleanly stopped to ensure minimal/no battery drain.
- **`Power Debug Log`**: `[ ON ]` or `[ OFF ]`.
  - Toggles high-resolution power logging to `/tmp/power_debug.log` and `/data/power_debug.log` (logs standby transitions, voltage, CPU frequencies, and wake sources). Local setting, not pushed to Hub.
- **`RF Pairing Menu`**: `[ LINKED ]` or `[ OPEN ]`.
  - Opens CC2544 pairing management: inspect remote address, paired Hub address, link RSSI, request pairing, or unpair.
- **`Reboot Remote`**: Clean reboot confirmation -> flushes flash buffers and executes `reboot -f`.
- **`Factory Reset`**: Two-stage safety confirmation -> removes custom startup scripts, restores stock Logitech UI binaries, and reboots.
  - *Hardware Factory Reset Alternative (No UI / SSH needed)*: Hold **`Menu`** (above `CH+`) and **`Off`** (top power button) simultaneously for ~20 seconds. The remote graphic with circular arrow will flash briefly first—**continue holding until the graphic stays permanently on the screen**. The remote then wipes user configurations and restores factory defaults.

### 6. `PUSH TO HUB`
- Immediately sends an `RF_MSG_REMOTE_CONFIG_BACKUP` (`0x19`) frame to the Hub to back up all current remote settings into `/data/harmony/remote_slot<slot>_config.json`.

---

## Hub-to-Remote Settings Configuration & Sync

Settings on the remote can be inspected and updated remotely from the Harmony Hub via REST API and over-the-air RF frames.

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
  "gyroRate": 50,
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
  "gyroRate": 50,
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
The Hub and the remote keep configuration synchronized using 17-byte binary payload structures:

1. **Hub -> Remote Push (`RF_MSG_SETTINGS_PUSH`, `0x0E`)**:
   - Sent when settings are POSTed to the Hub API, or during full config sync (`RF_MSG_CONFIG_END`).
   - Requires ACK from Remote (`RF_FLAG_ACK_REQ`).
   - The Remote applies all display, timeout, and sensor settings immediately, updates `/data/remote_settings.conf`, and repaints the screen.
2. **Remote -> Hub Backup (`RF_MSG_REMOTE_CONFIG_BACKUP`, `0x19`)**:
   - Sent whenever settings are modified in the remote's on-screen menu, or when the user selects `PUSH TO HUB`.
   - The Hub receives the frame and persists the configuration to `/data/harmony/remote_slot<slot>_config.json`.
3. **17-Byte Payload Format**:
   - `Byte 0`: Display Brightness (1–5)
   - `Byte 1`: Screen Timeout low byte (seconds)
   - `Byte 2`: Touchscreen Enable flag (0/1)
   - `Bytes 3-5`: Nav toggle key and mode of older firmware; current firmware sends 0
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

## Harmony Companion & Smart Control Screenless Remotes

HarmonyNG provides complete, native support for Logitech's button-only screenless RF remotes: the **Harmony Companion** and **Harmony Smart Control**.

### Hardware Comparison

| Feature | Touchscreen remotes (Elite, Ultimate) | Harmony Companion | Harmony Smart Control |
| :--- | :--- | :--- | :--- |
| **Model ID / eQuad ID** | `0x4021` (`"Juniper"`) | `0x4026` / `0x4027` (`"Companion"`, `"Home Control"`) | `0x401E` (`"20+"`) |
| **Display** | 240x320 RGB565 LCD + ClearPad Touch | None (Button-only) | None (Button-only) |
| **Operating System** | Embedded Linux (TI DaVinci ARM9) | Ultra-low-power RF micro-controller | Ultra-low-power RF micro-controller |
| **Power Source** | Li-ion Rechargeable (1300 mAh on the Elite) | 1x CR2032 Coin Cell | 1x CR2032 Coin Cell |
| **Rooting / USB Setup** | Required (USB rooting, `remote_ui`) | **None** (Zero-touch, native RF) | **None** (Zero-touch, native RF) |
| **Home Automation Keys** | Touchscreen devices & smart home | Dedicated physical buttons (Lights 1/2, Plugs 1/2, Dimmer +/-) | Standard media/nav buttons only |
| **Web UI Visual Skin** | Touchscreen remote shell | Companion shell with smart home keys | Smart Control layout |

### Zero-Touch Pairing (No Rooting Needed)

Unlike the touchscreen remotes, Companion and Smart Control remotes do not run Linux and have no USB interface. They communicate directly with the Hub via the Texas Instruments CC2544 2.4 GHz RF link:

1. **Start Pairing on Hub**:
   - In the HarmonyNG Web UI, navigate to the **Remotes** tab and click **Pair Remote** under Slot 1 or Slot 2.
   - Alternatively, call the REST API: `POST /api/rf/pair?slot=1` (or `slot=2`), or press the physical **Pair/Reset** button on the back of the Harmony Hub.
2. **Put Remote into Pairing Mode**:
   - Hold **`Menu`** and **`Mute`** buttons simultaneously until the status LED at the top of the remote starts flashing rapidly.
3. **Automatic Binding**:
   - The Hub radio exchanges pairing keys, assigns the remote to the selected slot, and persists the RF pairing table.
   - The Web UI immediately detects the remote type, updates the status badge (e.g. `Harmony Companion (Slot 1)`), and loads the appropriate button layout.

### eQuad RF Framing & Key Dispatching

When a button is pressed or released on a Companion or Smart Control remote, it transmits 32-byte eQuad frames over Report `0x20`:
- **HID Keyboard Reports (Sub-ID `0x01`)**: Carries standard alphanumeric and modifier codes (e.g. `KEY_1`..`KEY_9`, `KEY_0`, `KEY_ENTER`, `KEY_MENU`).
- **HID Consumer Control Reports (Sub-ID `0x03`)**: Carries 16-bit consumer page usages (e.g. `0x00E9` Volume Up, `0x00EA` Volume Down, `0x00E2` Mute, `0x00B5` Scan Next, `0x0041` Menu Pick).

`hub_rf` decodes these reports in real-time within `rf_spi.c` and `rf_keymap.c`, tracking press, hold, and release states without CPU overhead. Mapped commands are executed immediately on the Hub:
- **IR Pulsing**: Pulses hardware I2S registers with zero latency.
- **Bluetooth HID**: Transmits media keys or HID strokes to paired Apple TV, Fire TV, or Shield devices.
- **Activity Switching**: Top activity keys (Watch TV, Watch Movie, Listen to Music, All Off) trigger the orchestrator.
- **MQTT Event Mirroring**: Publishes real-time button events to your home automation broker.

### Per-Slot Configuration & Mapping Copy

- **Independent Mapping Files**:
  - Slot 1 mappings are stored in `/data/harmony/remote_slot1_map.json` (falling back to legacy `elite_remote_map.json` for backwards compatibility).
  - Slot 2 mappings are stored in `/data/harmony/remote_slot2_map.json`.
- **Copy Mappings Between Slots**:
  - If you use identical remotes or want to replicate mappings from Slot 1 to Slot 2, use the **Copy to other slot** button in the Web UI, or invoke `POST /api/rf-mapping-copy?from=1&to=2`.
- **Dynamic Feature Hiding**:
  - For screenless remotes, the Web UI automatically hides touchscreen configuration ("DEVICES" touch buttons) and over-the-air firmware updates (`/api/rf/ota-*`), focusing purely on physical button assignments.

---

## Official Firmware Management & Icon Extraction

The custom firmware environment includes a clean-room HID flashing tool (`tools/harmony_firmware.py`) that queries official Logitech SUS update servers, downloads official signed firmware bundles (`.hfw2`), flashes devices over USB HID without Logitech software, and extracts all remote UI assets.

### 1. Check & Flash Official Remote Firmware
```bash
# Query latest version from Logitech update servers:
python tools/harmony_firmware.py check

# Download and flash directly over USB HID:
python tools/harmony_firmware.py flash --target remote
```

### 2. Extract Official Remote UI Icons for Hub Web UI
The Elite firmware SquashFS filesystem contains 1,347 official UI icons (activity icons, command glyphs, power toggles, device branding). You can unpack and archive these directly on any platform with Python, without requiring a remote connected over USB or SSH:

```bash
python tools/harmony_firmware.py extract-icons --archive build/icons.tar.gz
```
The resulting `build/icons.tar.gz` (1.55 MB compressed) is automatically staged to `/cache/icons.tar.gz` during `deploy_hub.py`.