<#
.SYNOPSIS
    Deploy Harmony Elite custom binaries and recovery script over SSH.
.PARAMETER Ip
    Remote IP of the Elite remote (default 192.168.2.1).
.PARAMETER KeyPath
    Optional path to SSH private key.
.PARAMETER DebugBusybox
    Install modified extended BusyBox build to /data/busybox (default false; regular users use stock BusyBox).
.PARAMETER DevMode
    Alias for DebugBusybox.
#>
param(
    [string]$Ip = "192.168.2.1",
    [string]$KeyPath = "",
    [switch]$DebugBusybox,
    [switch]$DevMode,
    [switch]$FactoryReset,
    [switch]$Reboot
)

$ErrorActionPreference = "Continue"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BinDir = Join-Path $ScriptDir "bin"
$RcsLocal = Join-Path $ScriptDir "rcS.local"
$InitScript = Join-Path $ScriptDir "init.sh"
$Target = "root@$Ip"

if (-not $KeyPath) {
    $candFiles = @(
        Get-ChildItem -Path "$HOME\.ssh\harmony_owner_*" -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -ne ".pub" }
        Get-ChildItem -Path "$HOME\.ssh\id_rsa" -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -ne ".pub" }
        Get-ChildItem -Path "$HOME\.ssh\id_ed25519" -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -ne ".pub" }
    )
    if ($candFiles.Count -gt 0) {
        $KeyPath = $candFiles[0].FullName
    }
}

$sshArgs = @("-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR")
if ($KeyPath -and (Test-Path $KeyPath)) {
    $sshArgs += @("-i", $KeyPath)
}

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  Harmony Elite Deployment (SSH)" -ForegroundColor Cyan
Write-Host "  Target: $Target" -ForegroundColor Cyan
if ($FactoryReset) {
    Write-Host "  Mode: FACTORY RESET & STOCK RESTORATION" -ForegroundColor Red
}
Write-Host "============================================================" -ForegroundColor Cyan

Write-Host "[*] Testing connection..." -ForegroundColor Yellow
$probe = & ssh @sshArgs $Target "uname -a"
if ($LASTEXITCODE -ne 0) {
    Write-Host "[-] Could not connect to $Target over SSH: $probe" -ForegroundColor Red
    Write-Host "    Ensure USB Ethernet interface is set to 192.168.2.2/24."
    exit 1
}
Write-Host "[+] Remote: $probe" -ForegroundColor Green

if ($FactoryReset) {
    Write-Host "============================================================" -ForegroundColor Red
    Write-Host "  WARNING: HARMONY ELITE FACTORY RESET & STOCK RESTORATION" -ForegroundColor Red
    Write-Host "============================================================" -ForegroundColor Red
    Write-Host "  This will remove all custom software, restore original stock scripts," -ForegroundColor Yellow
    Write-Host "  and reboot the Elite Remote back into official Logitech stock firmware." -ForegroundColor Yellow
    $ans = Read-Host "Type 'RESET' to confirm"
    if ($ans -ne "RESET") {
        Write-Host "[-] Confirmation mismatch. Aborting reset." -ForegroundColor Red
        exit 1
    }
    Write-Host "[*] Reverting Elite Remote to stock (preserving config files)..." -ForegroundColor Yellow
    & ssh @sshArgs $Target "if [ -x /data/init.sh ]; then /data/init.sh stop 2>/dev/null || true; fi; killall -9 init.sh elite_ui codex_elite elite_watchdog.sh codex_watchdog.sh 2>/dev/null || true; if [ -f /data/stock_backup/rcS.local.orig ] && ! grep -q 'usbeth' /data/stock_backup/rcS.local.orig 2>/dev/null && ! grep -q 'dropbear' /data/stock_backup/rcS.local.orig 2>/dev/null && ! grep -q 'exit 0' /data/stock_backup/rcS.local.orig 2>/dev/null; then cp -f /data/stock_backup/rcS.local.orig /etc/init.d/rcS.local; chmod 755 /etc/init.d/rcS.local; echo '[+] Restored stock rcS.local'; else printf '#!/bin/sh\n/usr/bin/flashuboot /lib/firmware/u-boot.bin 2>/dev/null || true\n' > /etc/init.d/rcS.local; chmod 755 /etc/init.d/rcS.local; echo '[+] Restored clean stock rcS.local'; fi; chmod +x /usr/bin/watchdog /usr/bin/usbhid /usr/bin/hal /opt/luaworks/luaworks /opt/luadraws/luadraws /usr/sbin/bluetoothd /usr/bin/dbus-daemon 2>/dev/null || true; mkdir -p /data/conf_p 2>/dev/null; cp -f /data/elite_settings.conf /data/conf_p/ 2>/dev/null || true; rm -f /etc/nowatchdog /etc/tdeenable /data/reboot_counter /tmp/codex_elite.log /tmp/elite_ui.log /tmp/elite-init.log /tmp/elite_stop /tmp/elite_supervisors.pid 2>/dev/null || true; rm -f /usr/bin/sync /usr/sbin/dropbear /usr/sbin/dropbearkey /usr/sbin/dbclient /usr/sbin/scp 2>/dev/null || true; rm -f /usr/bin/head /usr/bin/tail /usr/bin/which /usr/bin/find /usr/bin/tar /usr/bin/xargs /usr/bin/xz /usr/bin/bzcat /usr/bin/bunzip2 /usr/bin/lzma /usr/bin/unlzma /usr/bin/diff /usr/bin/patch /usr/bin/awk /usr/bin/sed 2>/dev/null || true; /data/elite_sync 2>/dev/null || /data/sync 2>/dev/null || sync 2>/dev/null || true; sleep 1; rm -f /data/elite_* /data/codex_* /data/dropbear* /data/sync /data/mknod /data/scp /data/busybox /data/init.sh /data/safe_reboot.sh /data/factory_reset.sh 2>/dev/null || true; rm -rf /data/stock_backup /data/assets 2>/dev/null || true; cp -f /data/conf_p/* /data/ 2>/dev/null || true; rm -rf /data/conf_p 2>/dev/null; /data/elite_sync 2>/dev/null || sync 2>/dev/null || true; reboot -f 2>/dev/null &"
    Write-Host "[+] Factory reset complete! Remote is rebooting into stock firmware (configs preserved)." -ForegroundColor Green
    exit 0
}

