# Logitech Harmony Hub & Physical Remotes RF Protocol Specification

## 1. Overview & Architecture

Logitech Harmony Hub communicates locally with paired physical remotes, including the touchscreen remotes (**Harmony Elite 950**, **Harmony Ultimate**, **Harmony Pro 2400**), **Harmony Companion**, and **Harmony Smart Control**, using a proprietary 2.4 GHz RF protocol implemented on **Texas Instruments CC2544** 2.4 GHz RF SoCs. The host processors communicate with the CC2544 over synchronous SPI via the `/dev/rfspi` character device.

```
+------------------------------------+              2.4 GHz eQuad RF Link             +------------------------------------+
|       Harmony Hub (Host Mode)      |                                                |   Harmony Remotes (Device Mode)    |
|                                    |                                                |                                    |
| Atheros AR9344 SoC (MIPS 24Kc)    |                                                | • Touchscreen: DaVinci DM365 ARM   |
|   |                                |                                                | • Companion/Smart: CC2544/MCU      |
|   | /dev/rfspi (SPI Master)        |                                                |   |                                |
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
  - `0x50 | (seq & 0x0F)`: every frame `rf_build_frame()` builds (Hub and remote), whether or not it asks for an ACK.
  - `0x40 | (seq & 0x0F)`: `RF_MSG_ACK` frames.
  - `0x60 | ...`: the windowed RF OTA data frames (section 5.2, "RF OTA transfer").
  - Whether a frame wants an ACK is decided by the flags byte (`RF_FLAG_ACK_REQ`), not by this byte.
- **Byte 3 (`Msg Type`)**: Harmony / eQuad message type identifier (`RF_MSG_*`).
- **Byte 4 (`Flags`)**: Protocol control flags (e.g. `0x00` normal, `0x01` ACK_REQ, `0x02` IS_ACK).
- **Bytes 5..29**: 25-byte application payload.

### 3.2 Touchscreen Remote Device Framing (32-byte Report `0x20`, Sub-ID `0x12`)
In stock `hal_remote` (`libhal/rf/hot.c`, device mode `DAT_0003b530 == 1`), the CC2544 on screened remotes operates in device mode. All application data frames over `/dev/rfspi` use **Report `0x20`** with Sub-ID `0x12`:

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
- **Byte 2 (`0x12`)**: Sub-ID discriminator for Harmony application messaging.
- **Byte 3 (`0x01`)**: Pipe/Slot routing identifier.
- **Byte 4 (`Hdr/Seq`)**: `0x50 | (seq & 0x0F)` on every frame `rf_build_frame()` builds, `0x40 | (seq & 0x0F)` on `RF_MSG_ACK` frames (as in section 3.1; the ACK request is bit 0 of the flags byte).
- **Byte 5 (`Msg Type`)**: Harmony / eQuad message type.
- **Byte 6 (`Flags`)**: Protocol control flags.
- **Bytes 7..31**: 25-byte application payload.

### 3.3 Companion & Smart Control Screenless Remote Framing (Report `0x20`, Sub-ID `0x01` & `0x03`)
Screenless remotes (Harmony Companion and Smart Control) communicate directly via eQuad HID uplink frames over Report `0x20` rather than the bidirectional application protocol:

#### 1. HID Keyboard / Keypad Reports (Sub-ID `0x01`)
```
+--------+---------------+--------+-----------+----------+-----------+-----+
| Byte 0 |    Byte 1     | Byte 2 |  Byte 3   |  Byte 4  |  Byte 5   | ... |
|  0x20  | 0x20 | PipeId |  0x01  | Modifiers | Reserved | Keycode 1 | ... |
+--------+---------------+--------+-----------+----------+-----------+-----+
```
- **Byte 0 (`0x20`)**: Vendor Report ID.
- **Byte 1**: `0x20 | (pipe & 0x0F)` (`0x21` for Slot 1, `0x22` for Slot 2).
- **Byte 2 (`0x01`)**: Sub-ID discriminator for standard HID Keyboard reports.
- **Byte 3**: Modifier bitmask (Ctrl, Shift, Alt, GUI).
- **Byte 4**: Reserved (0x00).
- **Byte 5..10**: Up to 6 simultaneous HID keycodes (e.g. `0x1E` KEY_1..`0x27` KEY_0, `0x28` KEY_ENTER, `0x76` KEY_MENU). All zeros indicate button release.

#### 2. HID Consumer Control Reports (Sub-ID `0x03`)
```
+--------+---------------+--------+------------+------------+
| Byte 0 |    Byte 1     | Byte 2 |   Byte 3   |   Byte 4   |
|  0x20  | 0x20 | PipeId |  0x03  | Usage (LE) | Usage (LE) |
+--------+---------------+--------+------------+------------+
```
- **Byte 0 (`0x20`)**: Vendor Report ID.
- **Byte 1**: `0x20 | (pipe & 0x0F)` (`0x21` for Slot 1, `0x22` for Slot 2).
- **Byte 2 (`0x03`)**: Sub-ID discriminator for HID Consumer Control reports.
- **Bytes 3..4**: 16-bit little-endian HID consumer usage code (e.g. `0x00E9` Volume Up, `0x00EA` Volume Down, `0x00E2` Mute, `0x00B5` Scan Next, `0x00B6` Scan Prev, `0x00CD` Play/Pause). `0x0000` indicates button release.

### 3.4 Active Dongle Sync Pulse (10 bytes)
On a display wake, a key wake, after a resume and every 2 s while the link is lost with the screen on, the CC2544 radio's frequency-hopping spread-spectrum (FHSS) engine must synchronize with the Hub beacon:
```
12 01 00 00 00 00 00 00 00 00
```
- Writing this 10-byte pulse to `/dev/rfspi` triggers `libhal_getactivedongle` on CC2544, aligning the channel-hopping sequence. `remote_ui` sends a quick sync query (`RF_MSG_SYNC_REQUEST`) 5 ms after it, and skips the pulse during a config sync or an RF OTA.
- At start `remote_ui` writes two other 10-byte packets instead, `12 00 01 00 00 00 00 00 00 00` and `12 01 01 00 00 00 00 00 00 00`.
- The Hub writes the `12 01 00 ...` pulse too, during a stop-and-wait RF OTA, after the 3rd, 7th and 11th failed try of a chunk.

---

## 4. CC2544 Control & Pairing Commands

### 4.1 Subsystem Initialization
- **Init Command** (`10 FF 80 00 00 01 00`): Wakes and initializes CC2544 messaging engine.
- **Factory RF test step** (`10 FF 80 FD 01 00 00`): steps the CC2544 to its next factory test mode; reply `10 FF 80 FD 01 <mode> 00`. Logitech's firmware sends it only from the hidden factory test menu (`rf.test`), which names the modes: `0` RX sweep, `1`-`3` TX unmodulated at 2405/2444/2474 MHz, `4`-`6` TX modulated at the same channels, `7`-`9` RX at those channels, `10` TX sweep unmodulated, `11` ready. A reboot clears it. Not for normal operation: on the remote, from the fifth write since boot the link stays lost (`10 01 41 04 47`). Only `01` is accepted (`00` is rejected with `10 FF 8F 80 FD 03 00`). `remote_ui` does not send it.
- **Firmware Version Query** (`10 FF 81 F1 01 00 00`): Returns firmware version. Reply: `10 FF 81 F1 01 <ver> 00` (e.g. `0x27` = v39.00).

### 4.2 Power Mode Management
- **Set Power Mode** (`20 <mode> 00 00 00 00 00`, `hot_set_powermode()` in `libhal/rf/hot.c`, same on Hub and remote):
  - `0x20` (`RF_PM_ACTIVE`): normal operation. Stock sets it when its HOT state machine enters Idle; harmony-ng at start.
  - `0x30` (`RF_PM_TRANSFER`): for the whole of a transfer. Stock sets it when the state machine leaves Idle (`hot_Idle.exit`) and goes back to `0x20` 500 ms after the last object (`hot_CoolDown`). In this mode the remote's CC2544 fetches the Hub's queued frames by itself, about one every 4 ms on a Harmony Ultimate; in `0x20` it gets one only in the acknowledgement of a frame it transmits. harmony-ng uses it for the whole of every RF OTA, stop-and-wait or windowed: the Hub sets it on its own CC2544 before `RF_MSG_OTA_START`, the remote on its own radio when it accepts `RF_MSG_OTA_START`, and each goes back to `0x20` when the update ends or fails (measured: 250 frames/s with no remote transmission, against 7/s in `0x20`).
  - `0x40`: not used by stock; on a Harmony Ultimate the remote stops answering until `0x20` is set again.
  - A remote in device mode puts the current mode in byte 1 of every frame it sends (`mode | pipe`, section 3.2).

### 4.3 Pipe & Paired Device Table
Query command: `10 FF 83 B5 <pipe> 00 00`:
- **Hub Responses (`11 FF 83 B5 <pipe> ...`)**:
  - `pipe 0x03`: Hub's own 4-byte RF address (`11 FF 83 B5 03 <addr:4> ...`).
  - `pipe 0x20..0x25`: Slot identification / EquadID (`11 FF 83 B5 2n <2 bytes> <EquadID:2> ...`: `hub_rf` reads it from bytes 7..8 of the reply, high byte first). The EquadID names the hardware family, not the product. The list the stock Hub firmware knows (`pimento_rfthread`):
    - `0x4021` "Juniper": every touchscreen remote (Harmony Ultimate, Ultimate One, Ultimate Home, Elite, 950). The product is the remote's skin id (the `skin` record of its `mfg` partition), which a harmony-ng remote reports in `RF_MSG_UNIT_ID`. The stock Hub logs "Paired Remote is not Juniper" when no slot has `0x4021`; the stock button maps use the same number as a prefix (`16417Activity<id>`, 16417 = `0x4021`).
    - `0x401E` "Sprite": Harmony Smart Control (stock firmware, no skin id).
    - `0x4026`, `0x4027` "Sprite": Harmony Companion (stock firmware, no skin id).
    - `0x4024`: handled by a thread of its own in the stock Hub; the product is not identified.
    - `0x880E`: the Logitech USB dongle.
    - Any other id: an unpaired pipe.

  Skin ids of the Juniper products, as the stock remote UI names them (`harmony.lua`, `MODEL_NAME`); any other skin is "Harmony Touch" there. `0x63` appears once in the stock UI, in a provisioning check, without a name. The stock UI picks its "hops" layout for `0x6F` and `0x70` and "juniper" for the others.

  | Skin | Product | `model` in the RF status |
  | --- | --- | --- |
  | `0x64` (100) | Harmony Ultimate | `ultimate` |
  | `0x66` (102) | Harmony Ultimate One | `ultimate_one` |
  | `0x69` (105), `0x6C` (108) | Harmony Ultimate Home | `ultimate_home` |
  | `0x6F` (111) | Harmony Elite | `elite` |
  | `0x70` (112) | Harmony 950 | `950` |
  | other | Harmony Touch | `touch` |
  - `pipe 0x30..0x35`: Remote 4-byte RF address for slot (`11 FF 83 B5 30 <addr:4> ...`).
  - `pipe 0x40..0x45`: Remote name string for slot (`11 FF 83 B5 40 <len> <name...>`).
- **Remote Responses**:
  - `pipe 0x03`: Remote's own 4-byte RF address.
  - `pipe 0x20`: Paired Hub 4-byte RF address (located at bytes 12..15 of response).

### 4.4 Pairing State Machine
- **Open Pairing Lock (Hub)**: `10 FF 80 B2 01 00 <duration>` (e.g. `0x1E` = 30 seconds). Opens pairing window on Hub.
  The command has no slot: the CC2544 picks the pipe of the new remote. For a pairing into a
  chosen slot (`POST /api/rf/pair?slot=N`) `hub_rf` maps that pipe to the slot afterwards
  (`rf_slot_map_claim_pipe()`, table in `/data/harmony/rf_slot_map.txt`): the pipe that holds an
  EquadID in the pipe-table reply after the pairing and held none when the window opened. The
  order of the radio events does not matter (a frame of the new remote can come before the
  `4A`); a lock that closes without a pairing, a cancel or a timeout places nothing. Byte 1 of every frame,
  link status, NAK and pipe-table reply names the pipe; `rf_spi.c` turns it into the slot and back.
- **Request Pairing (Remote)**: `10 FF 80 B2 01 01 00`. Initiates bidirectional handshake from Remote.
- **Close Pairing Lock (Hub)**: `10 FF 80 B2 02 00 00`. Closes pairing window.
- **Unpair Slot**: `10 FF 80 B2 03 <pipe> 00` (`0x01` to `0x06`). The CC2544 has six pipes
  (pipe table `0x20..0x25`). `hub_rf` sends the pipe of the slot, and for "every remote" one
  command per pipe, as the stock `libhal_rf_command_unpair` does (it never sends index `0x00`
  either). An `RF_MSG_UNPAIR_NOTIFY` goes first, only to slots that hold a touchscreen remote (a Companion
  or Smart Control does not handle it).
- **Clear Bonding (Remote)**: when it is unpaired, `remote_ui` writes `10 FF 80 D0 01 00 00`, then the 20-byte reports `11 FF 82 B5 20 00 ...` and `11 FF 82 B5 30 00 ...` (zeroed pipe entries), then `10 FF 80 D0 00 00 00`, 50 ms apart (`cc2544_clear_bonding()`).

### 4.5 Hardware Link Events & Notifications
- **Link Status Event** (`10 <slot> 41 04 <status> <wpid_lo> <wpid_hi>`):
  - `status & 0x40 != 0`: Link **LOST** (`0x47`, `0x40`).
  - `status & 0x40 == 0`: Link **ESTABLISHED** (Remote: `0x07`, `0x0B`; Hub: `0x87`).
  - `<wpid_lo> <wpid_hi>`: wireless product ID of the peer, little-endian, as in the HID++ 1.0 device connection notification. The Remote reports `0B 88` (the Hub, `0x880B`); the Hub reports `21 40` (the touchscreen remote, `0x4021`). The bytes stay the same whether the link is lost or established, so they carry no signal level.
- **Hardware TX Status Report** (`20 <slot> 42 <status>`):
  - `status == 0x00`: The remote's radio received the packet. The remote application can still drop it, so this does not count as delivery; only `RF_MSG_ACK` does.
  - `status != 0x00`: TX failure / drop (`0x01` while the link is lost).
- **Pairing Notification** (`10 <slot> 4A <code> <status> ...`):
  - Hub: `code == 0x00` and `status == 0x00` means a remote was paired (`hub_rf` then reads the pipe table and closes the pairing lock); `code >= 0x01` with `status == 0x00` means the pairing lock is open.
  - Remote: `status == 0x00` completes pairing on the slot in byte 1; the remote then asks for a full config. `status == 0x01` means the attempt did not complete (the stock Hub `hal` logs a non-zero status as an error code); the remote does not stop there and asks again. Any other status ends the pairing attempt as failed.
- **Error / NAK** (`10 <slot> 8F <sub-id of the refused send> .. <err> ..`, error code in byte 5): the CC2544 refused a write. `hub_rf` counts them per slot (`naks` in `slots[].link` of `GET /api/rf/status`).

### 4.6 Readable Registers (no RSSI)
A sweep of every HID++ 1.0 short (`10 FF 81 <reg> 00 00 00`) and long (`10 FF 83 <reg> 00 00 00`) register read returned error `10 FF 8F <sub> <reg> 02 00` (invalid register) except for the registers below. `0xF1` returns error `03` without its parameter (see §4.1). Neither CC2544 exposes RSSI or link quality.

| Register | Hub | Remote |
| :--- | :--- | :--- |
| `81 00` | `00 01 00`: notification flags, set by the Init Command | error `02` |
| `81 01`, `81 03` | `00 00 00` | error `02` |
| `81 02` | `00 01 00` (on Unifying receivers, the number of connected devices) | error `02` |
| `81 D0` | `00 00 00` | `00 00 00` before `remote_ui` starts, `01 00 00` after |
| `81 D2` | error `02` | `00 00 0D`, constant while the link alternates between lost and established |
| `81 E6`, `81 F0` | `00 00 00` | error `02` |
| `83 B3` | Device activity: byte 4 + n is an 8-bit counter of payload frames received from slot n + 1 | error `02` |
| `83 B5 <pipe>` | §4.3 | §4.3; pipe `0x02` also answers |
| `83 FD` | error `02` | all zero |

On the Hub, `83 B3` advanced by exactly the number of `SYNC_REQUEST` frames that `hub_rf` received from slot 1, so it counts uplink frames, not attempts.

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
| `0x0F` | `RF_MSG_CONFIG_END` | Hub -> Remote | Open (clear) or close (complete) a config push (ACK'd) |
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
| `0x1B` | `RF_MSG_UNPAIR_NOTIFY` | Both | The sender is about to drop the pairing. No payload. The Hub ACKs the remote's notice and then removes the slot; the remote clears its bonding and its stored config without an ACK |
| `0x1C` | `RF_MSG_CONFIG_PULL_REQ` | Hub -> Remote | Ask the remote to send its settings (`RF_MSG_REMOTE_CONFIG_BACKUP`); `/api/rf/pull-config` |
| `0x1D` | `RF_MSG_SLEEP_REQ` | Hub -> Remote | Suspend now (on battery); `/api/rf/sleep`. The remote ACKs it, then blanks the screen and suspends, unless a config sync, an update, a pairing or an activity switch is running |
| `0x1E` | `RF_MSG_ACK` | Both | Application-level acknowledgment (by SEQ) |
| `0x1F` | `RF_MSG_PING` | Both | Keepalive; both ends handle it, current firmware sends it from neither |
| `0x22` | `RF_MSG_OTA_START` | Hub -> Remote | Start OTA binary update (size, chunks) |
| `0x23` | `RF_MSG_OTA_CHUNK` | Hub -> Remote | Stop-and-wait OTA chunk: index, length, up to 22 data bytes (`RF_OTA_CHUNK_BYTES`) |
| `0x24` | `RF_MSG_OTA_BLOCK_ACK` | Remote -> Hub | Defined, not used: OTA chunks are acknowledged with `RF_MSG_ACK` |
| `0x25` | `RF_MSG_OTA_END` | Hub -> Remote | OTA transfer complete and checksum verify |
| `0x26` | `RF_MSG_ICON_START` | Hub -> Remote | Custom icon transfer initiate: type (byte 0), size (1-2), chunks (3-4), id (5-24, zero-terminated); no checksum here. The id must be 1-19 characters of `A-Z a-z 0-9 _ -` (`rf_icon_id_valid()`); otherwise the remote answers status `0x05` and drops the transfer. Other ACK status values: `0x00` ready for the chunks, `0x03` cannot open its temporary file, `0x04` it already has this icon with the same size (the Hub then sends no chunks). Icons are at most 16384 bytes |
| `0x27` | `RF_MSG_ICON_CHUNK` | Hub -> Remote | Custom icon PNG data chunk: index (bytes 0-1), length (2), up to 22 data bytes (3-24). The Hub asks for an ACK on every 4th chunk and the last, 15 ms between chunks |
| `0x28` | `RF_MSG_ICON_END` | Hub -> Remote | Custom icon transfer complete: CRC16-CCITT of the PNG (bytes 0-1, little-endian) to verify; saved as `/data/assets/custom/{type}_{id}.png` and `/data/assets/custom/{id}.png` (type `activity`, `device` or `button` from byte 0 of `RF_MSG_ICON_START`: `0`, `1`, `2`; the Hub sends `0`). ACK status: `0x00` saved, `0x01` CRC or size wrong, `0x03` could not be saved |
| `0x29` | `RF_MSG_OTA_REQUEST` | Remote -> Hub | The remote asks the Hub to start an RF OTA (Settings, Firmware Update). The Hub's ACK status: `0x00` started, `0x01` could not start, `0x02` busy (an update is already running or being prepared) |
| `0x2A` | `RF_MSG_UPDATE_NOTIFY` | Hub -> Remote | A config or firmware update is waiting for the remote: flags (byte 0: bit 0 = the Hub's config version differs from the remote's, bit 1 = the Hub has a newer remote firmware), Hub config version (1), firmware version major, minor, patch (2-4). Sent after a version report and by `/api/rf/push-config`, only when a flag is set; the remote ACKs it |
| `0x2B` | `RF_MSG_ACT_DEVICES_CHUNK` | Hub -> Remote | Devices one activity uses (ACK'd); sent only to remotes that report `RF_CAP_ACT_DEVICES` |
| `0x2C` | `RF_MSG_NAME_TAIL_CHUNK` | Hub -> Remote | Rest of an activity or device name (ACK'd); sent only to remotes that report `RF_CAP_LONG_NAMES` |
| `0x2D` | `RF_MSG_HELP_REQUEST` | Remote -> Hub | Help: repair the running activity |
| `0x2E` | `RF_MSG_HELP_STATUS` | Hub -> Remote | Help progress, or a device to ask about (ACK'd except a repeated "started"); sent only in reply to `RF_MSG_HELP_REQUEST` |
| `0x2F` | `RF_MSG_HELP_ANSWER` | Remote -> Hub | Help: whether a device is on (ACK'd) |
| `0x30` | `RF_MSG_WAKE` | Hub -> Remote | Turn the screen on (ACK'd); a remote in light standby still receives frames. Remotes without it drop it as unknown and send no ACK |
| `0x31` | `RF_MSG_UNIT_ID` | Remote -> Hub | The remote's factory unit id and skin id (ACK'd); sent right after every `RF_MSG_VERSION_REPORT` by remotes that report `RF_CAP_UNIT_ID`. A Hub without it drops the frame as unknown |

The remote sets no ACK request flag on `RF_MSG_BATTERY_STATUS`, `RF_MSG_REMOTE_CONFIG_BACKUP`, `RF_MSG_VERSION_REPORT`, `RF_MSG_UNIT_ID` and `RF_MSG_OTA_REQUEST`; the Hub answers each of them with `RF_MSG_ACK` all the same (not the first two while an RF OTA runs).

### 5.2 Key Payload Structures

#### `RF_MSG_VERSION_REPORT` (`0x1A`)
- `payload[0..2]`: Software version major, minor, patch
- `payload[3]`: Config version the remote holds
- `payload[4]`: Capabilities, a bit mask of `RF_CAP_*`. Older remotes send `0x00`.
  - `0x01` `RF_CAP_ACT_DEVICES`: understands `RF_MSG_ACT_DEVICES_CHUNK`
  - `0x02` `RF_CAP_LONG_NAMES`: understands `RF_MSG_NAME_TAIL_CHUNK`
  - `0x04` `RF_CAP_UNIT_ID`: sends `RF_MSG_UNIT_ID` after every version report
  - `0x08` `RF_CAP_DEVICE_CMDS`: stores and ACKs `RF_MSG_DEVICE_CMD_CHUNK` (device browsing); for
    such a remote a device command frame without an ACK fails the config push like any other frame
  - `0x10` `RF_CAP_SWITCH_START`: understands the switch start notice of `RF_MSG_ACTIVITY_SYNC`
    (`payload[24]` `0x02`)
  - `0x20` `RF_CAP_HELP_KEY`: understands Help keys in `RF_MSG_BUTTON_LIST_CHUNK` (`payload[24]` `0x04`)
  - `0x40` `RF_CAP_STREAM_PUSH`: takes a streamed config push (see `RF_MSG_CONFIG_END`)

The remote sends this report when its link first comes up, every 24 hours, and before each full config request, so a Hub that restarted learns the capabilities before it builds the push. The Hub keeps them in `/data/harmony/remote_slot<N>_version.json` (`caps`). A remote drops frame types it doesn't know without an ACK, so the Hub must not send newer frame types to a remote that doesn't report them. The config push depends on the capabilities (Warmup steps, Help keys): when a known remote reports other capabilities with the current config version (an update, or `init.sh` falling back to `remote_ui.bak`), the Hub raises its config version, so the remote asks for a new push.

#### `RF_MSG_UNIT_ID` (`0x31`)
- `payload[0]`: Length `n` of the id, at most 24 (`RF_UNIT_ID_MAX`); 0 when the remote has no id
- `payload[1..n]`: The remote's factory unit id as ASCII, read from the `unitid` record of its
  `mfg` partition (21 characters on current hardware, for example `20141001120000-000002`)
- `payload[24]` (`RF_UNIT_ID_SKIN_OFF`): The remote's skin id, read from the `skin` record of the
  same partition (stored as text, `0x64` on a Harmony Ultimate); 0 when the record is missing.
  Read only when `n` < 24. Older remotes pad this byte with 0, older Hubs ignore it.

Sent right after every `RF_MSG_VERSION_REPORT` (link up, every 24 hours, before a full config
request) by remotes that report `RF_CAP_UNIT_ID`; a remote whose partition has neither record
sends nothing. The Hub ACKs it, keeps the id and the skin in memory and in
`/data/harmony/remote_slot<N>_version.json` (`remote_id`, `skin`, rewritten only when they
changed), reports them as `slots[].remote_id`, `slots[].skin` and the product in
`slots[].model` / `slots[].model_name` of `GET /api/rf/status`, and clears them when the slot is
unpaired.

#### `RF_MSG_ACT_DEVICES_CHUNK` (`0x2B`)
One frame per activity, after the device list in a config push:
- `payload[0]`: Activity index, as in `RF_MSG_ACTIVITY_LIST_CHUNK`
- `payload[1]`: Activity count
- `payload[2]`: Device count `n` (at most 22)
- `payload[3..3+n-1]`: Indexes into the device list of this push, from the activity's `deviceIds`

#### `RF_MSG_NAME_TAIL_CHUNK` (`0x2C`)
Follows an `RF_MSG_ACTIVITY_LIST_CHUNK` or `RF_MSG_DEVICE_LIST_CHUNK` whose name didn't fit, one frame per 22 bytes of the rest of the name. Names are up to 39 bytes (`RF_NAME_SIZE`); older remotes keep the part in the list chunk.
- `payload[0]`: `0` activity, `1` device
- `payload[1]`: Activity or device index
- `payload[2]`: Offset of this part in the name
- `payload[3..24]`: Name bytes, NUL-padded

#### Help: `RF_MSG_HELP_REQUEST` (`0x2D`), `RF_MSG_HELP_STATUS` (`0x2E`), `RF_MSG_HELP_ANSWER` (`0x2F`)
Help re-sends the running activity's commands except power toggles, then asks about each device whose only power command is a toggle. The Hub sends `RF_MSG_HELP_STATUS` only to a remote that asked, so no capability bit is needed.

1. The remote sends `RF_MSG_HELP_REQUEST` and repeats it every 2 s, up to 3 times, until it gets "started".
2. hub_rf calls `POST /api/activity/help` on the daemon and replies "started", or "busy" or "failed".
3. When the daemon has sent the commands (`GET /api/activity` reports `busy` and `sending` false, at most 120 s), hub_rf sends one question per toggle device, or "no questions".
4. For each question the remote sends `RF_MSG_HELP_ANSWER`; hub_rf calls `POST /api/activity/help/answer`, which sends the toggle when the answer is "off".

`RF_MSG_HELP_REQUEST`:
- `payload[0..11]`: Activity ID, NUL-padded

`RF_MSG_HELP_STATUS`:
- `payload[0]`: `0x00` question, `0x01` started (re-sending), `0x02` busy (another activity change is running), `0x03` failed (activity not running, or Hub error), `0x04` no questions (commands re-sent, no toggle devices)
- For a question:
  - `payload[1]`: Question index
  - `payload[2]`: Question count
  - `payload[3..10]`: Device ID, NUL-padded
  - `payload[11..24]`: First 14 bytes of the device name; the remote shows the full name from its device list when it has the ID

`RF_MSG_HELP_ANSWER`:
- `payload[0]`: `1` the device is on, `0` it is off
- `payload[1..8]`: Device ID, NUL-padded

#### `RF_MSG_SETTINGS_PUSH` (`0x0E`)
Hub -> remote, 24 bytes, ACK'd; built by `rf_build_settings_push()` (`rf_sync.c`) and read by
`remote_rf_sync.c`:
- `payload[0]`: Display Brightness (1–5)
- `payload[1]`: Touchscreen Enabled (`1` on, `0` off)
- `payload[2]`: Gyro/Tilt Wake Enabled (`1` on, `0` off)
- `payload[3..4]`: Screen Timeout in seconds, 16-bit big-endian (`payload[3]` high byte); the remote clamps it to 5..600
- `payload[5..6]`: Nav Toggle Key of older firmware (little-endian); the current remote ignores it
- `payload[7]`: Nav Toggle Mode of older firmware; ignored
- `payload[8]`: Activity Switch Confirmation (`1` Prompt, `0` Instant)
- `payload[9]`: Touch Sensitivity (`1` Low, `2` Normal, `3` High)
- `payload[10]`: Remote Mode, always `0` (activity mode; direct mode was removed)
- `payload[11]`: Gyro Sample Rate in Hz (`10`, `25`, `50`, `100`)
- `payload[12]`: Button Backlight Mode (`0` Off, `1` On, `2` Night)
- `payload[13]`: Night Start Hour (`0` to `23`)
- `payload[14]`: Night End Hour (`0` to `23`)
- `payload[15]`: USB Ethernet flag of older firmware; the current remote ignores it (USB Ethernet starts by itself on USB power)
- `payload[16]`: Config Version counter
- `payload[17]`: No-Screen Mode (`1` on, `0` off)
- `payload[18]`: Capacitive Buttons (`1` enabled, `2` disabled)
- `payload[19]`: Debug Logging (`1` on, `2` off)
- `payload[20]`: Language (`0` en, `1` de, `2` fr, `3` nl, `4` es, `5` it)
- `payload[21]`: Theme (`1` classic, `2` dark, `3` light, `4` colourful; `0` keeps the remote's choice)
- `payload[22]`: Home layout (`1` the theme's, `2` list, `3` grid, `4` carousel; `0` keeps the remote's choice)
- `payload[23]`: Physical Button Nav (`1` on, `0` off)

#### `RF_MSG_REMOTE_CONFIG_BACKUP` (`0x19`)
Remote -> Hub, 23 bytes, sent without the ACK request flag (the Hub ACKs it all the same, except
during an RF OTA); built by `send_remote_config_backup_rf()` (`remote_rf.c`) and read by
`save_remote_config_backup()` (`rf_sync.c`). The remote sends it after every saved settings change
while the link is up, also one applied from a settings push, and when the Hub asks with
`RF_MSG_CONFIG_PULL_REQ`. A different order from the push:
- `payload[0]`: Display Brightness (1–5)
- `payload[1]`: Screen Timeout low byte (seconds); `payload[16]` is the high byte
- `payload[2]`: Touchscreen Enabled (`1`/`0`)
- `payload[3..5]`: Nav toggle key and mode of older firmware, sent as `0`
- `payload[6]`: `1` while the remote is on USB power (USB Ethernet up), else `0`
- `payload[7]`: Activity Switch Confirmation (`1` Prompt, `0` Instant)
- `payload[8]`: Config Version counter
- `payload[9]`: Touch Sensitivity (`1` to `3`)
- `payload[10]`: Remote Mode, always `0`
- `payload[11]`: Gyro/Tilt Wake Enabled (`1`/`0`)
- `payload[12]`: Gyro Sample Rate in Hz
- `payload[13]`: Button Backlight Mode (`0` Off, `1` On, `2` Night)
- `payload[14]`: Night Start Hour; `payload[15]`: Night End Hour
- `payload[16]`: Screen Timeout high byte
- `payload[17]`: Capacitive Buttons (`1`/`0`)
- `payload[18]`: always `0` (the debug logging setting is not reported)
- `payload[19]`: Language (as in the push)
- `payload[20]`: Theme id (as in the push; `0` for a theme with no id)
- `payload[21]`: Home layout (as in the push)
- `payload[22]`: Physical Button Nav (`1`/`0`)

#### `RF_MSG_SYNC_REQUEST` (`0x17`)
- Remote sends every 800ms while screen is active:
  - `payload[0]`: `0x01` for quick state sync (Activity + Time), `0x02` for full config sync.

#### `RF_MSG_ACTIVITY_SYNC` (`0x0A`)
- Hub broadcasts upon activity change or sync query:
  - ID of 1 or 2 characters, or `"-1"` for PowerOff: `payload[0..2]` ID, `payload[3..23]` name (21 bytes).
  - Longer ID (e.g. `"12345678"`): `payload[0..9]` ID (10 bytes), `payload[10..23]` name (14 bytes).
  - `payload[24]`: `0x00` `RF_ACT_SYNC_COMPLETE` = the activity runs (a switch ended), `0x01` `RF_ACT_SYNC_QUERY` = answer to a `RF_MSG_SYNC_REQUEST` (catch-up, no switch overlay), `0x02` `RF_ACT_SYNC_STARTING` = the Hub started a switch to this activity, `0x03` `RF_ACT_SYNC_QUERY_SWITCHING` = answer to a `RF_MSG_SYNC_REQUEST` while a switch to this activity runs.
- hub_rf polls the activity state every 200 ms. When a switch starts it sends `0x02` with the target (from `/tmp/activity_target`), only to remotes that report `RF_CAP_SWITCH_START`: an older remote would take it for the end. When the switch ends it sends `0x00` with the running activity, also when that is the old one (the switch failed). It answers a sync query during a switch with `0x03` and the target, again only to remotes that report `RF_CAP_SWITCH_START`; older remotes get `0x01` with the running activity. `GET /api/rf/sync` sends nothing during a switch (its end notice follows).
- The remote opens the activity's screen (or home for `"-1"`) only while it shows home or an activity screen. In Settings, pairing or the device browser it records the new activity and keeps the page; the activity is there when the user goes back. A switch started on the remote itself always ends on its target screen.
- Switch screen: `0x02` opens it on home and activity screens (or keeps the one the remote opened for its own switch). `0x03` does the same (a remote that woke or restarted during the switch). After a `0x02`, the next `0x00` or `0x01` closes it, whatever activity it names: the Hub answers `0x03` while the switch runs, so a `0x01` means it is over, also when the end notice was lost (the remote asks every 800 ms while the screen is on). The remote's own estimate closes it only 10 s after the estimated end, and not while the last `0x02` or `0x03` is less than 10 s old. The Hub reads its state and then sends the answer, so a notice can go out in between: the remote ignores a `0x01` or `0x03` that answers a query sent before the last `0x00` or `0x02` arrived. A `0x03` received with the screen off (a wake for an alarm) opens no switch screen; the first query with the screen on does. Without a `0x02` (older Hub) the screen closes on the `0x00` of its target or 400 ms after the estimate, as before. Once a Hub sent `0x02`, a `0x00` for a switch the remote did not show opens no switch screen (that switch is over).

#### `RF_MSG_TIME_SYNC` (`0x10`)
- Hub transmits time and timezone configuration:
  - `payload[0..3]`: Epoch timestamp (32-bit unsigned little-endian).
  - `payload[4]`: Timezone string length (`tz_len` <= 14).
  - `payload[5..18]`: Timezone prefix string (e.g. `"CET-1CEST"`, `"EST5EDT"`, `"IST-2IDT"`, up to 14 bytes). The remote expands the prefix to the full POSIX string with accurate Daylight Saving Time transition rules.
  - `payload[19]`: Update notification flags (`uflags`: bit 0 = the Hub's config version differs from the one the remote reported, bit 1 = OTA available). The remote uses bit 1 and compares the config version in `payload[20]` itself.
  - `payload[20]`: Config version number.
  - `payload[21..24]`: Hub's 4-byte RF address.
- The Hub sends it in answer to a quick `RF_MSG_SYNC_REQUEST`, but only when 300 s have passed since the last one to that slot or when anything after the time changed (timezone, flags, config version, Hub address). The frame asks for no ACK.

#### `RF_MSG_ACTIVITY_LIST_CHUNK` (`0x0B`)
- `payload[0]`: Activity index; `payload[1]`: activity count.
- `payload[2]`: Legacy icon type, `1`..`27` = `watchtv`, `movie`, `music`, `game`, `ps`, `xbox`, `switch`, `shield`, `apple`, `roku`, `firetv`, `pc`, `smarttv`, `sonos`, `receiver`, `light`, `power`, `projector`, `bluray`, `satellite`, `soundbar`, `hifi`, `netflix`, `night`, `fireplace`, `sunrise`, `fan` (`rf_activity_icon_keyword()` in `rf_proto.h`). The remote draws its own icon for each keyword.
- `payload[3..22]`: Activity ID and name: a 1–2 character ID leaves 17 bytes for the name, a longer ID 12. `RF_MSG_NAME_TAIL_CHUNK` carries the rest.
  - ID of 1 or 2 characters: `payload[3..5]` ID (NUL-terminated), `payload[6..22]` name.
  - Longer ID: `payload[3..10]` ID (8 bytes; a longer ID is cut to 8 characters in this frame), `payload[11..22]` name.
- `payload[23..24]`: 16-bit Icon ID (little-endian):
  - `0x0001` .. `0x7FFF`: Stock icon index into canonical stock asset table (`/opt/luadraws/appscommon/asset/...`).
  - `0x8001` .. `0xFFFF`: Custom user icon index (`/data/assets/custom/{icon_id}.png`), wirelessly synced over RF.
  - `0x0000`: Fallback to legacy keyword from `payload[2]`.

#### `RF_MSG_DEVICE_LIST_CHUNK` (`0x0C`)
- Hub transmits list of Hub devices during config push (ACK'd):
  - `payload[0]`: Device 0-based index.
  - `payload[1]`: Total device count.
  - `payload[2]`: Legacy icon type, `1`..`27` (`< 0x20`), matching activity icon types or `0` for fallback (`payload[2] >= 0x20` starts legacy 4-byte ID).
  - `payload[3..6]`: Shortened device ID string (null-terminated, up to 4 bytes).
  - `payload[7..22]`: Friendly device name string (null-terminated, up to 16 bytes). `RF_MSG_NAME_TAIL_CHUNK` carries the rest.
  - `payload[23..24]`: 16-bit Icon ID (little-endian):
    - `0x0001` .. `0x7FFF`: Stock icon index into canonical stock asset table (`/opt/luadraws/appscommon/asset/...`).
    - `0x8001` .. `0xFFFF`: Custom user icon index (`/data/assets/custom/{icon_id}.png`), wirelessly synced over RF.
    - `0x0000`: Fallback to legacy keyword from `payload[2]`.

#### `RF_MSG_BUTTON_PRESS` (`0x14`)
- Remote transmits on key events:
  - `payload[0..1]`: 16-bit Linux input event key code (big-endian).
  - `payload[2]`: State (`0x01` press, `0x02` hold, `0x00` release). While the key is down a hold follows the press after 300 ms and then every 300 ms: the Hub repeats a held infrared command by itself and stops when no hold arrived for 600 ms.
  - `payload[3..18]`: Short button name string (up to 16 chars).
  - `payload[19]`: Key event counter. The remote numbers every key frame it sends (press, hold and release alike): `1`..`255`, then `1` again. `0` means the remote does not count (older remotes). The counter starts at `1` when `remote_ui` starts and is not reset by a resume from suspend.
  - `payload[20..24]`: Device context ID string (up to 4 chars + NUL) when the remote is browsing a device in the Hub Device Browser view (`VIEW_DEVICE_BROWSER`). Hub routes physical buttons to this device or falls back to matching command names. When not browsing a device, bytes 20..24 are zeroed.

- Key frames are sent once: the Hub does not acknowledge them and the remote does not resend them. A lost press is a key that does nothing, a lost release leaves the key down.
- The counter only measures that loss. For each slot the Hub compares the number of a key frame with the one before it and adds the numbers in between (over the ring `1`..`255`) to `key_events_lost` in the RF status (`GET /api/rf/status`, `slots[].link`), with one log line per gap: `[!] RF key events lost: slot 1, 2 missing before #57 (release DirectionRight)`. The first key frame after `hub_rf` starts only sets the reference.
- A remote that restarted counts from `1` again. After a version report, a unit id frame or a link-up report from the CC2544, the Hub takes the next key frame `#n` as the restarted counter (`n - 1` frames missing, none for `#1`) when that is the smaller gap. Without such a hint a counter that goes back is counted as a gap around the ring. The same number twice in a row is logged as a frame received twice, not as a loss (`#1` twice with such a hint in between is a restart).
- The Hub also sorts the missing frames by what the frames around the gap show: a hold or release of a key whose press it did not see (`key_lost_press`), a key frame while the key before it is still down (`key_lost_release`), and the rest, holds or frames it cannot tell (`key_lost_other`). Only the last key that went down is tracked per slot.
- A frame with counter `0` is not measured at all. A Hub without the counter ignores the byte.

