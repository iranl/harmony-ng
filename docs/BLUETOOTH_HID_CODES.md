# Bluetooth HID Button & KeyCode Reference

Harmony Hub supports emulating a Bluetooth HID Keyboard & Consumer device. Custom Bluetooth commands can be assigned using the format:

```text
HID:<Page>:<Usage>
```

- `<Page>` is a 2-digit hexadecimal HID Usage Page (`07` for Keyboard / Keypad, `0C` for Consumer / Media).
- `<Usage>` is a 4-digit hexadecimal HID Usage ID.

The Hub sends two ranges as codes: page `07` with usage `0001` to `00FF`, and page `0C` with usage `0001` to `03FF`. A code with another page or usage is not sent as a code: the Hub then sends the command by its name (see [Built-in Key Names](#built-in-key-names)). The stock Logitech keycode form `G:HID 16 Bit:()(PP00UUUU)():1` (page `PP`, usage `UUUU`) is accepted as well.

A tap sends the key down and up. A key held on a remote goes down at the press and stays down until the release (the Hub releases it by itself after 10 seconds when no release arrives); the hold events in between send nothing, the host repeats a held key itself.

---

## 1. Consumer & Media Controls (Usage Page `0x0C`)
Used for standard streaming box navigation, Android TV / NVIDIA Shield / Apple TV / Fire TV media controls, and audio volume.

| Button / Function | HID Code | Description |
|:---|:---|:---|
| **Home** | `HID:0C:0223` | AC Home (Android TV / Shield / Fire TV / Apple TV Home) |
| **Back** | `HID:0C:0224` | AC Back (Return to previous screen) |
| **Menu** | `HID:0C:0040` | Menu / Context Options (a command named `Menu` without a code sends `HID:0C:008D` instead, see [Built-in Key Names](#built-in-key-names)) |
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
| **Guide** | `HID:0C:008D` | Program Guide (also what the built-in names `Menu`, `Settings` and `Guide` send) |
| **Info** | `HID:0C:01BD` | What the built-in name `Info` sends |

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

### Built-in Key Names
A command of a Bluetooth device that has no usable `HID:` code is sent by its name. The Hub knows the names below (upper and lower case are the same, except for single letters). A name that is not in the list sends nothing.

| Command name | Sent as |
|:---|:---|
| `DirectionUp` / `ArrowUp` / `up`, `DirectionDown` / `ArrowDown` / `down` | `HID:07:0052`, `HID:07:0051` |
| `DirectionLeft` / `ArrowLeft` / `left`, `DirectionRight` / `ArrowRight` / `right` | `HID:07:0050`, `HID:07:004F` |
| `Select` / `OK` / `Enter` | `HID:07:0028` |
| `Back` / `Return` / `Exit` | `HID:0C:0224` |
| `Home` | `HID:0C:0223` |
| `Menu` / `Settings` / `Guide` | `HID:0C:008D` |
| `Info` | `HID:0C:01BD` |
| `Play`, `Pause`, `PlayPause` / `play_pause`, `Stop` | `HID:0C:00B0`, `HID:0C:00B1`, `HID:0C:00CD`, `HID:0C:00B7` |
| `SkipForward` / `NextTrack` / `next`, `SkipBack` / `PreviousTrack` / `prev` | `HID:0C:00B5`, `HID:0C:00B6` |
| `FastForward`, `Rewind` | `HID:0C:00B3`, `HID:0C:00B4` |
| `VolumeUp` / `vol_up`, `VolumeDown` / `vol_down`, `Mute` | `HID:0C:00E9`, `HID:0C:00EA`, `HID:0C:00E2` |
| `PowerToggle` / `power` | `HID:0C:0030` |
| `ChannelUp` / `ch_up`, `ChannelDown` / `ch_down` | `HID:0C:009C`, `HID:0C:009D` |
| `red`, `green`, `yellow`, `blue` | `HID:0C:0069`, `HID:0C:006A`, `HID:0C:006C`, `HID:0C:006B` |
| `0` to `9`, `Number0` to `Number9` | the codes of the Numeric Keys table |
| `escape` / `esc`, `backspace`, `tab`, `space` | `HID:07:0029`, `HID:07:002A`, `HID:07:002B`, `HID:07:002C` |
| `insert`, `delete`, `end`, `pageup`, `pagedown`, `capslock` | `HID:07:0049`, `HID:07:004C`, `HID:07:004D`, `HID:07:004B`, `HID:07:004E`, `HID:07:0039` |
| `minus`, `equal`, `leftbracket`, `rightbracket`, `backslash` | `HID:07:002D`, `HID:07:002E`, `HID:07:002F`, `HID:07:0030`, `HID:07:0031` |
| `semicolon`, `apostrophe`, `grave`, `comma`, `period`, `slash` | `HID:07:0033`, `HID:07:0034`, `HID:07:0035`, `HID:07:0036`, `HID:07:0037`, `HID:07:0038` |
| `F1` to `F12` | `HID:07:003A` to `HID:07:0045` |
| one letter, `a` to `z` | `HID:07:0004` to `HID:07:001D` (an upper-case letter is sent with Shift) |
| `0x` and a hexadecimal number, for example `0x0221` | that Consumer usage |

A keyboard name can carry modifiers in front: `ctrl+`, `shift+`, `alt+` and `win+` (also written `gui+` or `cmd+`), for example `ctrl+c`.

---

## 3. Special Device Actions
A command with one of the names below is not a key. It tells the Hub to open or close the Bluetooth link to the host of the device the command belongs to. The Hub looks at the command's name, not at its value, and acts once per tap or press.

| Action | Command name | Description |
|:---|:---|:---|
| **Connect** | `Connect` or `BT_CONNECT` | Connects (or reconnects) the Hub to the Bluetooth host of this command's device |
| **Disconnect** | `Disconnect` or `BT_DISCONNECT` | Closes the link to the Bluetooth host of this command's device |

### Removing a paired host
**Forget** next to a host in the web UI's Bluetooth host list (or `POST /api/bt-forget-host`,
admin only, see [API.md](API.md)) removes the host for good: its link key, its link, its entry in the
host list, and the link of every device that used it. Bluetooth must be on. Those devices then send
their keys to the default host (the last host paired), which can be another TV or box: link them to
another host or pair them again. Also remove the Hub ("Harmony Keyboard") from the host's own
Bluetooth list. A forgotten host can only come back through a new pairing in pairing mode.
Disconnect only closes the link; the host stays paired and reconnects.

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

Movement and wheel values outside -127 to 127 are limited to that range. A button held with `MOUSE_DOWN` is released by the Hub after 10 seconds without another mouse report. A line `TARGET <bluetooth address>` in front of a command selects the host that gets it. The same pipe takes the key commands the Hub uses itself: `KBD <hex usage>` and `CONSUMER <hex usage>` (tap), `KBD_DOWN <hex usage>` and `CONSUMER_DOWN <hex usage>` (held until `RELEASE` or `CONSUMER_UP`), `KEY <name>`, `KEYDOWN <name>` and `KEYUP <name>` (the [Built-in Key Names](#built-in-key-names)), `TEXT <text>` and `RAW <hex bytes of a report>`.

### HTTP & WebSocket API
- **HTTP**: `POST /api/bt-mouse` (also `POST /api/v1/bt/mouse`) with JSON:
  ```json
  {"dx": 10, "dy": -5, "buttons": 0, "wheel": 0, "action": "move"}
  ```
  `action` is `click`, `down` or `up` (with `button`: `left`, `right` or `middle`; `left` when absent), `wheel` (with `wheel`), or anything else for a move with `dx`, `dy`, `buttons` (button mask) and `wheel`. Optional `target`: the Bluetooth address of the host that gets the report. A `target` that is not a Bluetooth address, or a `button` that is not a plain name, is answered with `400`. The answer is `{"ok": true}` when the command was handed to the Bluetooth daemon, else `{"ok": false}`.
- **WebSocket**: `{ "action": "bt_mouse", "dx": 10, "dy": -5, "mouseAction": "move" }`. The fields are the same, with the mouse action in `mouseAction`. The reply has error `503` when the Bluetooth stack is not running.

---

## 5. Physical BLE Remotes (e.g. Homatics B25)
Harmony Hub also operates as a BLE central host to receive inputs from paired physical Bluetooth Low Energy remotes (such as the Homatics B25), mapping physical buttons to activities and device commands.

> [!NOTE]
> **Sleep & Wake Reconnection**:
> When a BLE remote is in low-power sleep mode, the first button press wakes up the remote's radio and initiates reconnection to the Harmony Hub. On the Homatics B25, this first press will be dropped and show a red light indicator. The second button press will register and execute normally once the BLE remote finishes reconnecting.

---

## 6. Official Specification Reference
For the usage definitions of the two pages the Hub sends (Keyboard `07` and Consumer `0C`, in the ranges given at the top) and of all other pages, consult the official standard:
- [USB Implementers Forum: HID Usage Tables v1.5](https://usb.org/document-library/hid-usage-tables-15)


