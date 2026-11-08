# RNDIS Driver Installation & Network Setup Guide

Both the **Logitech Harmony Hub** and the **Harmony touchscreen remotes** (Elite, Ultimate, Touch) communicate with host computers over USB using the **RNDIS** (Remote Network Driver Interface Specification) protocol, presenting themselves as virtual Ethernet-over-USB network cards.

---

## 1. Windows RNDIS Driver Installation (Windows 10 & 11)

Windows 10/11 frequently fails to automatically bind the correct driver, either showing the plugged-in Hub/remote as `Unknown Device` or mistakenly binding it to `usbser.inf` as a `USB Serial Device (COMx)` under **Ports (COM & LPT)**. Additionally, Windows 11 sets `ExcludeFromSelect=*` on its in-box RNDIS driver templates, preventing manual selection in Device Manager.

### Option A: Automated PowerShell Setup (Recommended)

HarmonyNG includes a Microsoft WHQL-signed RNDIS template driver package (`drivers/windows/rndis/`) and an automated, self-elevating setup script.

Run from an elevated or standard PowerShell console (it will prompt for UAC elevation automatically):

```powershell
powershell -ExecutionPolicy Bypass -File tools/setup_remote_driver.ps1
```

The script will automatically:
1. Install the signed RNDIS driver package into the Windows DriverStore via `pnputil`.
2. Re-bind `USB\VID_0525&PID_A4A2` (unbinds `usbser.sys` and binds `rndismp.sys`).
3. Locate the new `USB Ethernet/RNDIS Gadget` network adapter.
4. Assign static IP `192.168.2.2/24` (leaving the default gateway empty so internet routing is not affected).
5. Verify connectivity to `192.168.2.1`.

> [!TIP]
> If you run `python remote/setup_remote.py`, it will detect any missing driver or serial port binding on Windows and automatically launch this setup tool for you.

---

### Option B: Manual Device Manager Driver Selection

If your Windows build allows manual selection:
1. Connect the Hub or remote to your PC using a known **data-capable** Micro-USB cable.
2. Press `Win + X` and select **Device Manager**.
3. Locate the device under **Other devices** (or under **Ports (COM & LPT)** if bound as serial).
4. Right-click the device and select **Update driver**.
5. Choose **Browse my computer for drivers** -> **Let me pick from a list of available drivers on my computer**.
6. Select **Network adapters** and click **Next**.
7. In the manufacturer column on the left:
   - Select **Acer Incorporated** (or **Microsoft**).
8. In the model column on the right:
   - Select **USB Ethernet/RNDIS Gadget** (or **Remote NDIS Compatible Device**).
9. Click **Next** and click **Yes** on the Update Driver Warning dialog.

---

### Option C: WSL2 & usbipd-win (Alternative Fallback)

If you prefer to route the USB device directly into a Linux environment via WSL2:

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

`usbipd bind` needs an elevated console once. Run the SSH based steps (backup, `deploy_remote.py`) from inside WSL. When the remote reboots it re-enumerates, so run `usbipd attach` again.

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
   - **IP address**: `192.168.2.2` (for the remote) or `192.168.1.2` (for Hub)
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

# Assign static IP (remote shown; see the table above for the Hub's subnet)
sudo networksetup -setmanual "RNDIS/Ethernet Gadget" 192.168.2.2 255.255.255.0

# Revert to automatic addressing later
sudo networksetup -setdhcp "RNDIS/Ethernet Gadget"
```

---

## 3. Verifying Connectivity

### Test Ping:
```bash
# Verify connection to the remote:
ping 192.168.2.1

# Verify connection to Harmony Hub:
ping 192.168.1.1
```

### SSH Access:
Both devices run a lightweight Dropbear SSH server:
```bash
# SSH into the remote:
ssh root@192.168.2.1

# SSH into Harmony Hub:
ssh root@192.168.1.1
```
*(The private SSH key is stored in your user `~/.ssh/` directory).*
