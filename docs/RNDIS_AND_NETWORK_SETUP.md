# RNDIS Driver Installation & Network Setup Guide

Both the **Logitech Harmony Hub** and **Harmony Elite 950** handheld remote communicate with host computers over USB using the **RNDIS** (Remote Network Driver Interface Specification) protocol, presenting themselves as virtual Ethernet-over-USB network cards.

---

## 1. Windows RNDIS Driver Installation (Windows 10 & 11)

Windows 10/11 frequently fails to automatically bind the correct driver, showing the plugged-in Hub or Elite as `Unknown Device`, `USB Serial Device`, or an exclamation mark in Device Manager.

### Step-by-Step Driver Installation:
1. Connect the Hub or Elite cradle to your PC using a known **data-capable** Micro-USB cable.
2. Press `Win + X` and select **Device Manager**.
3. Locate the device under **Other devices** (usually named `RNDIS`, `CDC Composite Gadget`, or `Unknown device`).
4. Right-click the device and select **Update driver**.
5. Choose **Browse my computer for drivers**.
6. Click **Let me pick from a list of available drivers on my computer**.
7. In the hardware category list, select **Network adapters** and click **Next**.
8. In the manufacturer column on the left:
   - Select **Microsoft** (or **Acer Incorporated** if Microsoft is not listed).
9. In the model column on the right:
   - Select **Remote NDIS Compatible Device** (or **USB Ethernet/RNDIS Gadget**).
10. Click **Next** and click **Yes** on the Update Driver Warning dialog.
11. Device Manager will now show a new network adapter under Network Adapters.

---

## 2. Static IP Configuration

The USB Ethernet gadget uses fixed IP subnets on each device:

| Device | Device USB IP | Recommended Host PC IP | Subnet Mask | Gateway |
| :--- | :--- | :--- | :--- | :--- |
| **Harmony Hub** (Stock / Recovery) | `192.168.1.1` | `192.168.1.2` | `255.255.255.0` | `192.168.1.1` |
| **Harmony Elite 950 Remote** | `192.168.2.1` | `192.168.2.2` | `255.255.255.0` | `192.168.2.1` |

### Setting Host Static IP in Windows:
1. Press `Win + R`, type `ncpa.cpl`, and press **Enter** to open Network Connections.
2. Right-click the **RNDIS / USB Ethernet adapter** and choose **Properties**.
3. Double-click **Internet Protocol Version 4 (TCP/IPv4)**.
4. Select **Use the following IP address**:
   - **IP address**: `192.168.2.2` (for Elite) or `192.168.1.2` (for Hub)
   - **Subnet mask**: `255.255.255.0`
   - **Default gateway**: `192.168.2.1` (for Elite) or `192.168.1.1` (for Hub)
5. Leave DNS blank and click **OK**.

### Setting Host Static IP in Linux / WSL2:
```bash
# Identify the USB Ethernet interface (e.g. usb0 or eth1)
ip link show

# Assign static IP to the host interface
sudo ip addr add 192.168.2.2/24 dev usb0
sudo ip link set usb0 up
```

---

## 3. Verifying Connectivity

### Test Ping:
```bash
# Verify connection to Elite Remote:
ping 192.168.2.1

# Verify connection to Harmony Hub:
ping 192.168.1.1
```

### SSH Access:
Both devices run a lightweight Dropbear SSH server:
```bash
# SSH into Harmony Elite:
ssh root@192.168.2.1

# SSH into Harmony Hub:
ssh root@192.168.1.1
```
*(The private SSH key is stored in your user `~/.ssh/` directory).*