Write-Host "[*] Preserving stock configuration backup..." -ForegroundColor Yellow
& ssh @sshArgs $Target "mkdir -p /data/stock_backup; if [ -f /etc/init.d/rcS.local ] && [ ! -f /data/stock_backup/rcS.local.orig ]; then if ! grep -q 'codex' /etc/init.d/rcS.local 2>/dev/null && ! grep -q 'elite' /etc/init.d/rcS.local 2>/dev/null && ! grep -q 'usbeth' /etc/init.d/rcS.local 2>/dev/null && ! grep -q 'dropbear' /etc/init.d/rcS.local 2>/dev/null && ! grep -q 'exit 0' /etc/init.d/rcS.local 2>/dev/null; then cp -f /etc/init.d/rcS.local /data/stock_backup/rcS.local.orig; echo '[+] Saved /etc/init.d/rcS.local to /data/stock_backup/rcS.local.orig'; fi; fi"

Write-Host "[*] Stopping existing runtime services (/data/init.sh stop)..." -ForegroundColor Yellow
& ssh @sshArgs $Target "if [ -x /data/init.sh ]; then /data/init.sh stop 2>/dev/null || true; else killall -9 elite_watchdog.sh codex_watchdog.sh elite_ui codex_elite hal luaworks luadraws lua luajit luajit-2.0.0-beta10 watchdog usbhid 2>/dev/null || true; fi; sleep 1"

# One-way migration: move old configs (Hub config, then settings such as usbeth) and remove old codex binaries
& ssh @sshArgs $Target "if [ -f /data/codex_elite_config.json ] && [ ! -f /data/elite_rf_config.json ]; then mv /data/codex_elite_config.json /data/elite_rf_config.json 2>/dev/null || true; fi; if [ -f /data/codex_elite.conf ] && [ ! -f /data/elite_settings.conf ]; then mv /data/codex_elite.conf /data/elite_settings.conf 2>/dev/null || true; fi; if [ -f /data/elite_ui.conf ] && [ ! -f /data/elite_settings.conf ]; then mv /data/elite_ui.conf /data/elite_settings.conf 2>/dev/null || true; fi; rm -f /data/codex_elite /data/codex_elite.new /data/codex_sync /data/codex_watchdog.sh /data/codex_elite.conf /data/codex_elite_config.json /data/elite_ui.conf /data/resources/ActivityList.json /tmp/codex_elite.log /data/codex_crash.log 2>/dev/null || true"

$scpArgs = @("-O") + $sshArgs

Write-Host "[*] Querying remote checksums (md5sum)..." -ForegroundColor Yellow
$remoteMd5Raw = & ssh @sshArgs $Target "md5sum /data/elite_ui /data/elite_sync /data/init.sh /data/elite_watchdog.sh /data/safe_reboot.sh /data/dropbear /data/dropbearkey /data/scp /data/mknod /data/busybox /data/factory_reset.sh /etc/init.d/rcS.local 2>/dev/null || true"
$remoteHashes = @{}
if ($remoteMd5Raw) {
    foreach ($line in $remoteMd5Raw) {
        $parts = $line.Trim() -split '\s+'
        if ($parts.Length -ge 2) {
            $remoteHashes[$parts[1].Trim()] = $parts[0].Trim().ToLower()
        }
    }
}

