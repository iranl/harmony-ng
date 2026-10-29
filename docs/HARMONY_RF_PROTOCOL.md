# Logitech Harmony Hub & Elite Remote RF Protocol Specification

## 1. Overview & Architecture

Logitech Harmony Hub and Harmony Elite (and Companion) remotes communicate locally using a proprietary 2.4 GHz RF protocol implemented on **Texas Instruments CC2544** 2.4 GHz RF SoCs. The host processors communicate with the CC2544 over synchronous SPI via the `/dev/rfspi` character device.

```
+------------------------------------+              2.4 GHz eQuad RF Link             +------------------------------------+
|       Harmony Hub (Host Mode)      |                                                |      Harmony Elite (Device Mode)   |
|                                    |                                                |                                    |
| Atheros AR9344 SoC (MIPS 24Kc)    |                                                | TI DaVinci DM365 SoC (ARM926EJ-S)  |
|   |                                |                                                |   |                                |
|   | /dev/rfspi (SPI Master)        |                                                |   | /dev/rfspi (SPI Master)        |
|   v                                |                                                |   v                                |
| TI CC2544 (Host / Dongle Firmware) | <============================================> | TI CC2544 (Device Firmware)        |
+------------------------------------+                                                +------------------------------------+
```

---

## 2. Hardware Interfaces & Kernel Drivers

### 2.1 Hub (`cc2544.ko` on Atheros AR9344)
- **Shared SPI Bus**: The Atheros AR9344 SoC multiplexes SPI lines between the CC2544 radio and the NOR flash memory (`ar7240_flash_spi_down()` / `ar7240_flash_spi_up()`).
  - **CRITICAL CONSTRAINT**: Multiple processes must **never** open `/dev/rfspi` concurrently. Doing so corrupts the SPI arbiter register, hangs the NOR flash bus, and freezes the kernel.
- **Interrupts**: GPIO 14 (mapped to IRQ 46 / `0x2E`) handles CC2544 data-ready notifications.
- **Kernel I/O Model**: `cc2544.ko` does not implement `.poll` or `.select`. `read()` on `/dev/rfspi` blocks until an IRQ fires. Dedicated worker threads are required.

### 2.2 Remote (`cc2544_remote.ko` on TI DaVinci DM365)
- **Dedicated SPI**: The DM365 communicates over a dedicated SPI bus without flash bus multiplexing.
- **Interrupts**: CC2544 IRQ triggers `cc2544_spi_notif_interrupt`.
- **Kernel I/O Model**: `cc2544_remote.ko` ignores `O_NONBLOCK` and does not support `select`/`poll`. `read()` blocks until a packet arrives.

---

## 3. SPI Framing & Transport Layer

The SPI driver inspects byte 0 of every incoming or outgoing transfer to determine packet length:

| Report ID (Byte 0) | Packet Size | Description |
| :--- | :--- | :--- |
| `0x10` | 7 bytes | HID++ Short Report (Command / Link Event / Control) |
| `0x11` | 20 bytes | HID++ Long Report (Pipe table query response / info) |
| `0x12` | 30 bytes | HID++ Very Long Report (**Hub Host-Mode Data Frame**) |
| `0x20` | 32 bytes | Vendor Extended Report (**Remote Device-Mode Data Frame**) |

### 3.1 Hub Host Framing (30-byte Report `0x12`)
In stock `hal_hub` (`libhal/rf/hot.c`, host mode `DAT_00466f70 == 0`), all bidirectional air messages between Hub CPU and CC2544 use **Report `0x12`**:

```
+--------+--------+----------+----------+--------+--------------------------+
| Byte 0 | Byte 1 |  Byte 2  |  Byte 3  | Byte 4 |       Bytes 5..29        |
|  0x12  |  Slot  | Hdr/Seq  | Msg Type | Flags  |     Payload (25 bytes)   |
+--------+--------+----------+----------+--------+--------------------------+
```
- **Byte 0 (`0x12`)**: Report ID.
- **Byte 1 (`Slot`)**: Destination or source remote slot (`0x01` for Slot 1, `0x02` for Slot 2).
- **Byte 2 (`Hdr/Seq`)**: Header flags and sequence number:
  - `0x50 | (seq & 0x0F)`: Single frame requesting immediate ACK (Bit 4 = `PacketAck`).
  - `0x40 | (seq & 0x0F)`: Standard frame without ACK requirement.
  - `0x60 | (seq & 0x0F)`: Multi-packet data chunk frame.
