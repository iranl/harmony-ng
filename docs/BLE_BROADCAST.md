# BLE Broadcast Commands

Some devices take commands as BLE advertisements rather than over a connection: the phone app or remote broadcasts a packet, and the device acts on any packet carrying an ID it has paired with. Ceiling fans and fan-lights often work this way. Harmony Hub controls these devices by rebroadcasting packets captured from the original app.

## Command format

A broadcast command is a device command whose keyCode is:

```text
BLEADV:<hex>
```

`<hex>` is legacy advertising data: a sequence of AD structures (length byte, type byte, data), at most 31 bytes, filling the payload exactly. For example, flags plus a complete list of six 32-bit service UUIDs:

```text
BLEADV:02011A19054E6F720E0438064C2E561510D42DEB8C15D3B51804030201
       └flags┘└─ 0x19 bytes, type 0x05: UUIDs, each little-endian ─┘
```

When the command runs, `hub_daemon` checks the payload and sends `adv <hex>` to `hub_btstack` on `/tmp/hub_btstack.sock`. `hub_btstack` broadcasts it as non-connectable advertising (`ADV_NONCONN_IND`) on all three advertising channels at a 100 ms interval for 1.2 s. Broadcasts queue (up to 8), so each packet in an activity sequence goes out in full. A held button sends one broadcast; release and repeat events send nothing.

`adv <hex> <ms>` sets a different duration, from 200 ms to 10 s. Device commands always use the default.

## Adding a device

In Devices Studio, choose **Add Device** and set **Device Type** to **BLE broadcast**.

Open the device and add each command in the **BLE Broadcast Command** card. The card accepts either a `uuids=` line from `ble_scan` or raw advertising data hex.

## Capturing packets (macOS)

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

## Mantra Mistral

The mAntra Mistral ceiling fan-light pairs with the mAntra lighting app over BLE advertisements; its physical remote uses a separate RF radio and cannot be captured this way. Each app command is six 32-bit service UUIDs:

| UUID | Content |
|:---|:---|
| 1 | `0E726F4E`: constant across all captures, probably the app's pairing ID |
| 2 | `<flag> 06 <counter lo> <counter hi>`; the flag byte is XORed with the counter's high byte |
| 3–5 | 12 encrypted bytes; differ for every packet |
| 6 | `01020304`: constant |

The fan acts on replayed packets regardless of their counter, and each command sets a state (light on and light off are separate packets), so sending a command whose state is already set changes nothing. If UUID 1 is the app's pairing ID, captured packets only control a fan paired with the app installation that produced them.
