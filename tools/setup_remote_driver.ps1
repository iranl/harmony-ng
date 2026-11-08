<#
.SYNOPSIS
    Automated RNDIS Driver Installer and Network Setup for Logitech Harmony touchscreen remotes (Elite / Ultimate) on Windows.

.DESCRIPTION
    Installs the Microsoft WHQL-signed RNDIS template driver for Linux USB Ethernet gadgets (VID_0525&PID_A4A2),
    forces Windows to unbind any stuck USB Serial Device (COM port) driver, binds the RNDIS Network Adapter,
    and assigns static IP 192.168.2.2/24 (without a default gateway) so the remote at 192.168.2.1 is immediately reachable.

.PARAMETER NonInteractive
    Skip waiting for keypress at exit.
#>
[CmdletBinding()]
param(
    [switch]$NonInteractive
)

$ErrorActionPreference = "Stop"

trap {
    Write-Host "`n[-] ERROR OCCURRED: $_" -ForegroundColor Red
    if (-not $NonInteractive) {
        Write-Host "`nPress any key to close this window..."
        $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
    }
    exit 1
}

Write-Host "======================================================================" -ForegroundColor Cyan
Write-Host " Harmony Remote (Elite / Ultimate) - Windows RNDIS Driver & Network Setup" -ForegroundColor Cyan
Write-Host "======================================================================" -ForegroundColor Cyan

# 1. Ensure Administrator Privileges (Auto-Elevate)
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "[*] Administrator elevation required. Prompting for UAC..." -ForegroundColor Yellow
    $scriptFile = $PSCommandPath
    if (-not $scriptFile -and $MyInvocation.MyCommand.Path) {
        $scriptFile = $MyInvocation.MyCommand.Path
    }
    if (-not $scriptFile) {
        $scriptFile = (Resolve-Path "tools\setup_remote_driver.ps1").Path
    }
    $argList = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", "`"$scriptFile`""
    )
    if ($NonInteractive) {
        $argList += "-NonInteractive"
    }
    try {
        $proc = Start-Process powershell.exe -WorkingDirectory (Get-Location).Path -Verb RunAs -ArgumentList $argList -PassThru -Wait
        exit $proc.ExitCode
    } catch {
        Write-Error "[-] Administrator elevation was declined or failed: $_"
        exit 1
    }
}

# 2. Locate WHQL-signed RNDIS Driver INF
$scriptDir = $PSScriptRoot
if (-not $scriptDir -and $MyInvocation.MyCommand.Path) {
    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
}
if (-not $scriptDir) {
    $scriptDir = (Get-Location).Path
}

$candidatePaths = @(
    (Join-Path $scriptDir "..\drivers\windows\rndis\rndis.inf"),
    (Join-Path $scriptDir "drivers\windows\rndis\rndis.inf"),
    (Join-Path (Get-Location).Path "drivers\windows\rndis\rndis.inf"),
    (Join-Path (Get-Location).Path "..\drivers\windows\rndis\rndis.inf")
)

$infPath = $null
foreach ($c in $candidatePaths) {
    if (Test-Path -Path $c) {
        $infPath = (Resolve-Path -Path $c).Path
        break
    }
}

if (-not $infPath) {
    Write-Error "[-] Could not find drivers\windows\rndis\rndis.inf. Please run from repository root or tools/."
    exit 1
}

Write-Host "[*] Found RNDIS driver package: $infPath" -ForegroundColor Gray

# 3. Add Driver Package to Windows DriverStore via pnputil
Write-Host "[*] Staging and installing WHQL-signed RNDIS driver package..." -ForegroundColor Yellow
$pnpOutput = & pnputil.exe /add-driver "$infPath" /install
$pnpText = $pnpOutput -join "`n"

if ($pnpText -match "Added successfully" -or $pnpText -match "Driver package added" -or $pnpText -match "already exists" -or $LASTEXITCODE -eq 0) {
    Write-Host "[+] Driver package installed in DriverStore." -ForegroundColor Green
} else {
    Write-Host "[!] pnputil output:" -ForegroundColor Yellow
    Write-Host $pnpText -ForegroundColor Gray
}

# 4. Check for Connected Gadget & Force Re-binding
$hardwareIds = @(
    "USB\VID_0525&PID_A4A2",
    "USB\VID_050D&PID_010D",
    "USB\VID_0502&PID_3210"
)

Write-Host "[*] Checking for connected USB Ethernet gadget..." -ForegroundColor Yellow
$gadgetFound = $false

foreach ($hwId in $hardwareIds) {
    $devices = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like "*$hwId*" }
    if ($devices) {
        $gadgetFound = $true
        foreach ($dev in $devices) {
            Write-Host "[+] Detected remote hardware: $($dev.FriendlyName) ($($dev.InstanceId))" -ForegroundColor Green
            Write-Host "    Current Class: $($dev.Class) | Status: $($dev.Status)" -ForegroundColor Gray

            # If bound to Ports / COM serial or in Problem state, restart/remove to force driver re-match
            if ($dev.Class -eq "Ports" -or $dev.Problem -ne "CM_PROB_NONE") {
                Write-Host "[*] Device is bound to Serial or error state. Re-binding to RNDIS driver..." -ForegroundColor Yellow
                & pnputil.exe /restart-device "$($dev.InstanceId)" 2>$null | Out-Null
                Start-Sleep -Seconds 1

                # Re-check if still Ports
                $recheck = Get-PnpDevice -PresentOnly -InstanceId $dev.InstanceId -ErrorAction SilentlyContinue
                if ($recheck -and $recheck.Class -eq "Ports") {
                    Write-Host "[*] Forcing driver swap via device removal and PnP scan..." -ForegroundColor Yellow
                    & pnputil.exe /remove-device "$($dev.InstanceId)" 2>$null | Out-Null
                    Start-Sleep -Seconds 1
                    & pnputil.exe /scan-devices 2>$null | Out-Null
                }
            }
        }
    }
}

