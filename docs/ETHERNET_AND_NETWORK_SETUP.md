# Harmony Hub - USB Ethernet & Network Configuration Guide

This guide covers setting up and configuring **high-speed USB Ethernet networking** on the **Logitech Harmony Hub** using **HarmonyNG**.

---

## 1. Overview & Architecture

Stock Logitech firmware permanently restricts the Harmony Hub to 2.4 GHz Wi-Fi (`ath0`). The internal Micro-USB port was only configured for USB Device Gadget mode.

**HarmonyNG** introduces a dynamic USB host subsystem and network watchdog:
- **Standard Harmony Hub (USB Host Mode)**:
  - Dynamically reconfigures the Atheros AR9344 SoC PHY register (`set_phy_host`) and attaches the dormant `ar7240-ehci.0` platform driver to provide true High-Speed (480 Mbps) USB 2.0 host functionality.
  - Requires an **OTG Micro-USB Ethernet Adapter with Power Cable** to provide both 5V DC power and a 10/100 Mbps RJ45 port.
- **Harmony Hub Pro 2400 (Native Onboard RJ45 & PoE)**:
  - Features a dedicated onboard Atheros GMAC interface (**`eth1`**) wired to a physical RJ45 port with 802.3af Power-over-Ethernet (PoE) hardware.
  - **No USB Host Mode Required**: USB controller remains permanently in **USB Gadget Mode (`046D:C132`)**, allowing full PC USB HID control and console access simultaneously with active wired networking!
  - `network_manager.sh` detects onboard `eth1` automatically, manages routing and DNS transitions, and keeps the stock Logitech HAL notified via `/tmp/eth_conn_pipe`.
- **Automatic Hotplug & Dynamic Failover**: The built-in network monitor (`network_manager.sh`) checks Ethernet PHY carrier state every 3 seconds:
  - **Ethernet Connected (Link Up)**: All traffic immediately migrates to Ethernet (`eth1` on Pro 2400, or `eth2` / `eth0` on standard Hub). Subnet routes and default gateway are switched to the wired interface.
  - **Ethernet Disconnected (Cable Unplugged / Switch Off)**: Traffic automatically falls back to 2.4 GHz Wi-Fi (`ath0`) within 1–2 seconds without dropping daemon services or activities.
- **Unified Static IP Handover**: When configured with a static IP, the Hub maintains the **exact same IP address** across both Ethernet and Wi-Fi. Upon link handover, gratuitous ARP frames (`arping -U -A`) notify your home router and switches instantly, keeping all open Home Assistant, MQTT, WebSocket, and REST connections alive with zero reconnection delay.
- **MIPS Big-Endian Offload Correction (`eth_fix`)**: The Hub's MIPS 24Kc big-endian architecture causes hardware TX checksum corruption in vendor USB Ethernet drivers (such as Realtek `r8152.ko`). HarmonyNG includes a dedicated userspace helper ([`eth_fix`](../payload/source/eth_fix.c)) that automatically disables buggy hardware offloads via `SIOCETHTOOL` ioctl so that the Linux kernel computes 100% compliant TCP/UDP checksums in software.

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
HarmonyNG bundles kernel drivers for all common USB Ethernet chipsets in `/cache/modules/`:

| Chipset Family | USB Vendor:Product IDs | Driver Modules | Notes |
| :--- | :--- | :--- | :--- |
| **Realtek RTL8152 / RTL8153** | `0BDA:8152`, `0BDA:8153`, `2357:0601` | `r8152.ko` + `eth_fix` | **Tested & verified.** 10/100M Fast Ethernet. Automatic checksum fix applied on load. |
| **ASIX AX88179 / AX88772 / AX88178** | `0B95:7720`, `0B95:772B`, `0B95:1790`, `077B:2226` | `usbnet.ko`, `asix.ko` | Very common in generic USB-to-LAN adapters. Standard software checksumming. |
| **SMSC / Microchip LAN95xx** | `0424:9500`, `0424:9505`, `0424:EC00` | `usbnet.ko`, `smsc95xx.ko` | Standard embedded Ethernet chipset. |
| **Davicom DM9601 / DM9620** | `0A46:9601`, `0A46:9620`, `0A46:0268` | `usbnet.ko`, `dm9601.ko` | Common low-cost 10/100 adapters. |
| **USB CDC-Ethernet (Standard)** | Any USB device with Class `02` Subclass `06` | `usbnet.ko`, `cdc_ether.ko` | Standard Linux USB Communication Device Class. |
| **Realtek RTL8150** | `0BDA:8150` | `rtl8150.ko` | Legacy 10/100M USB 1.1/2.0 adapter. |

---

## 3. Configuration & Web Console

### Viewing Network Status
Open the Web Console at `http://<YOUR_HUB_IP>:8089` and navigate to the **Network** view:

- **Active Connection**: Displays `Ethernet` or `Dual (Ethernet Active + Wi-Fi Standby)`.
- **Active Interface**: Shows the active network device (e.g., `eth2` or `ath0`).
- **Ethernet Adapter**: Shows connection state, speed (`100Base-TX Link Up`), interface name, and IP address.
- **Wi-Fi Status**: Shows SSID, signal, and standby state.

### Configuring IP Assignment (DHCP vs. Static IP)

You can configure IP assignment directly in the Web Console under **Network Configuration**

> [!TIP]
> **Static IP Recommended**: Configuring a **Static IP** matching your Hub's Wi-Fi IP address provides zero-drop failover. When you plug in or unplug the Ethernet cable, the Hub switches interfaces and announces its MAC via Gratuitous ARP without waiting for a DHCP server lease.

---

## 4. REST API Endpoints

The network subsystem exposes full diagnostic and configuration REST endpoints:

### GET `/api/network-status`
Returns live status for both Ethernet and Wi-Fi interfaces, USB host mode, and active routing

### POST `/api/network/config`
Updates IP assignment settings without requiring a full Hub reboot

---

## 5. Troubleshooting & Diagnostics

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
```

### Offload Check
If connecting over Ethernet shows successful TCP handshakes but hangs on data reception, ensure `eth_fix` has executed:
```bash
/data/harmony/bin/hub_eth_fix eth2
# Expected output: [+] eth2: TX checksum offload disabled
```
This is automatically handled by `start_usb_eth.sh` and `network_manager.sh`.