#### `RF_MSG_CONFIG_END` (`0x0F`) and the config push
- `payload[0]`: `0x00` clear (opens the push), `0x01` complete (closes it).
- `payload[1]`: Config version being pushed.
- `payload[2..3]` (clear only): Number of chunk frames (`0x0B`, `0x0C`, `0x0D`, `0x11`, `0x12`) that follow, 16-bit little-endian. `0` from Hubs that do not send it; the remote then shows no progress total.
- A push is: clear, the chunk frames and `RF_MSG_SETTINGS_PUSH`, then complete. Every frame requests an ACK and is sent at most 3 times, with 300 ms to wait for the ACK each time. The Hub stops pushing to a slot at the first frame that slot never ACKs, so a remote never receives complete for a partial config.
- **Streamed push** (remotes that report `RF_CAP_STREAM_PUSH`): the clear carries `payload[4]` = `1`
  and is ACKed as above. Then both radios switch to `RF_PM_TRANSFER` (the remote after it sent
  the ACK), and the Hub sends the rest of the push, complete included, without waiting.
  - Each frame carries `RF_FLAG_STREAM` (`flags` bit `0x04`). The low nibble of `hdr_flag` is the
    frame's index in the push, mod 16.
  - Every 4th frame, the last one, and the 15th in flight carry `RF_FLAG_ACK_REQ`. The remote
    answers with an `RF_MSG_ACK` whose `payload[2..3]` is the count of frames it took in order and
    `payload[4]` = `0xF2` (`RF_PUSH_STREAMED`). The frame handlers' own ACKs are not sent.
  - The remote takes a frame only when it is the next in order. The Hub keeps at most 15 frames in
    flight (the 4-bit index) and sends from the remote's count again when it has not grown for 100 ms
    (go-back-N). It gives up after 10 s without progress.
  - The Hub sends 2 ms apart, as for the windowed OTA. In `RF_PM_ACTIVE` the remote gets a Hub frame
    only with the ACK of one it sends, which held the stop-and-wait push to about 7 frames a second.
  - Both radios go back to `RF_PM_ACTIVE` at the end. The Hub does this when the remote has every
    frame or the push fails; the remote does it 3 s after the last stream frame. Until then it still
    answers a resend after its last ACK was lost.
