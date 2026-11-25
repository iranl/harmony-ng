# Harmony Hub - USB Ethernet & Network Configuration Guide

This guide covers setting up and configuring **high-speed USB Ethernet networking** on the **Logitech Harmony Hub** using **HarmonyNG**.

---

## 1. Overview & Architecture

Stock Logitech firmware permanently restricts the Harmony Hub to 2.4 GHz Wi-Fi (`ath0`). The internal Micro-USB port was only configured for USB Device Gadget mode.

**HarmonyNG** introduces a dynamic USB host subsystem and network watchdog:
- **Standard Harmony Hub (USB Host Mode)**:
  - Dynamically switches the USB controller to host mode (`start_usb_eth.sh` unloads the gadget drivers and loads `ehci-hcd`) and registers the dormant `ar7240-ehci.0` platform device with `register_ehci`, a `hub_util` applet, to provide true High-Speed (480 Mbps) USB 2.0 host functionality.
  - Requires an **OTG Micro-USB Ethernet Adapter with Power Cable** to provide both 5V DC power and a 10/100 Mbps RJ45 port.
- **Harmony Hub Pro 2400 (Native Onboard RJ45 & PoE)**:
  - Features a dedicated onboard Atheros GMAC interface (**`eth1`**) wired to a physical RJ45 port with 802.3af Power-over-Ethernet (PoE) hardware.
  - **No USB Host Mode Required**: USB controller remains permanently in **USB Gadget Mode (`046D:C132`)**, allowing full PC USB HID control and console access simultaneously with active wired networking!
  - `network_manager.sh` detects onboard `eth1` automatically, manages routing and DNS transitions, and keeps the stock Logitech HAL notified via `/tmp/eth_conn_pipe`.
