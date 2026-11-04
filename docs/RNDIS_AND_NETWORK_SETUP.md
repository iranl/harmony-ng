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

### Windows 11: device stuck as "USB Serial Device (COMx)"

On current Windows 11 builds the remote can enumerate as `USB\VID_0525&PID_A4A2` and bind to the in-box serial driver (it shows up under **Ports (COM & LPT)** as `USB Serial Device (COMx)`). The steps above then do not work: the in-box RNDIS driver packages (`rndiscmp.inf`, `netrndis.inf`, `wceisvista.inf`) all declare `ExcludeFromSelect=*` in `[ControlFlags]`, so no RNDIS model appears in the driver list, not even through **Have Disk...**

A working alternative is to pass the device to WSL2 with [usbipd-win](https://github.com/dorssel/usbipd-win), where the Linux kernel binds it with `cdc_ether`/`cdc_subset`:

```powershell
usbipd list
usbipd bind --busid <BUSID>
usbipd attach --wsl --busid <BUSID>
```

```bash
ip -br link
sudo ip addr add 192.168.2.2/24 dev <interface>
sudo ip link set <interface> up
```

If you rooted the remote with `setup_remote.py` on Windows, its SSH key is in `%USERPROFILE%\.ssh` on the Windows side. Copy it into WSL first: WSL shows files under `/mnt/c` with open permissions, and ssh refuses a private key that others can read.

```bash
mkdir -p ~/.ssh
cp /mnt/c/Users/<you>/.ssh/harmony_owner_ed25519* ~/.ssh/
chmod 600 ~/.ssh/harmony_owner_ed25519
ssh -i ~/.ssh/harmony_owner_ed25519 root@192.168.2.1
```

`usbipd bind` needs an elevated console once. Run the SSH based steps (backup, `deploy_elite.py`) from inside WSL. When the remote reboots it re-enumerates, so run `usbipd attach` again.

---

## 2. Static IP Configuration

The USB Ethernet gadget uses fixed IP subnets on each device:

| Device | Device USB IP | Recommended Host PC IP | Subnet Mask |
| :--- | :--- | :--- | :--- |
| **Harmony Hub** (Stock / Recovery) | `192.168.1.1` | `192.168.1.2` | `255.255.255.0` |
| **Harmony Elite 950 / Ultimate / Touch** | `192.168.2.1` | `192.168.2.2` | `255.255.255.0` |

Do not set a default gateway on the USB adapter: the device is only reachable on its own subnet, and a gateway there can take over the PC's default route.

> [!NOTE]
> Tested on a Harmony Ultimate (2013, skin 100) with the same tooling and network setup as the Elite. Harmony Ultimate Home and Harmony Touch **SHOULD** also work, but are untested, try at your own risk.

### Setting Host Static IP in Windows:
1. Press `Win + R`, type `ncpa.cpl`, and press **Enter** to open Network Connections.
2. Right-click the **RNDIS / USB Ethernet adapter** and choose **Properties**.
3. Double-click **Internet Protocol Version 4 (TCP/IPv4)**.
4. Select **Use the following IP address**:
   - **IP address**: `192.168.2.2` (for Elite) or `192.168.1.2` (for Hub)
   - **Subnet mask**: `255.255.255.0`
   - **Default gateway**: leave empty
5. Leave DNS blank and click **OK**.

### Setting Host Static IP in Linux / WSL2:
```bash
# Identify the USB Ethernet interface (e.g. usb0 or eth1)
ip link show

# Assign static IP to the host interface
sudo ip addr add 192.168.2.2/24 dev usb0
sudo ip link set usb0 up
```

### Setting Host Static IP in macOS:
macOS needs no driver. When the remote is connected over USB, a network service named **RNDIS/Ethernet Gadget** appears. Without a static address, macOS gives it only a self-assigned `169.254.x.x` address, and `192.168.2.1` is unreachable. Assign the address without a router, so your normal internet route is unchanged:
```bash
# Confirm the service name
networksetup -listallnetworkservices

# Assign static IP (Elite shown; see the table above for the Hub's subnet)
sudo networksetup -setmanual "RNDIS/Ethernet Gadget" 192.168.2.2 255.255.255.0

# Revert to automatic addressing later
sudo networksetup -setdhcp "RNDIS/Ethernet Gadget"
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