- Limits of a push: 16 activities, 16 devices, 16 devices per activity, 255 commands per device, 255 button entries (`RF_BUTTON_LIST_MAX`), 64 steps. A device ID longer than 4 characters is cut to 4 in the device list, so the remote's device pages cannot address that device.
- A chunk frame that arrives without a clear opens a sync by itself (with no total, so the 30 s limit below applies) and is stored and ACKed like any other. A complete that arrives outside a sync is ACKed only when it carries the version the remote already holds (a resend after a lost ACK); with another version it is ignored and not ACKed. The remote applies the config and adopts the version on complete.
- If no frame arrives for 15 s (30 s when the clear carried no total), the remote abandons the push and restores its last complete config. It then waits 10 s (3 s when it has no config yet) before requesting a full sync on its own again.

#### `RF_MSG_DEVICE_CMD_CHUNK` (`0x12`)
- Hub sends one per command during a config push, after the `RF_MSG_DEVICE_LIST_CHUNK` frames:
  - `payload[0]`: Device index (position in the pushed device list).
  - `payload[1]`: Command index (position in the device's `commands` array in `devices.json`).
  - `payload[2]`: Number of commands for the device.
  - `payload[3..24]`: Command name (up to 22 chars, NUL-padded).
- A remote without device browsing does not ACK this type. For a remote that does not report `RF_CAP_DEVICE_CMDS`, the Hub stops sending device commands to that slot when the first one gets no ACK; for one that reports it, a missing ACK fails the push (it is not taken for a remote without device browsing).

#### `RF_MSG_BUTTON_LIST_CHUNK` (`0x0D`)
- Hub sends one per entry during a config push (ACK'd):
  - `payload[0]`: Entry index in this push.
  - `payload[1]`: Position of the button in its activity's on-screen list (or of the key in the activity's key map).
  - `payload[2]`: `0x01` on-screen button, `0x00` hard key.
  - `payload[3..11]`: Activity ID (9 bytes): the activity the on-screen button belongs to (`"-1"` = nothing running), the activity a hard key starts (`"-1"` for a stop key), or the activity a Help key belongs to.
  - `payload[12..23]`: Label (12 bytes); for a hard key, the button name.
  - `payload[24]`: `0x01` device command, `0x02` activity start, `0x03` activity stop, `0x04` Help.
- Only these entries are sent: the hard keys that start or stop an activity (the remote plays the switch by itself), the hard keys mapped to Help (`"type": "help"`) for a remote that reports `RF_CAP_HELP_KEY`, and the on-screen buttons. Every other hard-key mapping stays on the Hub, which maps the raw key. The start, stop and Help keys come first, at most 15, plus a `PwrOff` stop key when no activity maps one; then the on-screen buttons of every activity, up to 255 entries in all.
- A Help key opens the remote's Help screen while its activity runs (`RF_MSG_HELP_REQUEST`); in another activity the key goes to the Hub as a raw key. An older remote never gets Help keys: the Hub maps the raw key and calls `POST /api/activity/help`, which re-sends the running activity's commands without the power questions.

#### `RF_MSG_STEP_LIST_CHUNK` (`0x11`)
- Hub sends the steps the remote shows while an activity starts or stops, one per frame (ACK'd):
  - `payload[0]`: Chunk index in this push (at most 64 steps).
  - `payload[1]`: Step index within its sequence; bit 7 (`0x80`) marks a step of the activity's stop sequence.
  - `payload[2]`: Number of steps in the start sequence (at most 16 per activity, ending at its `Switched` step); `1` on every stop step (at most 16 per activity).
  - `payload[3..11]`: Activity ID (9 bytes), `"-1"` for power off.
  - `payload[12..13]`: Step delay in ms (16-bit little-endian; 250 when the step has none, capped at 20000; the delay of a `Delay` step that follows is added, up to 60000, and a `Delay` before the first command is a step named `Delay`).
  - `payload[14..24]`: Step name (11 bytes).
  - `payload[21]`, only in a step named `Warmup`: 1 + the device's index in the device list.
- Warmup: before the start sequence the Hub waits the longest `powerOnDelay` (1500 ms when a device has none) of the devices it powers on: devices of the new activity that the leaving activity does not use, that are not `isPowerAlwaysOn` and that the start sequence does not power on (`PowerOn`, `PowerToggle` or `Power`). Only the leaving activity is unknown at push time, so a start sequence begins with one `Warmup` step per such device (at most 4, longest first, capped at 60000 ms), and the remote keeps the longest one whose device the leaving activity does not use (`RF_MSG_ACT_DEVICES_CHUNK`). Only remotes that report `RF_CAP_SWITCH_START` get these steps; an older remote gets the list without them, its step numbers counted without them. They fill only the room the other steps leave of the remote's 64: an activity gets all its Warmup steps or none, the first activities first.

#### `RF_MSG_ACTIVITY_REQUEST` (`0x15`)
- Remote transmits to start an activity or power off (no ACK flag):
  - `payload[0..11]`: Activity ID, NUL-padded; `"-1"` = power off.

#### `RF_MSG_DEVICE_CMD_INDEX` (`0x13`)
- Remote transmits when a command is chosen in the device browser:
  - `payload[0..3]`: Shortened Device ID (4 bytes, null-terminated).
  - `payload[4]`: Command index.
  - `payload[5..24]`: First 20 chars of the command name prefix.
- The Hub sends the command at that index if its name matches the prefix, otherwise the first command whose name matches the prefix.

#### `RF_MSG_DEVICE_CMD_REQUEST` (`0x16`)
- Remote transmits for a screen button (bit 0 of the flags byte set; `rf_send_screen_button_press()`):
  - `payload[0..9]`: Activity (or device) id of the screen.
  - `payload[10]`: Button index.
  - `payload[11..22]`: Button label (first 12 chars).
  - The Hub sends the button's command from the remote's key map; a button that is not in the map sends nothing.
- Direct device command execution (flags bit 0 clear). The Hub still accepts it; today's `remote_ui` does not send it:
  - `payload[0..3]`: Shortened Device ID (4 bytes, null-terminated).
  - `payload[4..24]`: Command name (up to 21 chars, null-terminated).

#### `RF_MSG_BATTERY_STATUS` (`0x18`)
- Remote transmits battery status:
  - `payload[0]`: Battery percentage (0–100%).
  - `payload[1]`: Charging flag (`0x01` charging, `0x00` discharging).
  - `payload[2..3]`: Voltage in millivolts (16-bit unsigned little-endian).

#### RF OTA transfer (`0x22`..`0x25` and the `0x60` data frame)
- `RF_MSG_OTA_START` (`0x22`, ACK'd): `payload[0..3]` image size, `payload[4..5]` chunk count for the stop-and-wait transfer (22-byte chunks), `payload[6..9]` CRC32 of the image (identifies a checkpoint). The remote's ACK: `payload[1]` status (`0x02` battery under 40 % and not charging, `0x03` cannot write, also when `/tmp` has no room for the image or the image size is outside 1..1,441,770 bytes, `RF_OTA_MAX_BYTES` = 65535 chunks of 22 bytes), `payload[2..3]` the 22-byte chunk to resume from (checkpoint made by a stop-and-wait transfer), `payload[4]` = `0xF1` (`RF_OTA_WINDOWED`) if the remote supports the windowed transfer, `payload[6..7]` the 26-byte chunk to resume from (checkpoint made by a windowed transfer). A remote cannot continue a checkpoint with the other kind of frames; it starts over. Before `RF_MSG_OTA_START` the Hub sets its CC2544 to power mode `0x30` (section 4.2) and sends up to 4 `RF_MSG_WAKE` frames; it then sends `RF_MSG_OTA_START` up to 10 times (1 s wait for the ACK, 0.8 s pause). The remote sets its own radio to `0x30` when it accepts the frame, and leaves OTA mode (keeping its checkpoint) after 60 s without a frame.
- Stop-and-wait transfer (`RF_MSG_OTA_CHUNK` `0x23`): `payload[0..1]` chunk index, `payload[2]` length, `payload[3..24]` up to 22 data bytes. The Hub asks for an ACK on every 4th chunk and waits for it (15 retries with backoff and CC2544 wake pulses), 15 ms between chunks. About 20 chunks/s (440 B/s) on a Harmony Ultimate. Used for remotes that do not announce `0xF1`.
- Windowed transfer (remote announced `0xF1`): the Hub streams **OTA data frames**, which have no message type: like Logitech's own HOT continuation packets they use the header byte `0x60`: `12 <slot> <0x60 | idx[3:0] | ack> <idx[11:4]> <data[26]>` (30-byte Hub frame; the remote reads it as `20 <pipe> 12 <slot> <hdr> <idx[11:4]> <data[26]>`). `ack` = `0x10` asks for an ACK; the chunk index is 12 bits and the remote completes it from the chunk it expects next; the last chunk is shorter (the remote knows the image size). Every ACK carries the remote's contiguous chunk count in `payload[2..3]` and `0xF1` in `payload[4]`. The Hub streams with 2 ms between frames (the Hub's CC2544 queues 8 and drops frames beyond that), asks for an ACK every 16th chunk without waiting, stays at most 64 chunks ahead of the contiguous count, and resends from that count when it has not advanced for 300 ms (go-back-N). It gives up (resumable) after 20 s without progress. A remote accepts these frames only while in OTA mode. Measured on a Harmony Ultimate: about 250 frames/s, 6.5 KB/s, the 667 KB image in about 100 s.
- `RF_MSG_OTA_END` (`0x25`, ACK'd): `payload[0]` = `0x01`, `payload[1..20]` first 20 bytes of the image's SHA-256. The remote checks it has every chunk (`0x05` otherwise) and that the image was written to `/tmp` (`0x03` otherwise), verifies the digest (`0x06` on mismatch, and also when `payload[0]` is not `0x01`: an image is never installed without the digest; Hubs send it since v2.0.0), checks that the unpacked `remote_ui` is an ELF file (`0x05` otherwise), installs (`0x07` when the image is good but cannot be put in place, for example no space left on `/data`: the current binary is kept) and then reboots when it is on the cradle or on USB power; on battery it starts the new `remote_ui` in place instead (a reboot on battery switches the remote off), and a changed boot script applies at the next boot. The ACK comes after the install, so the Hub sends the same frame (same sequence number) up to 3 times, waiting 6 s each; a remote that already took it ignores the others. A remote whose checkpoint holds every chunk (its OTA_END was lost) answers OTA_START with the chunk count as the resume chunk, and the Hub then sends only OTA_END.

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