- **Automatic Hotplug & Dynamic Failover**: The built-in network monitor (`network_manager.sh`) checks Ethernet PHY carrier state every 3 seconds:
  - **Ethernet Connected (Link Up)**: All traffic immediately migrates to Ethernet (`eth1` on Pro 2400; on a standard Hub the USB adapter's interface, usually `eth2`: `network_manager.sh` never takes `eth0` or `eth1` as a USB adapter). Subnet routes and default gateway are switched to the wired interface.
  - **Ethernet Disconnected (Cable Unplugged / Switch Off)**: Traffic automatically falls back to 2.4 GHz Wi-Fi (`ath0`) without dropping daemon services or activities: within a few seconds with a static address (the monitor notices the lost link at its next 3-second check), and after a new DHCP lease in DHCP mode (the monitor waits up to 10 more seconds for it).
- **Unified Static IP Handover**: When configured with a static IP, the Hub maintains the **exact same IP address** across both Ethernet and Wi-Fi. Upon link handover, gratuitous ARP frames (`arping -U -A`) notify your home router and switches instantly, keeping all open Home Assistant, MQTT, WebSocket, and REST connections alive with zero reconnection delay.
- **MIPS Big-Endian Offload Correction (`eth_fix`)**: The Hub's MIPS 24Kc big-endian architecture causes hardware TX checksum corruption in vendor USB Ethernet drivers (such as Realtek `r8152.ko`). HarmonyNG includes a dedicated userspace helper ([`eth_fix`](../payload/source/hub_util.c)) that automatically disables buggy hardware offloads via `SIOCETHTOOL` ioctl so that the Linux kernel computes 100% compliant TCP/UDP checksums in software.

---

## 2. Tested & Recommended Hardware

Because the Hub requires power through its Micro-USB connector, you need an **OTG Micro-USB Ethernet Adapter with Power Cable** (commonly sold for streaming sticks like Chromecast, Fire TV Stick, and Google Home Mini).

### Verified Working Adapter
- **Tested Model**: [**Micro-USB to RJ45 Ethernet Adapter with USB Power Supply**](https://www.amazon.de/dp/B0G25K13VY) (Amazon.de)
- **Chipset**: Realtek RTL8152B (USB 2.0 10/100 Mbps Fast Ethernet)
- **USB ID**: `0BDA:8152`
- **Driver**: `r8152.ko` + `eth_fix`
- **Features**: Dedicated USB-A male cable for 5V power input, Micro-USB male plug for Hub, RJ45 100Base-TX Ethernet jack with link and activity LEDs.

```
       +----------------------------+
       |   5V USB Power Supply      |
       +--------------+-------------+
                      | USB-A (Power In)
                      v
+---------------------+---------------------+
| Micro-USB OTG Ethernet Adapter            |
| (Realtek RTL8152 / ASIX / SMSC)           |
+----------+--------------------------+-----+
           | RJ45 Ethernet Cable      | Micro-USB Male (Data + Power)
           v                          v
   +---------------+          +---------------+
   | Router/Switch |          |  Harmony Hub  |
   +---------------+          +---------------+
```

### Supported USB Ethernet Chipsets
HarmonyNG bundles kernel drivers for these USB Ethernet chipsets in `/cache/modules/`. The IDs listed are the ones the shipped modules bind; an adapter with another ID of the same family (for example the ASIX AX88179 `0B95:1790`, SMSC LAN9505 `0424:9505` or Davicom DM9620 `0A46:9620`) is not supported by these module builds.

> [!WARNING]
> **The adapter must run at high speed (USB 2.0, 480 Mbps).** A network adapter at full or low speed (a USB 1.1 adapter, or any adapter behind a USB 1.1 hub, as in some OTG cables with a built-in hub) froze the Hub as soon as its driver waited for received data; only a power cycle brought it back. This was seen with an AX88772B behind the full-speed hub of an OTG cable (`0a05:7211`), with modules that were later found to be built for another `struct sk_buff` layout, so it may not be a limit of the bus; until it is tested again with the rebuilt modules, `start_usb_eth.sh` loads no network driver while such a device is on the bus and logs `runs at full or low speed (USB 1.1)`. Check the speed with `cat /sys/bus/usb/devices/*/speed` (`480` is high speed, `12` full speed).
>
> The modules in `payload/modules` reach a Hub only with `build/deploy_hub.py` (it copies each changed module to `/cache/modules`): an OTA update carries no kernel modules, so a Hub updated over the air keeps the modules it had.

| Chipset Family | USB Vendor:Product IDs | Driver Modules | Notes |
| :--- | :--- | :--- | :--- |
| **Realtek RTL8152 / RTL8153** | `0BDA:8152`, `0BDA:8153`, `2357:0601` | `r8152.ko` + `eth_fix` | **Tested & verified** (`0BDA:8152`, `0BDA:8153`). 10/100M Fast Ethernet. Automatic checksum fix applied on load. |
| **ASIX AX88772 / AX88772B / AX88178** | `0B95:7720`, `0B95:772A`, `0B95:772B`, `0B95:1720`, `0B95:1780`, `077B:2226`, `0846:1040` | `usbnet.ko`, `asix.ko` | Common in generic USB-to-LAN adapters. Standard software checksumming. AX88772B (patched in by `build/patches/usb_net/asix-ax88772b.patch`): MAC address read from the EEPROM. It bound and opened on a Hub with an earlier build of these modules; with the shipped build no adapter has been bound and no network traffic tested. The other ASIX IDs are not tested on a Hub. |
| **SMSC / Microchip LAN9500** | `0424:9500`, `0424:EC00` | `usbnet.ko`, `smsc95xx.ko` | Standard embedded Ethernet chipset. Not tested on a Hub. |
| **Davicom DM9601** | `0A46:9601`, `0A46:0268`, `0A46:6688`, `0A46:8515` | `usbnet.ko`, `dm9601.ko` | Common low-cost 10/100 adapters. Not tested on a Hub. |
| **USB CDC-Ethernet (Standard)** | Any USB device with Class `02` Subclass `06` | `usbnet.ko`, `cdc_ether.ko` | Standard Linux USB Communication Device Class. Not tested on a Hub. |
| **Realtek RTL8150** | `0BDA:8150` | `rtl8150.ko` | Legacy 10/100M adapter. The RTL8150 is a USB 1.1 (full speed) device, so `start_usb_eth.sh` loads no driver for it (see the warning above). |

---

## 3. Configuration & Web Console

### Viewing Network Status
Open the Web Console at `http://<YOUR_HUB_IP>:8089` and navigate to the **Network** view:

- **Active Connection**: Displays `Ethernet` or `Dual (Ethernet Active + Wi-Fi Standby)`.
- **Active Interface**: Shows the active network device (e.g., `eth2` or `ath0`).
- **Ethernet Adapter**: Shows connection state, speed (`100Base-TX Link Up`), interface name, and IP address.
- **Wi-Fi Status**: Shows `Connected` with the SSID and the IP address, or `Disconnected`. Which link is on standby is shown in the Active Interface row.

### Configuring IP Assignment (DHCP vs. Static IP)

You can configure IP assignment directly in the Web Console under **Network Configuration**

The settings are stored on the Hub in `/data/harmony/ethernet.conf`, one `KEY=value` per line: `ETH_ENABLED` (`1` or `0`; with `0` the USB port stays in device mode), `ETH_FALLBACK_WIFI` (`1` or `0`), `ETH_MODE` (`dhcp` or `static`), `ETH_IP`, `ETH_NETMASK`, `ETH_GATEWAY` and `ETH_DNS` (one or several servers, separated by commas or spaces), and `USB_WIFI_SETUP` (`1` or `0`, Wi-Fi setup over USB, section 5). Without the file the Hub uses Ethernet with DHCP, falls back to Wi-Fi, and offers the USB setup.

> [!TIP]
> **Static IP Recommended**: Configuring a **Static IP** matching your Hub's Wi-Fi IP address provides zero-drop failover. When you plug in or unplug the Ethernet cable, the Hub switches interfaces and announces its MAC via Gratuitous ARP without waiting for a DHCP server lease.

---

## 4. REST API Endpoints

The network subsystem exposes full diagnostic and configuration REST endpoints:

### GET `/api/network-status`
Returns live status for both Ethernet and Wi-Fi interfaces, USB host mode, and active routing

### POST `/api/ethernet-save` (or `/ethernet`)
Saves the Ethernet settings and, with `"apply": "apply"`, applies them without a reboot. Every save
replaces the whole file: send all fields (API.md 8.3)

The fields are `eth_enabled`, `eth_fallback_wifi`, `eth_mode` (`dhcp` or `static`), `eth_ip`,
`eth_netmask`, `eth_gateway`, `eth_dns` and `usb_wifi_setup` (the only one that keeps its stored
value when left out; the web UI has no control for it yet). In `static` mode an address that
cannot work is refused with `400` and nothing is saved.

### Wi-Fi: GET `/api/wifi-scan`, POST `/api/wifi-test`, POST `/api/wifi-save` (or `/wifi`)
Scan for networks, test a network and its password, and save the Wi-Fi settings (API.md 8.2)

---

## 5. Setting the Wi-Fi over USB (Hub on no network)

When the Hub is on no network (a new router, a changed Wi-Fi password, a move) and you cannot
reach the web UI, set the Wi-Fi from a computer over USB, as the stock firmware allowed:

1. Connect the Hub's micro-USB port to the computer with a data cable, in place of its power
   supply (or Ethernet adapter): the Hub starts with the computer as its power source. The
   computer must be connected while the Hub starts (see Limits below).
2. Wait about a minute for the Hub to start.
3. Run, with Python 3.13 or newer:

   ```bash
   python tools/set_wifi_usb.py --status
   python tools/set_wifi_usb.py --ssid "YourNetworkName"     # asks for the password
   python tools/set_wifi_usb.py --ssid "Guest" --encryption NONE
   ```

   The Hub tries the network first, for up to about 15 s. When it works, the settings are saved
   and the tool prints the Hub's new address (the web UI is then at `http://<address>:8089/`).
   When it does not, the Hub keeps its previous Wi-Fi settings and the tool prints the reason
   (wrong password, network not found, a value the Hub cannot use). The password can also come
   from the environment variable `HARMONY_WIFI_PASS`. For 2 minutes after such an attempt
   `--status` shows it as the last attempt and the Hub as back on its previous network. Right
   after Windows set the USB device up again, the Hub may not answer for about a minute: the
   tool then opens the device again and retries for up to 60 s.
4. Put the power supply (or the Ethernet adapter) back: the Hub starts again on its network.

On Linux the user needs read and write access to the Hub's USB device node (or run the tool with
`sudo`; the udev rule is in [`FIRST_INSTALL_HUB.md`](FIRST_INSTALL_HUB.md), Prerequisites);
Windows and macOS need no driver. `python tools/root_hub_usb.py --skip-fw-check
--wifi-only --no-reboot --wifi-ssid "YourNetworkName"` does the same, with extra LAN checks.

Only the Wi-Fi network can be read and set this way: the password is never sent back, and every
other request of the stock USB protocol (SSH upload, boot trigger, factory reset, firmware) is
refused. The same works in the Hub's emergency boot mode (after more than five failed boots).

Like the stock firmware, the Hub answers any computer on its USB port without a login: it can
read the network name and address and set another network (each failed attempt takes the Hub off
Wi-Fi for about 12 s). To turn the USB setup off, clear the "Wi-Fi setup over USB" switch in
the web UI (Network page; in the legacy web UI it sits in the Ethernet & IP settings and is
saved with them), send `"usb_wifi_setup": false` with `POST /api/ethernet-save` (API.md 8.3),
or put `USB_WIFI_SETUP=0` in
`/data/harmony/ethernet.conf`. Requests are then refused at once; from the next start of the Hub
services or boot `hub_usbwifi` is not started and the boot does not wait for a computer (so
Ethernet over USB always starts). With it off and no Wi-Fi network, the way back is an Ethernet
adapter (with Ethernet over USB on) or a factory reset.

### Limits
- **USB port mode**: the Hub's one USB port is either a USB device (this setup) or a USB host
  (Ethernet over USB), and it can switch to host mode only, not back, until the next restart. At
  boot, with Ethernet over USB on (the default), the Hub waits for a computer on the port: the
  sign is `usbhid` connecting to `hub_usbwifi` a second time, which it does when a computer sets
  the USB device up. Then the port stays a USB device for that boot and Ethernet over USB is not
  started (`/tmp/harmony-network.log` says so). The wait is 6 s, or 20 s once the USB device
  controller has counted interrupts (`/proc/udc`); interrupts alone never keep the port a device,
  so an adapter or a charger always ends in host mode as before, at most 20 s later. A computer
  plugged in after that is not seen until the Hub is restarted with the computer connected. With
  Ethernet turned off in the settings, and on the Harmony Pro 2400 (onboard Ethernet), the port is
  always a USB device; with `USB_WIFI_SETUP=0` the boot does not wait at all. The mode decided is
  kept in `/tmp/harmony_usb_port_mode`: a restart of the Hub services starts `usbhid` again only
  in device mode.
- **2.4 GHz WPA/WPA2 pass phrase or open networks only** (`WPA2-PSK`, `WPA-PSK`, `NONE`): WEP,
  enterprise (`WPA-EAP`, `WPA2-ENTERPRISE`) and any other encryption value are refused.
  The network is saved with `scan_ssid=1`, so hidden networks work too.
- The Hub must be close enough to the access point: the test needs the connection to come up
  within about 12 s. The address comes from DHCP (or the static address in the Ethernet
  settings) and can take a few more seconds.

### How it works
The stock firmware's `/usr/bin/usbhid` (still started by the stock boot script) presents the Hub
as the USB HID device `046D:C129` (`046D:C132` on the Pro) and passes the 64-byte reports to a
program on `127.0.0.1:16716`: stock that was `hal`, which harmony-ng does not run. `init.sh`
starts `hub_usbwifi` (a `hub_util` applet, `payload/source/usb_wifi.c`) there instead. It speaks
the stock file protocol for one virtual file, `/sys/wifi/connect`:

- the stream starts with one connection type byte from `usbhid` (`0x06`), then 64-byte frames
  both ways;
- a request is `FF <command> <sequence> <parameter count> <parameters>`: open `0x01` (path, mode
  `W` or `R`, size), write `0x03` (handle, frame count, then the data frames
  `<flags> <length> <data>`), read `0x04` (handle, frames wanted), close `0x07`, ping `0x00`;
- an answer is `FF <command> <sequence | 0x80> ...`; a refusal is `FF <command> FF 01 01 <code>`
  (byte 2 is `0xFF`): `0x0B` for any other file or command, `0x0C` for a request it cannot parse,
  `0x04` for an unknown handle;
- writing `ssid,<name>\npassword,<password>\nencryption,<WPA2-PSK|WPA-PSK|NONE>\n` sets the
  network; the write is answered when the attempt has ended, always with success once the request
  was understood, and the result is in the status;
- reading returns `key,value` lines: `ssid`, `encryption`, `password` (always empty),
  `connect_status` (`connected`, `inactive`, `connecting`, or `failed` for 2 minutes after a failed
  USB attempt), `ip_address`, `error_code` (`none` or the reason), `last_attempt`, and
  `firmware,harmony-ng`.

The Wi-Fi test and save are the same code as the web UI's (`payload/source/wifi_config.c`), and one
test runs at a time across the web UI and USB. A Hub with no network stored runs no
`wpa_supplicant` (it cannot start without its file): the test starts it on the network being
tried. On failure each Wi-Fi file gets its previous content back, or is removed when there was
none. While a test runs, copies of the previous files and a mark are kept on flash next to them
(`/etc/wpa_supplicant*.pretest`): a test cut short by a kill or a power cut is undone when
`hub_daemon` or `hub_usbwifi` starts next. Each attempt is logged in `/tmp/harmony-init.log`
(network name and result, never the password). On the Hub, `/data/harmony/bin/hub_usbwifi --status`
prints what the computer reads, and `/tmp/harmony_usbwifi.state` counts the connections from
`usbhid` (it connects again each time a computer configures the device).

---

## 6. Troubleshooting & Diagnostics

### Checking Interface Status via SSH
```bash
# Check routing table and active interface
route -n

# Check physical link carrier (1 = cable connected, 0 = unplugged)
cat /sys/class/net/eth2/carrier

# Inspect kernel messages for USB and Ethernet adapter
dmesg | grep -E 'ehci|r8152|usbnet|asix'

# View network manager watchdog logs
cat /tmp/harmony-network.log

# Show the Ethernet settings in use and the active link (interface:state:address)
/data/harmony/network_manager.sh status

# Apply /data/harmony/ethernet.conf after a change made by hand
/data/harmony/network_manager.sh apply
```

### Offload Check
If connecting over Ethernet shows successful TCP handshakes but hangs on data reception, ensure `eth_fix` has executed:
```bash
/data/harmony/bin/eth_fix eth2
# Expected output: [+] eth2: TX checksum offload disabled
```
This is automatically handled by `start_usb_eth.sh` and `network_manager.sh`.
