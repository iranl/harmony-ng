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
  - `0x50 | (seq & 0x0F)`: Single frame requesting immediate ACK (Bit 4 = `PacketAck`).
  - `0x40 | (seq & 0x0F)`: Standard frame without ACK requirement.
  - `0x60 | (seq & 0x0F)`: Multi-packet data chunk frame.
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
- **Byte 4 (`Hdr/Seq`)**: `0x50 | (seq & 0x0F)` (single frame + ACK requested) or `0x40 | (seq & 0x0F)`.
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
On Remote startup or display wake, the CC2544 radio's frequency-hopping spread-spectrum (FHSS) engine must synchronize with the Hub beacon:
```
12 01 00 00 00 00 00 00 00 00
```
- Writing this 10-byte pulse to `/dev/rfspi` triggers `libhal_getactivedongle` on CC2544, aligning the channel-hopping sequence.

---

## 4. CC2544 Control & Pairing Commands

### 4.1 Subsystem Initialization
- **Init Command** (`10 FF 80 00 00 01 00`): Wakes and initializes CC2544 messaging engine.
- **Factory RF test step** (`10 FF 80 FD 01 00 00`): steps the CC2544 to its next factory test mode; reply `10 FF 80 FD 01 <mode> 00`. Logitech's firmware sends it only from the hidden factory test menu (`rf.test`), which names the modes: `0` RX sweep, `1`-`3` TX unmodulated at 2405/2444/2474 MHz, `4`-`6` TX modulated at the same channels, `7`-`9` RX at those channels, `10` TX sweep unmodulated, `11` ready. A reboot clears it. Not for normal operation: on the remote, from the fifth write since boot the link stays lost (`10 01 41 04 47`). Only `01` is accepted (`00` is rejected with `10 FF 8F 80 FD 03 00`). `remote_ui` does not send it.
- **Firmware Version Query** (`10 FF 81 F1 01 00 00`): Returns firmware version. Reply: `10 FF 81 F1 01 <ver> 00` (e.g. `0x27` = v39.00).

### 4.2 Power Mode Management
- **Set Power Mode** (`20 <mode> 00 00 00 00 00`, `hot_set_powermode()` in `libhal/rf/hot.c`, same on Hub and remote):
  - `0x20` (`RF_PM_ACTIVE`): normal operation. Stock sets it when its HOT state machine enters Idle; harmony-ng at start.
  - `0x30` (`RF_PM_TRANSFER`): for the whole of a transfer. Stock sets it when the state machine leaves Idle (`hot_Idle.exit`) and goes back to `0x20` 500 ms after the last object (`hot_CoolDown`). In this mode the remote's CC2544 fetches the Hub's queued frames by itself, about one every 4 ms on a Harmony Ultimate; in `0x20` it gets one only in the acknowledgement of a frame it transmits. harmony-ng uses it for the windowed RF OTA (measured: 250 frames/s with no remote transmission, against 7/s in `0x20`).
  - `0x40`: not used by stock; on a Harmony Ultimate the remote stops answering until `0x20` is set again.
  - A remote in device mode puts the current mode in byte 1 of every frame it sends (`mode | pipe`, section 3.2).

### 4.3 Pipe & Paired Device Table
Query command: `10 FF 83 B5 <pipe> 00 00`:
- **Hub Responses (`11 FF 83 B5 <pipe> ...`)**:
  - `pipe 0x03`: Hub's own 4-byte RF address (`11 FF 83 B5 03 <addr:4> ...`).
  - `pipe 0x20..0x25`: Slot identification / EquadID (`11 FF 83 B5 20 <EquadID:2> ...`). The EquadID names the hardware family, not the product. The list the stock Hub firmware knows (`pimento_rfthread`):
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
- **Request Pairing (Remote)**: `10 FF 80 B2 01 01 00`. Initiates bidirectional handshake from Remote.
- **Close Pairing Lock (Hub)**: `10 FF 80 B2 02 00 00`. Closes pairing window.
- **Unpair Slot**: `10 FF 80 B2 03 <slot> 00` (`0x01` = Slot 1, `0x02` = Slot 2, `0x00` = All).