function Deploy-EliteFileIfChanged {
    param(
        [string]$LocalFile,
        [string]$RemoteFile,
        [string]$Perm = "755"
    )
    if (-not (Test-Path $LocalFile)) { return $false }
    $localMd5 = (Get-FileHash -Algorithm MD5 $LocalFile).Hash.ToLower()
    $remoteMd5 = $remoteHashes[$RemoteFile]
    $fileName = Split-Path -Leaf $LocalFile
    if ($remoteMd5 -and ($remoteMd5 -eq $localMd5)) {
        Write-Host "    -> $fileName (MD5 match: $localMd5 - skipped)" -ForegroundColor DarkGray
        return $false
    }
    Write-Host "    -> Uploading $fileName (local: $localMd5 != remote: $(if ($remoteMd5) { $remoteMd5 } else { 'missing' }))" -ForegroundColor Yellow
    $tmpRemote = "$RemoteFile.new"
    & scp @scpArgs $LocalFile "${Target}:${tmpRemote}" | Out-Null
    & ssh @sshArgs $Target "chmod $Perm $tmpRemote 2>/dev/null; mv -f $tmpRemote $RemoteFile 2>/dev/null || true"
    return $true
}

Write-Host "[*] Syncing core binaries to /data/ (MD5 check)..." -ForegroundColor Yellow
$binaries = @("elite_ui", "elite_sync", "dropbear", "dropbearkey", "scp", "mknod", "busybox")
foreach ($b in $binaries) {
    $src = Join-Path $BinDir $b
    Deploy-EliteFileIfChanged -LocalFile $src -RemoteFile "/data/$b" -Perm "755" | Out-Null
}

$watchdogScript = Join-Path $ScriptDir "elite_watchdog.sh"
if (Test-Path $watchdogScript) {
    $wName = Split-Path -Leaf $watchdogScript
    Write-Host "[*] Syncing /data/$wName (MD5 check)..." -ForegroundColor Yellow
    Deploy-EliteFileIfChanged -LocalFile $watchdogScript -RemoteFile "/data/$wName" -Perm "755" | Out-Null
}

if (Test-Path $RcsLocal) {
    Write-Host "[*] Syncing /etc/init.d/rcS.local (MD5 check)..." -ForegroundColor Yellow
    Deploy-EliteFileIfChanged -LocalFile $RcsLocal -RemoteFile "/etc/init.d/rcS.local" -Perm "755" | Out-Null
}

$resetScript = Join-Path $ScriptDir "factory_reset.sh"
if (Test-Path $resetScript) {
    Write-Host "[*] Syncing /data/factory_reset.sh (MD5 check)..." -ForegroundColor Yellow
    Deploy-EliteFileIfChanged -LocalFile $resetScript -RemoteFile "/data/factory_reset.sh" -Perm "755" | Out-Null
}

if (Test-Path $InitScript) {
    Write-Host "[*] Syncing /data/init.sh (MD5 check)..." -ForegroundColor Yellow
    Deploy-EliteFileIfChanged -LocalFile $InitScript -RemoteFile "/data/init.sh" -Perm "755" | Out-Null
}

$safeRebootScript = Join-Path $ScriptDir "safe_reboot.sh"
if (Test-Path $safeRebootScript) {
    Write-Host "[*] Syncing /data/safe_reboot.sh (MD5 check)..." -ForegroundColor Yellow
    Deploy-EliteFileIfChanged -LocalFile $safeRebootScript -RemoteFile "/data/safe_reboot.sh" -Perm "755" | Out-Null
}

Write-Host "[*] Verifying permissions and flushing NAND flash buffers..." -ForegroundColor Yellow
$postCmd = "chmod 755 /data/elite_* /data/init.sh /data/safe_reboot.sh /data/dropbear* /data/scp /etc/init.d/rcS.local 2>/dev/null || true; " +
           "chmod -x /usr/bin/watchdog /usr/bin/usbhid /usr/bin/hal /opt/luaworks/luaworks /opt/luadraws/luadraws 2>/dev/null || true; " +
           "[ -e /data/sync ] || ln -sf /data/elite_sync /data/sync 2>/dev/null || true; " +
           "[ -e /usr/bin/sync ] || ln -sf /data/elite_sync /usr/bin/sync 2>/dev/null || true; " +
           "[ -L /usr/sbin/dropbear ] || ln -sf /data/dropbear /usr/sbin/dropbear 2>/dev/null || true; " +
           "rm -f /cache/crashlog-*.json 2>/dev/null || true; " +
           "/data/elite_sync 2>/dev/null || sync 2>/dev/null || true"

$res = & ssh @sshArgs $Target $postCmd

if ($Reboot) {
    Write-Host "[*] Pre-reboot verification and safe reboot (flush flash buffers)..." -ForegroundColor Yellow
    $rebootCmd = 'if ! /bin/sh -c "true" 2>/dev/null; then echo "[-] CRITICAL: /bin/sh verification failed. Aborting reboot!"; exit 1; fi; (/data/safe_reboot.sh >/dev/null 2>&1 &)'
    & ssh @sshArgs $Target $rebootCmd
    Write-Host "[+] Deployment complete! Elite Remote is rebooting safely." -ForegroundColor Green
}
else {
    Write-Host "[*] Hot-restarting Elite runtime services (/data/init.sh start)..." -ForegroundColor Yellow
    $restartCmd = "rm -f /tmp/elite_stop 2>/dev/null || true; (/data/init.sh start >/dev/null 2>&1 &)"
    & ssh @sshArgs $Target $restartCmd
    Write-Host "[+] Deployment complete! elite_ui hot-restarted (no reboot)." -ForegroundColor Green
}