- **Byte 3 (`Msg Type`)**: Harmony / eQuad message type identifier (`RF_MSG_*`).
- **Byte 4 (`Flags`)**: Protocol control flags (e.g. `0x00` normal, `0x01` ACK_REQ, `0x02` IS_ACK).
- **Bytes 5..29**: 25-byte application payload.

### 3.2 Remote Device Framing (32-byte Report `0x20`)
In stock `hal_remote` (`libhal/rf/hot.c`, device mode `DAT_0003b530 == 1`), the CC2544 operates in device mode. All data frames over `/dev/rfspi` use **Report `0x20`**:

```
+--------+--------+--------+--------+----------+----------+--------+--------------------------+
| Byte 0 | Byte 1 | Byte 2 | Byte 3 |  Byte 4  |  Byte 5  | Byte 6 |       Bytes 7..31        |
|  0x20  |  0x21  |  0x12  |  0x01  | Hdr/Seq  | Msg Type | Flags  |     Payload (25 bytes)   |
+--------+--------+--------+--------+----------+----------+--------+--------------------------+
```
- **Byte 0 (`0x20`)**: Vendor Report ID.
- **Byte 1 (`0x21`)**: `(Pipe & 0x0F) | PowerMode`:
  - `PowerMode = 0x20` (Active mode).
  - `Pipe = 0x01` (Hub downlink pipe).
- **Byte 2 (`0x12`)**: Sub-ID discriminator.
- **Byte 3 (`0x01`)**: Pipe/Slot routing identifier.
- **Byte 4 (`Hdr/Seq`)**: `0x50 | (seq & 0x0F)` (single frame + ACK requested) or `0x40 | (seq & 0x0F)`.
- **Byte 5 (`Msg Type`)**: Harmony / eQuad message type.
- **Byte 6 (`Flags`)**: Protocol control flags.
- **Bytes 7..31**: 25-byte application payload.

### 3.3 Active Dongle Sync Pulse (10 bytes)
On Remote startup or display wake, the CC2544 radio's frequency-hopping spread-spectrum (FHSS) engine must synchronize with the Hub beacon:
```
12 01 00 00 00 00 00 00 00 00
```
- Writing this 10-byte pulse to `/dev/rfspi` triggers `libhal_getactivedongle` on CC2544, aligning the channel-hopping sequence.

---

## 4. CC2544 Control & Pairing Commands

### 4.1 Subsystem Initialization
- **Init Command** (`10 FF 80 00 00 01 00`): Wakes and initializes CC2544 messaging engine.
- **Extended Test Query** (`10 FF 80 FD 01 00 00`): Queries device role and assigned slot. Reply: `10 FF 80 FD 01 <slot> 00`.
- **Firmware Version Query** (`10 FF 81 F1 01 00 00`): Returns firmware version. Reply: `10 FF 81 F1 01 <ver> 00` (e.g. `0x27` = v39.00).

### 4.2 Power Mode Management
- **Set Active Power Mode** (`20 20 00 00 00 00 00`):
  - In `libhal/rf/hot.c` (`hot_set_powermode(0x20)`), switches CC2544 receiver into continuous listening mode.
  - Used on Hub initialization and Remote wake.

### 4.3 Pipe & Paired Device Table
Query command: `10 FF 83 B5 <pipe> 00 00`:
- **Hub Responses (`11 FF 83 B5 <pipe> ...`)**:
  - `pipe 0x03`: Hub's own 4-byte RF address (`11 FF 83 B5 03 <addr:4> ...`).
  - `pipe 0x20..0x25`: Slot identification / EquadID (`11 FF 83 B5 20 <EquadID:2> ...`, `0x4021` = Juniper remote).
  - `pipe 0x30..0x35`: Remote 4-byte RF address for slot (`11 FF 83 B5 30 <addr:4> ...`).
  - `pipe 0x40..0x45`: Remote name string for slot (`11 FF 83 B5 40 <len> <name...>`).