### 4.5 Hardware Link Events & Notifications
- **Link Status Event** (`10 <slot> 41 04 <status> <wpid_lo> <wpid_hi>`):
  - `status & 0x40 != 0`: Link **LOST** (`0x47`, `0x40`).
  - `status & 0x40 == 0`: Link **ESTABLISHED** (Remote: `0x07`, `0x0B`; Hub: `0x87`).
  - `<wpid_lo> <wpid_hi>`: wireless product ID of the peer, little-endian, as in the HID++ 1.0 device connection notification. The Remote reports `0B 88` (the Hub, `0x880B`); the Hub reports `21 40` (the touchscreen remote, `0x4021`). The bytes stay the same whether the link is lost or established, so they carry no signal level.
- **Hardware TX Status Report** (`20 <slot> 42 <status>`):
  - `status == 0x00`: The remote's radio received the packet. The remote application can still drop it, so this does not count as delivery; only `RF_MSG_ACK` does.
  - `status != 0x00`: TX failure / drop (`0x01` while the link is lost).

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
| `0x1E` | `RF_MSG_ACK` | Both | Application-level acknowledgment (by SEQ) |
| `0x1F` | `RF_MSG_PING` | Both | Keepalive / ping query |
| `0x22` | `RF_MSG_OTA_START` | Hub -> Remote | Start OTA binary update (size, chunks) |
| `0x23` | `RF_MSG_OTA_CHUNK` | Hub -> Remote | 24-byte binary payload chunk |
| `0x24` | `RF_MSG_OTA_BLOCK_ACK` | Remote -> Hub | OTA block completion ACK |
| `0x25` | `RF_MSG_OTA_END` | Hub -> Remote | OTA transfer complete and checksum verify |
| `0x26` | `RF_MSG_ICON_START` | Hub -> Remote | Custom icon transfer initiate (target_type, size, md5) |
| `0x27` | `RF_MSG_ICON_CHUNK` | Hub -> Remote | Custom icon PNG data chunk (23 bytes payload) |
| `0x28` | `RF_MSG_ICON_END` | Hub -> Remote | Custom icon transfer complete, checksum verify, save to `/data/assets/custom/{id}.png` |
| `0x2B` | `RF_MSG_ACT_DEVICES_CHUNK` | Hub -> Remote | Devices one activity uses (ACK'd); sent only to remotes that report `RF_CAP_ACT_DEVICES` |
| `0x2C` | `RF_MSG_NAME_TAIL_CHUNK` | Hub -> Remote | Rest of an activity or device name (ACK'd); sent only to remotes that report `RF_CAP_LONG_NAMES` |
| `0x2D` | `RF_MSG_HELP_REQUEST` | Remote -> Hub | Help: repair the running activity |
| `0x2E` | `RF_MSG_HELP_STATUS` | Hub -> Remote | Help progress, or a device to ask about (ACK'd except a repeated "started"); sent only in reply to `RF_MSG_HELP_REQUEST` |
| `0x2F` | `RF_MSG_HELP_ANSWER` | Remote -> Hub | Help: whether a device is on (ACK'd) |
| `0x30` | `RF_MSG_WAKE` | Hub -> Remote | Turn the screen on (ACK'd); a remote in light standby still receives frames. Remotes without it drop it as unknown and send no ACK |
| `0x31` | `RF_MSG_UNIT_ID` | Remote -> Hub | The remote's factory unit id and skin id (ACK'd); sent right after every `RF_MSG_VERSION_REPORT` by remotes that report `RF_CAP_UNIT_ID`. A Hub without it drops the frame as unknown |

### 5.2 Key Payload Structures

#### `RF_MSG_VERSION_REPORT` (`0x1A`)
- `payload[0..2]`: Software version major, minor, patch
- `payload[3]`: Config version the remote holds
- `payload[4]`: Capabilities, a bit mask of `RF_CAP_*`. Older remotes send `0x00`.
  - `0x01` `RF_CAP_ACT_DEVICES`: understands `RF_MSG_ACT_DEVICES_CHUNK`
  - `0x02` `RF_CAP_LONG_NAMES`: understands `RF_MSG_NAME_TAIL_CHUNK`
  - `0x04` `RF_CAP_UNIT_ID`: sends `RF_MSG_UNIT_ID` after every version report

The remote sends this report when its link first comes up, every 24 hours, and before each full config request, so a Hub that restarted learns the capabilities before it builds the push. The Hub keeps them in `/data/harmony/remote_slot<N>_version.json` (`caps`). A remote drops frame types it doesn't know without an ACK, so the Hub must not send newer frame types to a remote that doesn't report them.

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

#### `RF_MSG_SETTINGS_PUSH` (`0x0E`) & `RF_MSG_REMOTE_CONFIG_BACKUP` (`0x19`)
Both frames use a 17-byte standardized configuration payload:
- `payload[0]`: Display Brightness (1–5)
- `payload[1]`: Screen Timeout low byte (seconds)
- `payload[2]`: Touchscreen Enabled (`0x01` on, `0x00` off)
- `payload[3..4]`: Nav Toggle Key Linux code (16-bit unsigned little-endian, e.g. `139` = KEY_MENU). Current remote firmware has no NAV toggle: it ignores these bytes and sends `0`; older firmware reads them.
- `payload[5]`: Nav Toggle Mode (`0x00` Short Press, `0x01` Long Press); ignored and sent as `0` like the key
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
- `payload[20]`: Language (`0` en, `1` de, `2` fr, `3` nl, `4` es, `5` it); byte 19 in backup
- `payload[21]`: Theme (`1` classic, `2` dark, `3` light, `4` colourful; `0` in a push keeps the Remote's choice); byte 20 in backup, where `0` means a theme with no id
- `payload[22]`: Home layout (`1` the theme's, `2` list, `3` grid, `4` carousel; `0` in a push keeps the Remote's choice); byte 21 in backup

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
  - `payload[4]`: Timezone string length (`tz_len` <= 14).
  - `payload[5..18]`: Timezone prefix string (e.g. `"CET-1CEST"`, `"EST5EDT"`, `"IST-2IDT"`, up to 14 bytes). The remote expands the prefix to the full POSIX string with accurate Daylight Saving Time transition rules.
  - `payload[19]`: Update notification flags (`uflags`: bit 1 = OTA available).
  - `payload[20]`: Config version number.
  - `payload[21..24]`: Hub's 4-byte RF address.

#### `RF_MSG_ACTIVITY_LIST_CHUNK` (`0x0B`)
- `payload[0]`: Activity index; `payload[1]`: activity count.
- `payload[2]`: Legacy icon type, `1`..`17` = `watchtv`, `movie`, `music`, `game`, `ps`, `xbox`, `switch`, `shield`, `apple`, `roku`, `firetv`, `pc`, `smarttv`, `sonos`, `receiver`, `light`, `power` (`rf_activity_icon_keyword()` in `rf_proto.h`). The remote draws its own icon for each keyword.
- `payload[3..22]`: Activity ID and name: a 1–2 character ID leaves 17 bytes for the name, a longer ID 12. `RF_MSG_NAME_TAIL_CHUNK` carries the rest.
- `payload[23..24]`: 16-bit Icon ID (little-endian):
  - `0x0001` .. `0x7FFF`: Stock icon index into canonical stock asset table (`/opt/luadraws/appscommon/asset/...`).
  - `0x8001` .. `0xFFFF`: Custom user icon index (`/data/assets/custom/{icon_id}.png`), wirelessly synced over RF.
  - `0x0000`: Fallback to legacy keyword from `payload[2]`.

#### `RF_MSG_DEVICE_LIST_CHUNK` (`0x0C`)
- Hub transmits list of Hub devices during config push (ACK'd):
  - `payload[0]`: Device 0-based index.
  - `payload[1]`: Total device count.
  - `payload[2]`: Legacy icon type, `1`..`17` (`< 0x20`), matching activity icon types or `0` for fallback (`payload[2] >= 0x20` starts legacy 4-byte ID).
  - `payload[3..6]`: Shortened device ID string (null-terminated, up to 4 bytes).
  - `payload[7..22]`: Friendly device name string (null-terminated, up to 16 bytes). `RF_MSG_NAME_TAIL_CHUNK` carries the rest.
  - `payload[23..24]`: 16-bit Icon ID (little-endian):
    - `0x0001` .. `0x7FFF`: Stock icon index into canonical stock asset table (`/opt/luadraws/appscommon/asset/...`).
    - `0x8001` .. `0xFFFF`: Custom user icon index (`/data/assets/custom/{icon_id}.png`), wirelessly synced over RF.
    - `0x0000`: Fallback to legacy keyword from `payload[2]`.

#### `RF_MSG_BUTTON_PRESS` (`0x14`)
- Remote transmits on key events:
  - `payload[0..1]`: 16-bit Linux input event key code (big-endian).
  - `payload[2]`: State (`0x01` press, `0x02` hold, `0x00` release).
  - `payload[3..18]`: Short button name string (up to 16 chars).
  - `payload[20..24]`: Device context ID string (up to 4 chars + NUL) when the remote is browsing a device in the Hub Device Browser view (`VIEW_DEVICE_BROWSER`). Hub routes physical buttons to this device or falls back to matching command names. When not browsing a device, bytes 20..24 are zeroed.

#### `RF_MSG_CONFIG_END` (`0x0F`) and the config push
- `payload[0]`: `0x00` clear (opens the push), `0x01` complete (closes it).
- `payload[1]`: Config version being pushed.
- `payload[2..3]` (clear only): Number of chunk frames (`0x0B`, `0x0C`, `0x0D`, `0x11`, `0x12`) that follow, 16-bit little-endian. `0` from Hubs that do not send it; the remote then shows no progress total.
- A push is: clear, the chunk frames and `RF_MSG_SETTINGS_PUSH`, then complete. Every frame requests an ACK and is retried up to 3 times. The Hub stops pushing to a slot at the first frame that slot never ACKs, so a remote never receives complete for a partial config.
- The remote accepts chunk frames only between clear and complete, and leaves stray ones unACKed. It applies the config and adopts the version on complete.
- If no frame arrives for 15 s (45 s when the clear carried no total), the remote abandons the push and restores its last complete config. It then waits 60 s before requesting a full sync on its own again.

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

#### RF OTA transfer (`0x22`..`0x25` and the `0x60` data frame)
- `RF_MSG_OTA_START` (`0x22`, ACK'd): `payload[0..3]` image size, `payload[4..5]` chunk count for the stop-and-wait transfer (22-byte chunks), `payload[6..9]` CRC32 of the image (identifies a checkpoint). The remote's ACK: `payload[1]` status (`0x02` low battery, `0x03` cannot write), `payload[2..3]` the 22-byte chunk to resume from (checkpoint made by a stop-and-wait transfer), `payload[4]` = `0xF1` (`RF_OTA_WINDOWED`) if the remote supports the windowed transfer, `payload[6..7]` the 26-byte chunk to resume from (checkpoint made by a windowed transfer). A remote cannot continue a checkpoint with the other kind of frames; it starts over.
- Stop-and-wait transfer (`RF_MSG_OTA_CHUNK` `0x23`): `payload[0..1]` chunk index, `payload[2]` length, `payload[3..24]` up to 22 data bytes. The Hub asks for an ACK on every 4th chunk and waits for it (15 retries with backoff and CC2544 wake pulses), 15 ms between chunks. About 20 chunks/s (440 B/s) on a Harmony Ultimate. Used for remotes that do not announce `0xF1`.
- Windowed transfer (remote announced `0xF1`): the Hub sets CC2544 power mode `0x30` on both ends (section 4.2) and streams **OTA data frames**, which have no message type: like Logitech's own HOT continuation packets they use the header byte `0x60`: `12 <slot> <0x60 | idx[3:0] | ack> <idx[11:4]> <data[26]>` (30-byte Hub frame; the remote reads it as `20 <pipe> 12 <slot> <hdr> <idx[11:4]> <data[26]>`). `ack` = `0x10` asks for an ACK; the chunk index is 12 bits and the remote completes it from the chunk it expects next; the last chunk is shorter (the remote knows the image size). Every ACK carries the remote's contiguous chunk count in `payload[2..3]` and `0xF1` in `payload[4]`. The Hub streams with 2 ms between frames (the Hub's CC2544 queues 8 and drops frames beyond that), asks for an ACK every 16th chunk without waiting, stays at most 64 chunks ahead of the contiguous count, and resends from that count when it has not advanced for 300 ms (go-back-N). It gives up (resumable) after 20 s without progress. A remote accepts these frames only while in OTA mode. Measured on a Harmony Ultimate: about 250 frames/s, 6.5 KB/s, the 667 KB image in about 100 s.
- `RF_MSG_OTA_END` (`0x25`, ACK'd): `payload[0]` = `0x01`, `payload[1..20]` first 20 bytes of the image's SHA-256. The remote checks it has every chunk (`0x05` otherwise), verifies the digest (`0x06` on mismatch), installs and reboots.

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
