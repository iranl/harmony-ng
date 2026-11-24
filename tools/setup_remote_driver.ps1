<#
.SYNOPSIS
    Automated RNDIS Driver Installer and Network Setup for Logitech Harmony touchscreen remotes (Elite / Ultimate) on Windows.

.DESCRIPTION
    Installs the Microsoft WHQL-signed RNDIS template driver for Linux USB Ethernet gadgets (VID_0525&PID_A4A2),
    forces Windows to unbind any stuck USB Serial Device (COM port) driver, binds the RNDIS Network Adapter,
    and assigns a static IP 192.168.2.x/24 (without a default gateway; x = 2 unless another adapter has it) so the
    remote at 192.168.2.1 is immediately reachable.

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
    throw "pnputil could not add the driver package (exit code $LASTEXITCODE). If it reports a catalog hash mismatch, rndis.inf must have CRLF line endings exactly as signed in RNDIS.cat."
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

# The adapter is taken from the remote's own USB device (its PnPDeviceID holds the VID/PID
# above). Phones in USB tethering mode and LTE sticks are RNDIS adapters too: an adapter found
# only by its description is used only while the remote's device is present, and never when
# it has a default route or an address of another network (that is someone's Internet link).
function Test-OtherNetwork($ad) {
    if (Get-NetRoute -InterfaceIndex $ad.ifIndex -DestinationPrefix "0.0.0.0/0" -ErrorAction SilentlyContinue) { return $true }
    $other = Get-NetIPAddress -InterfaceIndex $ad.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.IPAddress -notlike "169.254.*" -and $_.IPAddress -notlike "192.168.2.*" }
    return [bool]$other
}

for ($i = 0; $i -lt 10; $i++) {
    $candidates = @(Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object {
        $_.Status -eq "Up" -or $_.Status -eq "Disconnected"
    })
    # Every remote gets its own adapter in Windows: the adapter of a remote set up before can
    # still be listed (Disconnected). The adapter that is Up is the remote plugged in now.
    $adapter = $candidates | Where-Object {
        $pnpId = [string]$_.PnPDeviceID
        @($hardwareIds | Where-Object { $pnpId -like "$_*" }).Count -gt 0
    } | Sort-Object { $_.Status -ne "Up" } | Select-Object -First 1

    if (-not $adapter -and $gadgetFound) {
        $adapter = $candidates | Where-Object {
            $_.InterfaceDescription -like "*RNDIS*" -or
            $_.InterfaceDescription -like "*Ethernet Gadget*" -or
            $_.InterfaceDescription -like "*Acer*" -or
            $_.DriverFileName -like "*rndis*"
        } | Where-Object { -not (Test-OtherNetwork $_) } | Select-Object -First 1
    }

    if ($adapter) {
        break
    }
    Start-Sleep -Milliseconds 500
}

if ($adapter) {
    Write-Host "[+] Found Network Adapter: '$($adapter.Name)' ($($adapter.InterfaceDescription))" -ForegroundColor Green

    # Assign a static 192.168.2.x/24 WITHOUT default gateway (prevents overriding host internet route).
    # Windows can make a new adapter for each remote (or USB port), and the adapter of a remote set up
    # before keeps its address even while that remote is unplugged (the adapter is hidden then).
    # A new address is chosen among those no other adapter holds (static ones in the registry, live
    # ones of any kind): 192.168.2.2, .3, ... The remote is always 192.168.2.1 and answers any host
    # address in 192.168.2.0/24.
    $ownGuid = [string]$adapter.InterfaceGuid
    $usedByOthers = @{}
    Get-ChildItem "HKLM:\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters\Interfaces" -ErrorAction SilentlyContinue |
        Where-Object { $_.PSChildName -ne $ownGuid } | ForEach-Object {
            foreach ($a in @((Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue).IPAddress)) {
                if ($a) { $usedByOthers[[string]$a] = $true }
            }
        }
    Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.InterfaceIndex -ne $adapter.ifIndex } | ForEach-Object { $usedByOthers[[string]$_.IPAddress] = $true }

    $currentIps = Get-NetIPAddress -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue
    $hostIp = $null
    foreach ($ip in $currentIps) {
        # Kept: a 192.168.2.x/24 that Windows did not mark Duplicate or Invalid, unless another adapter
        # holds it and the remote does not answer on port 22
        if (-not $hostIp -and $ip.IPAddress -like "192.168.2.*" -and $ip.IPAddress -ne "192.168.2.1" -and
            $ip.PrefixLength -eq 24 -and
            "$($ip.AddressState)" -ne "Duplicate" -and "$($ip.AddressState)" -ne "Invalid" -and
            (-not $usedByOthers.ContainsKey($ip.IPAddress) -or
             (Test-NetConnection -ComputerName "192.168.2.1" -Port 22 -WarningAction SilentlyContinue).TcpTestSucceeded)) {
            $hostIp = $ip.IPAddress
        } elseif ($ip.IPAddress -notlike "169.254.*") {
            # Clean up old/conflicting static IPs on this interface
            Remove-NetIPAddress -InterfaceIndex $adapter.ifIndex -IPAddress $ip.IPAddress -Confirm:$false -ErrorAction SilentlyContinue
        }
    }

    # Ensure no default gateway is assigned to this point-to-point link
    Get-NetRoute -InterfaceIndex $adapter.ifIndex -DestinationPrefix "0.0.0.0/0" -ErrorAction SilentlyContinue |
        Remove-NetRoute -Confirm:$false -ErrorAction SilentlyContinue

    if ($hostIp) {
        Write-Host "[+] IP $hostIp/24 already assigned to '$($adapter.Name)'." -ForegroundColor Green
    } else {
        $hostIp = 2..254 | ForEach-Object { "192.168.2.$_" } | Where-Object { -not $usedByOthers.ContainsKey($_) } |
            Select-Object -First 1
        if (-not $hostIp) { throw "Every address in 192.168.2.2-254 is set on another adapter." }
        Write-Host "[*] Assigning static IP $hostIp/24 (no gateway) to '$($adapter.Name)'..." -ForegroundColor Yellow
        New-NetIPAddress -InterfaceIndex $adapter.ifIndex -IPAddress $hostIp -PrefixLength 24 -Confirm:$false | Out-Null
        Write-Host "[+] Configured $hostIp/24 on '$($adapter.Name)'." -ForegroundColor Green
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
