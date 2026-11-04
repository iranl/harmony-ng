# Windows RNDIS Ethernet Driver (WHQL Signed)

This package contains the Microsoft WHQL-signed RNDIS template driver for USB Ethernet gadgets (`USB\VID_0525&PID_A4A2` and `USB\VID_0502&PID_3210`).

## Background
Windows 10 and 11 often fail to bind an RNDIS network adapter to USB Ethernet gadgets (including the Logitech Harmony Elite / Ultimate handheld remote during root setup), mistakenly binding the device as a generic `USB Serial Device (COMx)` under `Ports (COM & LPT)`.

Furthermore, Windows 11 sets `ExcludeFromSelect=*` in its built-in `rndiscmp.inf` and `netrndis.inf`, hiding RNDIS drivers from the manual driver picker in Device Manager.

This package provides:
- **`rndis.inf`**: Device setup information file matching `USB\VID_0525&PID_A4A2` for x86 and x64 (`NTamd64`). It references the native Windows system binaries `usb8023.sys` and `rndismp.sys` already present in `C:\Windows\System32\drivers\`.
- **`RNDIS.cat`**: Security catalog digitally signed by **Microsoft Windows Hardware Compatibility Publisher** (WHQL), allowing driver installation without disabling driver signature enforcement.

## Automated Installation
Run the automated PowerShell setup tool from the repository root:
```powershell
powershell -ExecutionPolicy Bypass -File tools/setup_remote_driver.ps1
```

## Manual Installation (Elevated Command Prompt or PowerShell)
```cmd
pnputil /add-driver rndis.inf /install
pnputil /restart-device /deviceid "USB\VID_0525&PID_A4A2"
```
