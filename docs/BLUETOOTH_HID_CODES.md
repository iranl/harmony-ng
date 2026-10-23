# Bluetooth HID Button & KeyCode Reference

Harmony Hub supports emulating a Bluetooth HID Keyboard & Consumer device. Custom Bluetooth commands can be assigned using the format:

```text
HID:<Page>:<Usage>
```

- `<Page>` is a 2-digit hexadecimal HID Usage Page (`07` for Keyboard / Keypad, `0C` for Consumer / Media).
- `<Usage>` is a 4-digit hexadecimal HID Usage ID.

---

## 1. Consumer & Media Controls (Usage Page `0x0C`)
Used for standard streaming box navigation, Android TV / NVIDIA Shield / Apple TV / Fire TV media controls, and audio volume.

| Button / Function | HID Code | Description |
|:---|:---|:---|
| **Home** | `HID:0C:0223` | AC Home (Android TV / Shield / Fire TV / Apple TV Home) |
| **Back** | `HID:0C:0224` | AC Back (Return to previous screen) |
| **Menu** | `HID:0C:0040` | Menu / Context Options |
| **Play / Pause (Toggle)** | `HID:0C:00CD` | Play / Pause toggle |
| **Play** | `HID:0C:00B0` | Dedicated Play |
| **Pause** | `HID:0C:00B1` | Dedicated Pause |
| **Stop** | `HID:0C:00B7` | Stop playback |
| **Fast Forward** | `HID:0C:00B3` | Fast Forward |
| **Rewind** | `HID:0C:00B4` | Rewind / Scan backward |
| **Next Track / Skip** | `HID:0C:00B5` | Skip Forward |
| **Previous Track** | `HID:0C:00B6` | Skip Backward |
| **Volume Up** | `HID:0C:00E9` | Increment Volume |
| **Volume Down** | `HID:0C:00EA` | Decrement Volume |
| **Mute** | `HID:0C:00E2` | Mute Audio |
| **Power Toggle** | `HID:0C:0030` | Power On / Standby |
| **Search / Assistant** | `HID:0C:0221` | Voice Search / AC Search |
| **Channel Up** | `HID:0C:009C` | Channel Increment |
| **Channel Down** | `HID:0C:009D` | Channel Decrement |
| **Info / Guide** | `HID:0C:008D` | Program Guide |

---

## 2. Keyboard & Navigation Controls (Usage Page `0x07`)
Used for directional navigation, text input, Enter, and system shortcuts.

### Navigation & Core Buttons
| Button / Function | HID Code | Description |
|:---|:---|:---|
| **Enter / Select / OK** | `HID:07:0028` | Keyboard Return / Enter |
| **Up Arrow** | `HID:07:0052` | Direction Up |
| **Down Arrow** | `HID:07:0051` | Direction Down |
| **Left Arrow** | `HID:07:0050` | Direction Left |
| **Right Arrow** | `HID:07:004F` | Direction Right |
| **Escape / Exit** | `HID:07:0029` | Escape / Cancel |
| **Backspace** | `HID:07:002A` | Backspace (alternative Back) |
| **Tab** | `HID:07:002B` | Tab key |
| **Space** | `HID:07:002C` | Spacebar |
| **Context Menu** | `HID:07:0065` | Application / Context Menu key |
| **Page Up** | `HID:07:004B` | Page Up |
| **Page Down** | `HID:07:004E` | Page Down |

### Numeric Keys
| Key | HID Code | Key | HID Code |
|:---|:---|:---|:---|
| **0** | `HID:07:0027` | **5** | `HID:07:0022` |
| **1** | `HID:07:001E` | **6** | `HID:07:0023` |
| **2** | `HID:07:001F` | **7** | `HID:07:0024` |
| **3** | `HID:07:0020` | **8** | `HID:07:0025` |
| **4** | `HID:07:0021` | **9** | `HID:07:0026` |

---

## 3. Special Device Actions
Harmony Hub also accepts built-in connection keywords:

| Action | Command Value | Description |
|:---|:---|:---|
| **Connect** | `Connect` or `BT_CONNECT` | Connects or reconnects to the paired Bluetooth host |
| **Disconnect** | `Disconnect` or `BT_DISCONNECT` | Disconnects the active Bluetooth HID session |

---

## 4. Mouse & Pointer Controls (Report ID `0x03`)
Hub presents a composite HID device: Keyboard (Report 1), Consumer (Report 2), and Mouse (Report 3).

### Direct FIFO Commands (`/tmp/bthid_input`)
- `MOUSE <btn_mask> <dx> <dy> [wheel]` — Send pointer move or click (`btn_mask`: bit 0=Left, bit 1=Right, bit 2=Middle).
- `MOUSE_MOVE <dx> <dy>` — Move relative pointer coordinates (-127 to 127).
- `MOUSE_CLICK <left|right|middle>` — Quick button tap with auto-release after 40ms.
- `MOUSE_DOWN <left|right|middle>` — Hold mouse button down.
- `MOUSE_UP <left|right|middle>` — Release mouse button.
- `MOUSE_WHEEL <steps>` — Vertical scroll wheel step (+1 up, -1 down).

### HTTP & WebSocket API
- **HTTP**: `POST /api/bt-mouse` with JSON:
  ```json
  {"dx": 10, "dy": -5, "buttons": 0, "wheel": 0, "action": "move"}
  ```
- **WebSocket**: `{ "action": "bt_mouse", "dx": 10, "dy": -5, "mouseAction": "move" }`

---

## 5. Physical BLE Remotes (e.g. Homatics B25)
Harmony Hub also operates as a BLE central host to receive inputs from paired physical Bluetooth Low Energy remotes (such as the Homatics B25), mapping physical buttons to activities and device commands.

> [!NOTE]
> **Sleep & Wake Reconnection**:
> When a BLE remote is in low-power sleep mode, the first button press wakes up the remote's radio and initiates reconnection to the Harmony Hub. On the Homatics B25, this first press will be dropped and show a red light indicator. The second button press will register and execute normally once the BLE remote finishes reconnecting.

---

## 6. Official Specification Reference
For a complete list of all 65,536 HID usage definitions across all pages, consult the official standard:
- [USB Implementers Forum: HID Usage Tables v1.5](https://usb.org/document-library/hid-usage-tables-15)


