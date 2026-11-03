# BLE Broadcast Commands

Some devices take commands as BLE advertisements rather than over a connection: the phone app or remote broadcasts a packet, and the device acts on any packet carrying an id it has paired with. Ceiling fans and fan-lights often work this way. Harmony Hub controls these devices in two ways:

- **Mantra fans:** the Hub builds each command itself from the fan's id, and can pair the fan.
- **Other devices:** the Hub rebroadcasts packets captured from the device's own app.

Both are sent by `hub_btstack` as non-connectable advertising (`ADV_NONCONN_IND`) on all three advertising channels at a 100 ms interval, for 1.2 s per command. Broadcasts queue (up to 8), so each packet in an activity sequence goes out in full. A held button sends one broadcast; release and repeat events send nothing.

## Mantra fans

Mantra ceiling fans and fan-lights (mAntra lighting app by HuiZhou NewEnergy) use the protocol of the `mantra` codec in [ha-ble-adv](https://github.com/NicoIIT/ha-ble-adv) by @NicoIIT (MIT License). `payload/source/mantra_codec.h` implements it, with ha-ble-adv's license notice. The fans' physical remotes use a separate RF radio and are not involved.

### Adding a fan

In Devices Studio, choose **Add Device** and set **Device Type** to **Mantra fan or light (Bluetooth)**. The device gets a random fan id and a starting set of commands. Then give the fan an id in one of two ways, from the device's **Mantra Fan** card:

- **Pair:** switch the fan off at the wall for 5 s and back on, then press **Pair** within 30 s. The Hub broadcasts the app's pairing packets with the device's id, and the light blinks 3 times when the fan accepts it. The fan may then ignore the mAntra app until the app pairs again.
- **Listen:** press **Listen**, then use any control in the mAntra lighting app near the Hub. The Hub decodes the fan id from the app's broadcast; choose it to save it. The fan keeps obeying the app as well.

The id can also be typed in as 4 hex digits. **Mantra Model** selects the codec variant: fan and light (most models), or light only.

### Commands

A Mantra command's keyCode is `MANTRA:<action>`:

| Action | Effect |
|:---|:---|
| `LIGHT_ON`, `LIGHT_OFF` | Light on or off |
| `BRIGHTNESS:<1-100>` | Brightness in percent, keeping the colour temperature |
| `CT:<0-100>` | Colour temperature, 0 cool to 100 warm, keeping the brightness |
| `FAN_ON`, `FAN_OFF` | Fan on or off |
| `SPEED:<0-31>` | Fan speed |
| `FORWARD`, `REVERSE` | Fan direction (summer, winter) |
| `BREEZE`, `SLEEP` | Fan modes |
| `TIMER_1H`, `TIMER_2H`, `TIMER_4H`, `TIMER_8H` | Off timer |
| `ALL_OFF` | Light and fan off |
| `RAW:<cmd>:<param>:<a0>:<a1>:<a2>:<a3>:<a4>` | Any codec command, in hex |

The light-only variant supports `LIGHT_ON`, `LIGHT_OFF`, `BRIGHTNESS`, `CT` and `RAW`. The fan sets brightness and colour temperature in one packet, so `hub_daemon` keeps each fan's last values (100 % and 50 after a restart). The **Add Fan Command** card builds these keyCodes.

### Packet format

Each command is an 18-byte body:

```text
72 0E <counter:2> 06 <cmd> 12 34 56 78 <fan id:2> <param> <arg0..arg4>
```

Bytes from offset 5 are whitened with a 16-bit LFSR seeded by the counter, and the Hub uses a new counter for every broadcast. The app sends the body as manufacturer data (company `0x6F4E`) and, from iOS, as a list of six 32-bit service UUIDs `4E 6F <body> 04 03 02 01`; the Hub sends the UUID-list form. The light-only variant uses `72 0F`.

Pairing uses the same whitening with header `69 63` and prefix `5F 48`, and is not part of ha-ble-adv; it follows the app's broadcasts. The app sends a search (`cmd F4`), then a bind (`cmd 05`) carrying `00 01 <fan id> <fan id>`; the fan obeys the bound id from then on.

`POST /api/mantra-pair` and `/api/mantra-learn` are described in [API.md](API.md#67-mantra-fans).

## Captured packets

A broadcast command for any other device has the keyCode:

```text
BLEADV:<hex>
```

`<hex>` is legacy advertising data: a sequence of AD structures (length byte, type byte, data), at most 31 bytes, filling the payload exactly. For example, flags plus a complete list of 16-bit service UUIDs:

```text
BLEADV:0201060303AAFE
       └flags┘└ UUIDs ┘
```

When the command runs, `hub_daemon` checks the payload and sends `adv <hex>` to `hub_btstack` on `/tmp/hub_btstack.sock`. `adv <hex> <ms>` sets a different duration, from 200 ms to 10 s; device commands use the default.

To add such a device, choose **Add Device**, set **Device Type** to **Other Bluetooth broadcast device (advanced)**, and add each command in the **BLE Broadcast Command** card. The card accepts a `uuids=` line from `ble_scan` or raw advertising data hex.

### Capturing packets (macOS)

`tools/ble_capture` holds two CoreBluetooth command-line tools:

```sh
cd tools/ble_capture
swiftc -O ble_scan.swift -o ble_scan
swiftc -O ble_replay.swift -o ble_replay
```

1. Run `./ble_scan -r -60 | tee scan.log` with the phone about 30 cm from the Mac. macOS asks for Bluetooth access for the terminal app the first time.
2. Wait until no new `NEW` lines appear, then for each control type a label, press Enter, and use the control in the app. The packets that appear after each marker belong to that control.
3. Check a packet with `./ble_replay <uuids>` before adding it. Replay also shows whether the device acts on stale packets and whether a command sets a state or toggles it.

An iOS app can only advertise a local name and service UUIDs, so packets from iOS apps show up as `uuids=` lists.