if (-not $gadgetFound) {
    Write-Host "[i] No g_ether device currently plugged in or remote is still in USB HID mode." -ForegroundColor Gray
    Write-Host "    Driver is installed in DriverStore and will bind automatically once remote enters Ethernet mode." -ForegroundColor Gray
}

# 5. Locate RNDIS Network Adapter and Configure Static IP
Write-Host "[*] Looking for RNDIS / USB Ethernet Network Adapter..." -ForegroundColor Yellow
$adapter = $null

for ($i = 0; $i -lt 10; $i++) {
    $adapter = Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object {
        $_.Status -eq "Up" -or $_.Status -eq "Disconnected"
    } | Where-Object {
        $_.InterfaceDescription -like "*RNDIS*" -or
        $_.InterfaceDescription -like "*Ethernet Gadget*" -or
        $_.InterfaceDescription -like "*Acer*" -or
        $_.DriverFileName -like "*rndis*"
    } | Select-Object -First 1

    if ($adapter) {
        break
    }
    Start-Sleep -Milliseconds 500
}

if ($adapter) {
    Write-Host "[+] Found Network Adapter: '$($adapter.Name)' ($($adapter.InterfaceDescription))" -ForegroundColor Green

    # Assign static IP 192.168.2.2/24 WITHOUT default gateway (prevents overriding host internet route)
    $currentIps = Get-NetIPAddress -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue
    $hasCorrectIp = $false

    foreach ($ip in $currentIps) {
        if ($ip.IPAddress -eq "192.168.2.2" -and $ip.PrefixLength -eq 24) {
            $hasCorrectIp = $true
        } elseif ($ip.IPAddress -notlike "169.254.*") {
            # Clean up old/conflicting static IPs on this interface
            Remove-NetIPAddress -InterfaceIndex $adapter.ifIndex -IPAddress $ip.IPAddress -Confirm:$false -ErrorAction SilentlyContinue
        }
    }

    # Ensure no default gateway is assigned to this point-to-point link
    Get-NetRoute -InterfaceIndex $adapter.ifIndex -DestinationPrefix "0.0.0.0/0" -ErrorAction SilentlyContinue |
        Remove-NetRoute -Confirm:$false -ErrorAction SilentlyContinue

    if (-not $hasCorrectIp) {
        Write-Host "[*] Assigning static IP 192.168.2.2/24 (no gateway) to '$($adapter.Name)'..." -ForegroundColor Yellow
        New-NetIPAddress -InterfaceIndex $adapter.ifIndex -IPAddress 192.168.2.2 -PrefixLength 24 -Confirm:$false | Out-Null
        Write-Host "[+] Configured 192.168.2.2/24 on '$($adapter.Name)'." -ForegroundColor Green
    } else {
        Write-Host "[+] IP 192.168.2.2/24 already assigned to '$($adapter.Name)'." -ForegroundColor Green
    }

    # 6. Test Connectivity to Remote
    Write-Host "[*] Probing connection to remote at 192.168.2.1..." -ForegroundColor Yellow
    $reachable = $false
    for ($attempt = 1; $attempt -le 5; $attempt++) {
        $tcp = Test-NetConnection -ComputerName "192.168.2.1" -Port 8088 -WarningAction SilentlyContinue
        if ($tcp.TcpTestSucceeded) {
            $reachable = $true
            Write-Host "[+] SUCCESS: Connected to Remote HTTP Setup Server (192.168.2.1:8088)!" -ForegroundColor Green
            break
        }
        $tcpSsh = Test-NetConnection -ComputerName "192.168.2.1" -Port 22 -WarningAction SilentlyContinue
        if ($tcpSsh.TcpTestSucceeded) {
            $reachable = $true
            Write-Host "[+] SUCCESS: Connected to Remote Dropbear SSH (192.168.2.1:22)!" -ForegroundColor Green
            break
        }
        Start-Sleep -Seconds 1
    }

    if (-not $reachable) {
        Write-Host "[i] Remote IP 192.168.2.1 not responding yet. Ensure remote is powered on and booted." -ForegroundColor Gray
    }
} else {
    Write-Host "[i] No active RNDIS network adapter detected yet." -ForegroundColor Gray
    Write-Host "    Driver is ready; adapter will appear when remote boots with USB Ethernet enabled." -ForegroundColor Gray
}

Write-Host "======================================================================" -ForegroundColor Cyan
Write-Host " Setup complete! You can now run: python remote/setup_remote.py" -ForegroundColor Green
Write-Host "======================================================================" -ForegroundColor Cyan

if (-not $NonInteractive) {
    Write-Host "`nPress any key to close this window..."
    $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
}