- **Remote Responses**:
  - `pipe 0x03`: Remote's own 4-byte RF address.
  - `pipe 0x20`: Paired Hub 4-byte RF address (located at bytes 12..15 of response).

### 4.4 Pairing State Machine
- **Open Pairing Lock (Hub)**: `10 FF 80 B2 01 00 <duration>` (e.g. `0x1E` = 30 seconds). Opens pairing window on Hub.
- **Request Pairing (Remote)**: `10 FF 80 B2 01 01 00`. Initiates bidirectional handshake from Remote.
- **Close Pairing Lock (Hub)**: `10 FF 80 B2 02 00 00`. Closes pairing window.
- **Unpair Slot**: `10 FF 80 B2 03 <slot> 00` (`0x01` = Slot 1, `0x02` = Slot 2, `0x00` = All).

### 4.5 Hardware Link Events & Notifications
- **Link Status Event** (`10 <slot> 41 04 <status> <rssi> ...`):
  - `status & 0x40 != 0`: Link **LOST** (`0x47`, `0x40`).
  - `status & 0x40 == 0`: Link **ESTABLISHED** (`0x07`, `0x0B`).
- **Hardware TX Status Report** (`20 <slot> 42 <status>`):
  - `status == 0x00`: Packet delivered and ACKed over RF.
  - `status != 0x00`: TX failure / drop.

---

## 5. Application Layer Protocol (Harmony / eQuad)

All application messages are encapsulated in the payload section of Report `0x12` (Hub) or Report `0x20` (Remote).

