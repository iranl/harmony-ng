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
    [switch]$FactoryReset
)

$ErrorActionPreference = "Continue"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BinDir = Join-Path $ScriptDir "bin"
$RcsLocal = Join-Path $ScriptDir "rcS.local"
$Target = "root@$Ip"

$sshArgs = @("-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=NUL", "-o", "LogLevel=ERROR")
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
    Write-Host "  This will remove all Codex software, restore original stock scripts," -ForegroundColor Yellow
    Write-Host "  and reboot the Elite Remote back into official Logitech stock firmware." -ForegroundColor Yellow
    $ans = Read-Host "Type 'RESET' to confirm"
    if ($ans -ne "RESET") {
        Write-Host "[-] Confirmation mismatch. Aborting reset." -ForegroundColor Red
        exit 1
    }
    Write-Host "[*] Reverting Elite Remote to stock (preserving config files)..." -ForegroundColor Yellow
    & ssh @sshArgs $Target "killall -9 codex_elite 2>/dev/null || true; if [ -f /data/stock_backup/rcS.local.orig ]; then cp -f /data/stock_backup/rcS.local.orig /etc/init.d/rcS.local; chmod 755 /etc/init.d/rcS.local; echo '[+] Restored stock rcS.local'; else rm -f /etc/init.d/rcS.local; fi; chmod +x /usr/bin/watchdog /usr/bin/usbhid /usr/bin/hal /opt/luaworks/luaworks /opt/luadraws/luadraws 2>/dev/null || true; mkdir -p /data/conf_p 2>/dev/null; cp -f /data/codex_elite_config.json /data/codex_elite.conf /data/conf_p/ 2>/dev/null || true; rm -f /etc/nowatchdog /data/reboot_counter /tmp/codex_elite.log 2>/dev/null || true; rm -f /data/codex_elite /data/codex_elite.new /data/codex_sync /data/dropbear* /data/sync /data/mknod /data/scp /data/busybox 2>/dev/null || true; rm -f /usr/bin/sync /usr/sbin/dropbear /usr/sbin/dropbearkey /usr/sbin/dbclient /usr/sbin/scp 2>/dev/null || true; cp -f /data/conf_p/* /data/ 2>/dev/null || true; rm -rf /data/conf_p 2>/dev/null; /data/codex_sync 2>/dev/null || /data/sync 2>/dev/null || sync 2>/dev/null || true; reboot -f 2>/dev/null &"
    Write-Host "[+] Factory reset complete! Remote is rebooting into stock firmware (configs preserved)." -ForegroundColor Green
    exit 0
}

Write-Host "[*] Preserving stock configuration backup..." -ForegroundColor Yellow
& ssh @sshArgs $Target "mkdir -p /data/stock_backup; if [ -f /etc/init.d/rcS.local ] && [ ! -f /data/stock_backup/rcS.local.orig ]; then if ! grep -q 'codex' /etc/init.d/rcS.local 2>/dev/null; then cp -f /etc/init.d/rcS.local /data/stock_backup/rcS.local.orig; echo '[+] Saved /etc/init.d/rcS.local to /data/stock_backup/rcS.local.orig'; fi; fi"

$scpArgs = @("-O") + $sshArgs

Write-Host "[*] Stopping existing codex_elite and stock processes..." -ForegroundColor Yellow
& ssh @sshArgs $Target "killall -9 codex_elite hal luaworks luadraws lua luajit luajit-2.0.0-beta10 watchdog usbhid 2>/dev/null || true"

Write-Host "[*] Querying remote checksums (md5sum)..." -ForegroundColor Yellow
$remoteMd5Raw = & ssh @sshArgs $Target "md5sum /data/* /data/bin/* /etc/init.d/rcS.local 2>/dev/null || true"
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
$binaries = @("codex_elite", "codex_sync", "dropbear", "dropbearkey", "scp", "mknod", "busybox")
foreach ($b in $binaries) {
    $src = Join-Path $BinDir $b
    Deploy-EliteFileIfChanged -LocalFile $src -RemoteFile "/data/$b" -Perm "755" | Out-Null
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

$switchPng = Join-Path (Split-Path -Parent $ScriptDir) "payload\www\assets\icons\switch.png"
if (Test-Path $switchPng) {
    & ssh @sshArgs $Target "mkdir -p /data/assets 2>/dev/null || true"
    Deploy-EliteFileIfChanged -LocalFile $switchPng -RemoteFile "/data/assets/switch.png" -Perm "644" | Out-Null
}

Write-Host "[*] Configuring permissions and starting codex_elite..." -ForegroundColor Yellow
$postCmd = "chmod 755 /data/codex_* /data/dropbear* /data/scp /etc/init.d/rcS.local 2>/dev/null || true; " +
           "chmod -x /usr/bin/watchdog /usr/bin/usbhid /usr/bin/hal /opt/luaworks/luaworks /opt/luadraws/luadraws 2>/dev/null || true; " +
           "ln -sf /data/codex_sync /data/sync 2>/dev/null || true; " +
           "ln -sf /data/codex_sync /usr/bin/sync 2>/dev/null || true; " +
           "ln -sf /data/dropbear /usr/sbin/dropbear 2>/dev/null || true; " +
           "if [ -x /data/busybox ]; then cp -f /data/busybox /bin/busybox 2>/dev/null; for a in head tail which find tar xargs xz bzcat bunzip2 lzma unlzma diff patch awk sed; do ln -sf /data/busybox /usr/bin/$a 2>/dev/null; done; fi; " +
           "killall -9 codex_elite hal luaworks luadraws lua luajit luajit-2.0.0-beta10 watchdog usbhid 2>/dev/null || true; " +
           "/data/codex_elite > /tmp/codex_elite.log 2>&1 & sleep 1; pidof codex_elite || echo 'offline'"

$res = & ssh @sshArgs $Target $postCmd
Write-Host "[+] Status: $res" -ForegroundColor Green
Write-Host "[+] Elite deployment complete!" -ForegroundColor Cyan
