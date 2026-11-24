# Windows RNDIS Ethernet Driver (WHQL Signed)

This package contains the Microsoft WHQL-signed RNDIS template driver for USB Ethernet gadgets (`USB\VID_0525&PID_A4A2` and `USB\VID_0502&PID_3210`).

## Background
Windows 10 and 11 often fail to bind an RNDIS network adapter to USB Ethernet gadgets (including the Logitech Harmony Elite / Ultimate handheld remote during root setup), mistakenly binding the device as a generic `USB Serial Device (COMx)` under `Ports (COM & LPT)`.

Furthermore, Windows 11 sets `ExcludeFromSelect=*` in its built-in `rndiscmp.inf` and `netrndis.inf`, hiding RNDIS drivers from the manual driver picker in Device Manager.

This package provides:
- **`rndis.inf`**: Device setup information file matching `USB\VID_0525&PID_A4A2` for x86, x64 (`NTamd64`) and Itanium (`NTia64`). It references the native Windows system binaries `usb8023.sys` and `rndismp.sys` already present in `C:\Windows\System32\drivers\`. It carries the same `ExcludeFromSelect = *` line as the built-in files; installing it with `pnputil` or the setup tool, as described below, does not go through the manual driver picker.
- **`RNDIS.cat`**: Security catalog digitally signed by **Microsoft Windows Hardware Compatibility Publisher** (WHQL), allowing driver installation without disabling driver signature enforcement.

## Automated Installation
Run the automated PowerShell setup tool from the repository root:
```powershell
powershell -ExecutionPolicy Bypass -File tools/setup_remote_driver.ps1
```
The tool asks for administrator rights itself (UAC prompt), installs the driver, restarts or removes a remote that Windows bound as a serial port so that it is matched again, gives the remote's network adapter a static address in `192.168.2.0/24` (no gateway; `192.168.2.2`, or the next free one when another adapter, hidden ones included, already has it) and tests ports 8088 and 22 of the remote at `192.168.2.1`. Add `-NonInteractive` to skip the key press at the end.

## Manual Installation (Elevated Command Prompt or PowerShell)
```cmd
pnputil /add-driver rndis.inf /install
pnputil /restart-device /deviceid "USB\VID_0525&PID_A4A2"
```