### 5.1 Message Type Definitions
| Type Code | Constant | Direction | Description |
| :---: | :--- | :---: | :--- |
| `0x0A` | `RF_MSG_ACTIVITY_SYNC` | Hub -> Remote | Current active activity state notification |
| `0x0B` | `RF_MSG_ACTIVITY_LIST_CHUNK` | Hub -> Remote | Activity list sync item chunk (ACK'd) |
| `0x0C` | `RF_MSG_DEVICE_LIST_CHUNK` | Hub -> Remote | Device list sync item chunk (ACK'd) |
| `0x0D` | `RF_MSG_BUTTON_LIST_CHUNK` | Hub -> Remote | Screen button list sync chunk (ACK'd) |
| `0x0E` | `RF_MSG_SETTINGS_PUSH` | Hub -> Remote | Remote display, timeout, and sensor settings push (ACK'd) |
| `0x0F` | `RF_MSG_CONFIG_END` | Hub -> Remote | End config sync transaction (ACK'd) |
| `0x10` | `RF_MSG_TIME_SYNC` | Hub -> Remote | Epoch time, timezone, and Hub RF address sync |
| `0x11` | `RF_MSG_STEP_LIST_CHUNK` | Hub -> Remote | Activity macro step list chunk (ACK'd) |
| `0x12` | `RF_MSG_DEVICE_CMD_CHUNK` | Hub -> Remote | Device command name for device browsing (ACK'd) |
| `0x13` | `RF_MSG_DEVICE_CMD_INDEX` | Remote -> Hub | Send a browsed device command by index |
| `0x14` | `RF_MSG_BUTTON_PRESS` | Remote -> Hub | Physical button press/release event |
| `0x15` | `RF_MSG_ACTIVITY_REQUEST` | Remote -> Hub | Request to switch active activity |
| `0x16` | `RF_MSG_DEVICE_CMD_REQUEST` | Remote -> Hub | Request direct device command execution |
| `0x17` | `RF_MSG_SYNC_REQUEST` | Remote -> Hub | Request quick state sync (`0x01`) or full config (`0x02`) |
| `0x18` | `RF_MSG_BATTERY_STATUS` | Remote -> Hub | Remote battery percentage, voltage (mV), and charging flag |
| `0x19` | `RF_MSG_REMOTE_CONFIG_BACKUP` | Remote -> Hub | Remote settings backup push to Hub |
| `0x1A` | `RF_MSG_VERSION_REPORT` | Remote -> Hub | Remote software and config version report |
| `0x1E` | `RF_MSG_ACK` | Both | Application-level acknowledgment (by SEQ) |
| `0x1F` | `RF_MSG_PING` | Both | Keepalive / ping query |
| `0x22` | `RF_MSG_OTA_START` | Hub -> Remote | Start OTA binary update (size, chunks) |
| `0x23` | `RF_MSG_OTA_CHUNK` | Hub -> Remote | 24-byte binary payload chunk |
| `0x24` | `RF_MSG_OTA_BLOCK_ACK` | Remote -> Hub | OTA block completion ACK |
| `0x25` | `RF_MSG_OTA_END` | Hub -> Remote | OTA transfer complete and checksum verify |

### 5.2 Key Payload Structures

#### `RF_MSG_SETTINGS_PUSH` (`0x0E`) & `RF_MSG_REMOTE_CONFIG_BACKUP` (`0x19`)
Both frames use a 17-byte standardized configuration payload:
- `payload[0]`: Display Brightness (1–5)
- `payload[1]`: Screen Timeout low byte (seconds)
- `payload[2]`: Touchscreen Enabled (`0x01` on, `0x00` off)
- `payload[3..4]`: Nav Toggle Key Linux code (16-bit unsigned little-endian, e.g. `139` = KEY_MENU)
- `payload[5]`: Nav Toggle Mode (`0x00` Short Press, `0x01` Long Press)
- `payload[6]`: USB Ethernet Enabled (`0x01` on, `0x00` off)
- `payload[7]`: Activity Switch Confirmation (`0x01` Prompt, `0x00` Instant)
- `payload[8]`: Config Version counter
- `payload[9]`: Touch Sensitivity (`1` Low, `2` Normal, `3` High)
- `payload[10]`: Remote Mode (`0` Activity, `1` Direct Device)
- `payload[11]`: Gyro/Tilt Wake Enabled (`0x01` on, `0x00` off)
- `payload[12]`: Gyro Sample Rate in Hz (`10`, `25`, `50`)
- `payload[13]`: Button Backlight Mode (`0` Off, `1` On, `2` Night Schedule)
- `payload[14]`: Night Schedule Start Hour (`0` to `23`)
- `payload[15]`: Night Schedule End Hour (`0` to `23`)
- `payload[16]`: Screen Timeout high byte (seconds)
- `payload[17]`: No-Screen Mode (`0x01` on, `0x00` off)
- `payload[18]`: Capacitive Buttons (`1` Enabled, `2` Disabled in push; `1`/`0` in backup)
- `payload[19]`: Power Debug Log (`1` Enabled, `2` Disabled in push; byte 18 `1`/`0` in backup)

#### `RF_MSG_SYNC_REQUEST` (`0x17`)
- Remote sends every 800ms while screen is active:
  - `payload[0]`: `0x01` for quick state sync (Activity + Time), `0x02` for full config sync.

#### `RF_MSG_ACTIVITY_SYNC` (`0x0A`)
- Hub broadcasts upon activity change or sync query:
  - `payload[0..11]`: Activity ID string (null-terminated, e.g. `"12345678"` or `"-1"` for PowerOff).
  - `payload[12..24]`: Activity friendly name string (null-terminated, e.g. `"Watch TV"`).

#### `RF_MSG_TIME_SYNC` (`0x10`)
- Hub transmits time and timezone configuration:
  - `payload[0..3]`: Epoch timestamp (32-bit unsigned little-endian).
  - `payload[4]`: Timezone string length.
  - `payload[5..19]`: Timezone POSIX string (e.g. `"CET-1CEST,M3.5.0"`).
  - `payload[20]`: Config version number.
  - `payload[21..24]`: Hub's 4-byte RF address.

#### `RF_MSG_ACTIVITY_LIST_CHUNK` (`0x0B`)
- `payload[0]`: Activity index; `payload[1]`: activity count.
- `payload[2]`: Icon type, `1`..`17` = `watchtv`, `movie`, `music`, `game`, `ps`, `xbox`, `switch`, `shield`, `apple`, `roku`, `firetv`, `pc`, `smarttv`, `sonos`, `receiver`, `light`, `power` (`rf_activity_icon_keyword()` in `rf_proto.h`). The remote draws its own icon for each keyword.
- `payload[3..]`: Activity ID and name.

#### `RF_MSG_DEVICE_LIST_CHUNK` (`0x0C`)
- Hub transmits list of Hub devices during config push (ACK'd):
  - `payload[0]`: Device 1-based index.
  - `payload[1]`: Total device count.
  - `payload[2..5]`: Shortened device ID string (null-terminated, up to 4 bytes).
  - `payload[6..25]`: Friendly device name string (null-terminated, up to 20 bytes).

#### `RF_MSG_BUTTON_PRESS` (`0x14`)
- Remote transmits on key events:
  - `payload[0..1]`: 16-bit Linux input event key code (big-endian).
  - `payload[2]`: State (`0x01` press, `0x02` hold, `0x00` release).
  - `payload[3..18]`: Short button name string (up to 16 chars).
  - `payload[20..24]`: Device context ID string (up to 4 chars + NUL) when the remote is browsing a device in the Hub Device Browser view (`VIEW_DEVICE_BROWSER`). Hub routes physical buttons to this device or falls back to matching command names. When not browsing a device, bytes 20..24 are zeroed.

#### `RF_MSG_DEVICE_CMD_CHUNK` (`0x12`)
- Hub sends one per command during a config push, after the `RF_MSG_DEVICE_LIST_CHUNK` frames:
  - `payload[0]`: Device index (position in the pushed device list).
  - `payload[1]`: Command index (position in the device's `commands` array in `devices.json`).
  - `payload[2]`: Number of commands for the device.
  - `payload[3..24]`: Command name (up to 22 chars, NUL-padded).
- A remote without device browsing does not ACK this type; the Hub stops sending device commands to that slot when the first one gets no ACK.

#### `RF_MSG_DEVICE_CMD_INDEX` (`0x13`)
- Remote transmits when a command is chosen in the device browser:
  - `payload[0..3]`: Shortened Device ID (4 bytes, null-terminated).
  - `payload[4]`: Command index.
  - `payload[5..24]`: First 20 chars of the command name prefix.
- The Hub sends the command at that index if its name matches the prefix, otherwise the first command whose name matches the prefix.

#### `RF_MSG_DEVICE_CMD_REQUEST` (`0x16`)
- Remote transmits for direct device command execution:
  - `payload[0..3]`: Shortened Device ID (4 bytes, null-terminated).
  - `payload[4..24]`: Command name (up to 21 chars, null-terminated).

#### `RF_MSG_BATTERY_STATUS` (`0x18`)
- Remote transmits battery status:
  - `payload[0]`: Battery percentage (0–100%).
  - `payload[1]`: Charging flag (`0x01` charging, `0x00` discharging).
  - `payload[2..3]`: Voltage in millivolts (16-bit unsigned little-endian).

---

## 6. Reverse Engineering Reference Mapping

Source traces from disassembly of stock binaries (`hal_hub` and `hal_remote`):

| Stock Function (`libhal`) | Binary / Address | Protocol Role |
| :--- | :--- | :--- |
| `libhal_rf_init` | `hal_remote: 0x00019a70` / `hal_hub: main` | Initializes RF subsystem; sets role `is_device = 1` on Remote, `0` on Hub |
| `hot_init` | `hal_remote: 0x0001f5e8` | Sets role global `DAT_0003b530`, allocates buffers, spawns `hot_thread` |
| `hot_set_powermode` | `hal_remote: 0x0001ef60` | Sends `20 <mode> 00 00 00 00 00`; configures receiver power mode |
| `hot_send_message_and_data` | `hal_remote: 0x0001d840` / `hal_hub: 0x0042c3c8` | Formats Report `0x12` (Hub) vs Report `0x20` (Remote) |
| `hot_thread` | `hal_remote: 0x0001e28c` | Registers receive filters: `0x20` on Remote, `0x12` on Hub |
| `hot_recv_parse_packet` | `hal_remote: 0x0001df14` | Parses received 32-byte (Remote) or 30-byte (Hub) data packets |
| `libhal_getactivedongle` | `hal_remote: 0x00018eb0` | Sends 10-byte pulse `12 01 00 ...` to synchronize hopping channel |
| `libhal_rf_device_pair_write` | `hal_remote: 0x00019e50` | Sends pairing start command `10 FF 80 B2 01 01 00` |
| `libhal_get_paired_device_info`| `hal_remote: 0x000193d4` | Queries pipes `0x03`, `0x20..0x40` via `10 FF 83 B5` |
