<#
.SYNOPSIS
    Deploy Harmony Hub native binaries, startup scripts, and configurations over SSH.
.PARAMETER HubIp
    IP address of the Harmony Hub (default 192.168.1.1).
.PARAMETER KeyPath
    Path to the SSH private key (default auto-searches ~/.ssh/ for harmony_owner_* or id_rsa).
.PARAMETER DebugBusybox
    Install modified extended BusyBox to /cache/bin (default false; regular users use stock BusyBox).
.PARAMETER DevMode
    Alias for DebugBusybox.
#>
[Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSAvoidUsingWriteHost', '')]
param(
    [string]$HubIp = "192.168.1.1",
    [string]$KeyPath = "",
    [switch]$DebugBusybox,
    [switch]$DevMode,
    [switch]$Reboot,
    [switch]$FactoryReset
)

$ErrorActionPreference = "Continue"
$ScriptDir = if ($PSScriptRoot) { $PSScriptRoot } elseif ($MyInvocation.MyCommand.Path) { Split-Path -Parent $MyInvocation.MyCommand.Path } else { (Get-Location).Path }
$RepoRoot = Split-Path -Parent $ScriptDir
$PayloadDir = Join-Path $RepoRoot "payload"

# Support running directly from extracted release archive or from repo build/ directory
if (Test-Path (Join-Path $ScriptDir "bin")) {
    $BinDir = Join-Path $ScriptDir "bin"
    $ScriptsDir = Join-Path $ScriptDir "scripts"
    $ModulesDir = Join-Path $ScriptDir "modules"
    $WwwDir = Join-Path $ScriptDir "www"
    $PayloadDir = $ScriptDir
}
else {
    $BinDir = Join-Path $PayloadDir "bin"
    $ScriptsDir = Join-Path $PayloadDir "scripts"
    $ModulesDir = Join-Path $PayloadDir "modules"
    $WwwDir = Join-Path $PayloadDir "www"

    # Local Development: Fallback to build/output or android-app/dist if payload directories are empty
    if (-not (Test-Path (Join-Path $BinDir "hub_daemon")) -and (Test-Path (Join-Path $RepoRoot "build\output\hub_daemon"))) {
        $BinDir = Join-Path $RepoRoot "build\output"
    }
    if (-not (Test-Path (Join-Path $WwwDir "index.html.gz")) -and -not (Test-Path (Join-Path $WwwDir "index.html"))) {
        if (Test-Path (Join-Path $RepoRoot "android-app\dist\index.html")) {
            $WwwDir = Join-Path $RepoRoot "android-app\dist"
        }
    }
}

$Target = "root@$HubIp"

# Auto-detect SSH key if not specified
if (-not $KeyPath) {
    $candFiles = @(
        Get-ChildItem -Path "$HOME\.ssh\harmony_owner_*" -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -ne ".pub" }
        Get-ChildItem -Path "$HOME\.ssh\id_rsa" -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -ne ".pub" }
        Get-ChildItem -Path "$HOME\.ssh\id_ed25519" -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -ne ".pub" }
    )
    foreach ($cand in $candFiles) {
        if ($cand -and (Test-Path $cand.FullName)) {
            $KeyPath = $cand.FullName
            break
        }
    }
}

$sshArgs = @("-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=NUL", "-o", "LogLevel=ERROR")
if ($KeyPath -and (Test-Path $KeyPath)) {
    $sshArgs += @("-i", $KeyPath)
    Write-Host "[*] Using SSH Key: $KeyPath" -ForegroundColor Gray
}

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  Harmony Hub Deployment (SSH)" -ForegroundColor Cyan
Write-Host "  Target: $Target" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

Write-Host "[*] Testing connection to Hub..." -ForegroundColor Yellow
$probe = & ssh @sshArgs $Target "uname -a"
if ($LASTEXITCODE -ne 0) {
    Write-Host "[-] Could not connect to $Target over SSH: $probe" -ForegroundColor Red
    exit 1
}
Write-Host "[+] Connected: $probe" -ForegroundColor Green

if ($FactoryReset) {
    Write-Host "============================================================" -ForegroundColor Red
    Write-Host "  WARNING: HARMONY HUB FACTORY RESET & STOCK RESTORATION" -ForegroundColor Red
    Write-Host "============================================================" -ForegroundColor Red
    Write-Host "  This will remove all custom software, restore original stock scripts," -ForegroundColor Yellow
    Write-Host "  and reboot the Hub back into official Logitech stock firmware." -ForegroundColor Yellow
    $ans = Read-Host "Type 'RESET' to confirm"
    if ($ans -ne "RESET") {
        Write-Host "[-] Confirmation mismatch. Aborting reset." -ForegroundColor Red
        exit 1
    }
    Write-Host "[*] Reverting Hub to stock (preserving config files)..." -ForegroundColor Yellow
    & ssh @sshArgs $Target "killall -9 init.sh hub_daemon codex_daemon hub_btstack codex_btstack hub_rf codex_rf hub_dhcpd codex_dhcpd hub_portal codex_portal hal luaworks luadraws lua 2>/dev/null || true; if [ -f /data/stock_backup/rcS.local.orig ]; then cp -f /data/stock_backup/rcS.local.orig /etc/init.d/rcS.local; chmod 755 /etc/init.d/rcS.local; echo '[+] Restored stock rcS.local'; else rm -f /etc/init.d/rcS.local; fi; chmod +x /usr/bin/watchdog /opt/luaworks/luaworks /usr/bin/hal /usr/sbin/bluetoothd /usr/bin/dbus-daemon 2>/dev/null || true; rm -f /etc/nowatchdog /usr/sbin/dropbear /usr/sbin/dropbearkey /data/rf_pair_test /data/test_hub_tx /data/codex_rf /data/hub_rf 2>/dev/null || true; mkdir -p /data/harmony_p 2>/dev/null; cp -rf /data/harmony/config/* /data/harmony_p/ 2>/dev/null || true; cp -f /data/harmony/mqtt_config.json /data/harmony_p/ 2>/dev/null || true; rm -rf /data/harmony/bin /data/harmony/www /data/harmony-backups /data/codex-backups /data/codex /data/codexmqtt /cache/*.log /cache/crashlog-*.json 2>/dev/null || true; rm -f /data/harmony/*.conf /data/harmony/current_activity /data/harmony/config_version 2>/dev/null || true; mkdir -p /data/harmony/config 2>/dev/null; cp -rf /data/harmony_p/* /data/harmony/config/ 2>/dev/null; [ -f /data/harmony_p/mqtt_config.json ] && cp -f /data/harmony_p/mqtt_config.json /data/harmony/mqtt_config.json 2>/dev/null || true; rm -rf /data/harmony_p 2>/dev/null; /data/harmony/bin/hub_sync 2>/dev/null || sync 2>/dev/null || true; reboot -f 2>/dev/null &"
    Write-Host "[+] Factory reset complete! Hub is rebooting into stock firmware (configs preserved)." -ForegroundColor Green
    exit 0
}

$scpArgs = @("-O") + $sshArgs

Write-Host "[*] Preserving stock configuration backup..." -ForegroundColor Yellow
& ssh @sshArgs $Target "mkdir -p /data/stock_backup; if [ -f /etc/init.d/rcS.local ] && [ ! -f /data/stock_backup/rcS.local.orig ]; then if ! grep -q 'elite' /etc/init.d/rcS.local 2>/dev/null && ! grep -q 'codex' /etc/init.d/rcS.local 2>/dev/null && ! grep -q 'harmony' /etc/init.d/rcS.local 2>/dev/null; then cp -f /etc/init.d/rcS.local /data/stock_backup/rcS.local.orig; echo '[+] Saved /etc/init.d/rcS.local to /data/stock_backup/rcS.local.orig'; fi; fi"

Write-Host "[*] Stopping existing daemons cleanly (quiescing RF & SPI flash)..." -ForegroundColor Yellow
$stopCmd = @'
touch /tmp/harmony_stop 2>/dev/null || true
if [ -x /data/harmony/init.sh ]; then
    /data/harmony/init.sh stop
elif [ -x /data/harmony/scripts/init.sh ]; then
    /data/harmony/scripts/init.sh stop
else
    killall -15 hub_rf 2>/dev/null || true
    sleep 1
    killall -9 hub_rf hub_daemon hub_btstack hub_dhcpd hub_portal hal luaworks luadraws lua 2>/dev/null || true
fi
for _p in $(ps | grep 'init\.sh' | sed 's/^[ ]*//' | cut -d' ' -f1 2>/dev/null); do
    [ -n "$_p" ] && [ "$_p" != "$$" ] && kill -9 "$_p" 2>/dev/null || true
done
/data/harmony/bin/hub_sync 2>/dev/null || /cache/bin/sync 2>/dev/null || sync 2>/dev/null || true
'@
& ssh @sshArgs $Target "$stopCmd"

Write-Host "[*] Migrating legacy codex paths and preparing directories..." -ForegroundColor Yellow
& ssh @sshArgs $Target "if [ -d /data/codex ] && [ ! -L /data/codex ]; then if [ ! -d /data/harmony ]; then mv /data/codex /data/harmony 2>/dev/null || true; else cp -rn /data/codex/* /data/harmony/ 2>/dev/null || true; rm -rf /data/codex 2>/dev/null || true; fi; fi; rm -f /data/codex /data/codex_p 2>/dev/null || true; rm -rf /data/codex-backups /data/codexmqtt /data/harmony-backups /cache/crashlog-*.json 2>/dev/null; rm -f /cache/codex_*.ota.gz /cache/codex_*.ota /cache/*.new /cache/*/*.new /cache/www/*.new /cache/www/assets/*.new /data/harmony/*.new /data/harmony/*/*.new /data/rf_pair_test /data/test_hub_tx /data/codex_rf /data/hub_rf /data/harmony/bin/codex_* /cache/bin/codex_* /var/run/codex_* 2>/dev/null || true; mkdir -p /data/harmony/bin /data/harmony/www /data/harmony/www/assets /data/harmony/custom_icons /etc/init.d /cache/www/assets/icons; [ -L /data/harmony/www/assets/icons ] || ln -s /cache/www/assets/icons /data/harmony/www/assets/icons"

Write-Host "[*] Querying remote checksums (md5sum)..." -ForegroundColor Yellow
$remoteMd5Raw = & ssh @sshArgs $Target "md5sum /data/harmony/bin/* /data/harmony/* /cache/www/* /cache/www/assets/* /cache/modules/* /cache/bin/busybox /cache/elite_ui.ota.gz /cache/icons.tar.gz /etc/init.d/rcS.local 2>/dev/null || true"
$remoteHashes = @{}
if ($remoteMd5Raw) {
    foreach ($line in $remoteMd5Raw) {
        $parts = $line.Trim() -split '\s+'
        if ($parts.Length -ge 2) {
            $remoteHashes[$parts[1].Trim()] = $parts[0].Trim().ToLower()
        }
    }
}

function Install-HubFileIfChanged {
    [Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSAvoidUsingBrokenHashAlgorithms', '')]
    param(
        [string]$LocalFile,
        [string]$RemoteFile,
        [string]$Perm = "755",
        [switch]$UseAtomicNew
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
    if ($UseAtomicNew) {
        $tmpRemote = "$RemoteFile.new"
        & scp @scpArgs $LocalFile "${Target}:${tmpRemote}"
        if ($LASTEXITCODE -ne 0) {
            Write-Host "    [-] Upload failed for $fileName! Cleaning up temp file." -ForegroundColor Red
            & ssh @sshArgs $Target "rm -f $tmpRemote 2>/dev/null"
            return $false
        }
        & ssh @sshArgs $Target "chmod $Perm $tmpRemote 2>/dev/null; mv -f $tmpRemote $RemoteFile 2>/dev/null || true"
    }
    else {
        & scp @scpArgs $LocalFile "${Target}:${RemoteFile}"
        if ($LASTEXITCODE -ne 0) {
            Write-Host "    [-] Upload failed for $fileName!" -ForegroundColor Red
            return $false
        }
        & ssh @sshArgs $Target "chmod $Perm $RemoteFile 2>/dev/null || true"
    }
    return $true
}
Set-Alias Deploy-HubFileIfChanged Install-HubFileIfChanged

# 1. Upload binaries (standard core binaries only)
Write-Host "[*] Uploading native MIPS binaries (MD5 check)..." -ForegroundColor Yellow
$bins = Get-ChildItem -Path $BinDir -File | Where-Object {
    $_.Name -notmatch '^\.|\.txt$|^FILES$' -and $_.Name -ne 'busybox'
}
foreach ($b in $bins) {
    Install-HubFileIfChanged -LocalFile $b.FullName -RemoteFile "/data/harmony/bin/$($b.Name)" -Perm "755" | Out-Null
}
# 1b. Upload Elite RF OTA image to /cache/elite_ui.ota.gz if available
$eliteOtaSrc = Join-Path $RepoRoot "elite\bin\elite_ui.ota.gz"
if (-not (Test-Path $eliteOtaSrc)) {
    $eliteOtaSrc = Join-Path $RepoRoot "build\output\elite\elite_ui.ota.gz"
}
if (Test-Path $eliteOtaSrc) {
    Write-Host "[*] Syncing Elite RF OTA image (/cache/elite_ui.ota.gz)..." -ForegroundColor Yellow
    Install-HubFileIfChanged -LocalFile $eliteOtaSrc -RemoteFile "/cache/elite_ui.ota.gz" -Perm "644" | Out-Null
}

# Extended BusyBox (default deployment)
if ($DebugBusybox -or $DevMode) {
    Write-Host "[*] Developer / Debug mode active." -ForegroundColor Cyan
}
$bb = Join-Path $BinDir "busybox"
if (Test-Path $bb) {
    Write-Host "[*] Syncing extended BusyBox to /cache/bin/..." -ForegroundColor Yellow
    & ssh @sshArgs $Target "mkdir -p /cache/bin 2>/dev/null"
    $changed = Install-HubFileIfChanged -LocalFile $bb -RemoteFile "/cache/bin/busybox" -Perm "755"
    if ($changed) {
        $bbCmd = 'rm -f /data/bin/sh /data/bin/ash /data/bin/busybox 2>/dev/null || true; ln -sf /cache/bin/busybox /data/harmony/bin/busybox 2>/dev/null; /cache/bin/busybox --install -s /cache/bin 2>/dev/null || true; for _app in $(/cache/bin/busybox --list 2>/dev/null); do [ ! -e "/bin/$_app" ] && [ ! -e "/usr/bin/$_app" ] && ln -sf /cache/bin/busybox "/usr/bin/$_app" 2>/dev/null || true; done; mkdir -p /etc/profile.d 2>/dev/null; echo "export PATH=/cache/bin:/data/harmony/bin:\$PATH" > /etc/profile.d/00-harmony.sh 2>/dev/null; chmod 644 /etc/profile.d/00-harmony.sh 2>/dev/null || true'
        & ssh @sshArgs $Target "$bbCmd"
    }
}
# Ensure root profile uses extended BusyBox in cache if present
$profCmd = 'for prof in /root/.profile /home/root/.profile; do mkdir -p "$(dirname "$prof")" 2>/dev/null; echo "export PATH=/cache/bin:/data/harmony/bin:\$PATH" > "$prof"; chmod 644 "$prof" 2>/dev/null || true; done'
& ssh @sshArgs $Target "$profCmd"

# 2. Upload scripts
Write-Host "[*] Uploading scripts (MD5 check)..." -ForegroundColor Yellow
$scripts = Get-ChildItem -Path $ScriptsDir -File | Where-Object { $_.Name -notmatch '^\.' }
foreach ($s in $scripts) {
    Install-HubFileIfChanged -LocalFile $s.FullName -RemoteFile "/data/harmony/$($s.Name)" -Perm "755" | Out-Null
}

# 2b. Upload USB Ethernet kernel modules to /cache/modules (symlinked to /data/harmony/modules)
if (Test-Path $ModulesDir) {
    $modFiles = Get-ChildItem -Path $ModulesDir -Filter *.ko -File
    if ($modFiles) {
        Write-Host "[*] Syncing USB Ethernet kernel modules (/cache/modules)..." -ForegroundColor Yellow
        & ssh @sshArgs $Target "mkdir -p /cache/modules 2>/dev/null; [ -L /data/harmony/modules ] || ln -sf /cache/modules /data/harmony/modules"
        foreach ($m in $modFiles) {
            Install-HubFileIfChanged -LocalFile $m.FullName -RemoteFile "/cache/modules/$($m.Name)" -Perm "644" | Out-Null
        }
    }
}

# 3. Upload Web UI static bundle (gzip assets, only when MD5 differs) -> Store on /cache/www
if (Test-Path $WwwDir) {
    Write-Host "[*] Building & compressing Web UI assets with build_webui.ps1..." -ForegroundColor Yellow
    $buildScript = Join-Path $ScriptDir "build_webui.ps1"
    if (Test-Path $buildScript) {
        & powershell -ExecutionPolicy Bypass -File $buildScript
        $WwwDir = Join-Path $PayloadDir "www"
    }
    Write-Host "[*] Syncing Web UI static assets (/cache/www, MD5 check)..." -ForegroundColor Yellow
    & ssh @sshArgs $Target "mkdir -p /cache/www/assets /cache/www/assets/icons 2>/dev/null; if [ -d /data/harmony/www ] && [ ! -L /data/harmony/www ]; then rm -rf /data/harmony/www 2>/dev/null; fi; mkdir -p /data/harmony 2>/dev/null; [ -L /data/harmony/www ] || ln -sfn /cache/www /data/harmony/www 2>/dev/null; rm -f /cache/www/*.js /cache/www/*.css /cache/www/assets/*.js /cache/www/assets/*.css 2>/dev/null || true"
    $wwwFiles = Get-ChildItem -Path $WwwDir -Recurse -File | Where-Object { $_.FullName -notmatch '[\\/]icons([\\/]|$)' }
    $localRelNames = @()
    foreach ($f in $wwwFiles) {
        $relPath = $f.FullName.Substring($WwwDir.Length).TrimStart('\', '/') -replace '\\', '/'
        $remotePath = "/cache/www/$relPath"
        $localRelNames += $remotePath
        Install-HubFileIfChanged -LocalFile $f.FullName -RemoteFile $remotePath -Perm "644" | Out-Null
    }
    # Prune orphaned bundles on Hub (prevent hash rotation accumulation)
    $activeAssetNames = $wwwFiles | Where-Object { $_.DirectoryName -match 'assets' } | ForEach-Object { $_.Name }
    if ($activeAssetNames) {
        $activeCases = ($activeAssetNames | ForEach-Object { "$_*" }) -join '|'
        $pruneCmd = "if [ -d /cache/www/assets ]; then cd /cache/www/assets && for f in index-*.js* index-*.css* web-*.js*; do [ -f `"`$f`" ] || continue; case `"`$f`" in $activeCases) ;; *) rm -f `"`$f`" ;; esac; done; fi"
        & ssh @sshArgs $Target $pruneCmd
    }
    # Upload icons archive to /cache/icons.tar.gz (preserve /data space) and symlink to /data/harmony/icons.tar.gz
    $hasRemoteIcons = $remoteHashes.ContainsKey("/cache/icons.tar.gz")
    if ($hasRemoteIcons) {
        Write-Host "    -> icons.tar.gz (present on Hub - skipped)" -ForegroundColor DarkGray
        & ssh @sshArgs $Target "if [ -f /data/harmony/icons.tar.gz ] && [ ! -L /data/harmony/icons.tar.gz ]; then rm -f /data/harmony/icons.tar.gz 2>/dev/null; fi; [ -L /data/harmony/icons.tar.gz ] || ln -sf /cache/icons.tar.gz /data/harmony/icons.tar.gz 2>/dev/null"
    }
    else {
        $iconsTar = $null
        $fwTool = $null
        if ($RepoRoot -and (Test-Path (Join-Path $RepoRoot "build\icons.tar.gz"))) {
            $iconsTar = Join-Path $RepoRoot "build\icons.tar.gz"
        }
        elseif (Test-Path (Join-Path $ScriptDir "icons.tar.gz")) {
            $iconsTar = Join-Path $ScriptDir "icons.tar.gz"
        }
        else {
            # Release bundle ships tools\harmony_firmware.py next to this script; a repo checkout has it under the repo root
            if (Test-Path (Join-Path $ScriptDir "tools\harmony_firmware.py")) {
                $fwTool = Join-Path $ScriptDir "tools\harmony_firmware.py"
                $archiveOut = Join-Path $ScriptDir "icons.tar.gz"
            }
            elseif ($RepoRoot -and (Test-Path (Join-Path $RepoRoot "tools\harmony_firmware.py"))) {
                $fwTool = Join-Path $RepoRoot "tools\harmony_firmware.py"
                $archiveOut = Join-Path $RepoRoot "build\icons.tar.gz"
            }
        }
        if ($fwTool) {
            Write-Host "`n[?] $archiveOut not found on host." -ForegroundColor Yellow
            $resp = Read-Host "Download official Elite firmware, extract 1,350 icons, and build $archiveOut now? (Y/n)"
            if ($resp -eq "" -or $resp.Trim().ToLower() -eq "y") {
                Write-Host "[*] Extracting remote UI icons and packaging $archiveOut..." -ForegroundColor Cyan
                $pyCmd = if (Get-Command python -ErrorAction SilentlyContinue) { "python" } elseif (Get-Command python3 -ErrorAction SilentlyContinue) { "python3" } else { "py" }
                & $pyCmd $fwTool extract-icons --archive $archiveOut
                if (Test-Path $archiveOut) {
                    $iconsTar = $archiveOut
                }
            }
        }
        if ($iconsTar -and (Test-Path $iconsTar)) {
            Install-HubFileIfChanged -LocalFile $iconsTar -RemoteFile "/cache/icons.tar.gz" -Perm "644" | Out-Null
            & ssh @sshArgs $Target "if [ -f /data/harmony/icons.tar.gz ] && [ ! -L /data/harmony/icons.tar.gz ]; then rm -f /data/harmony/icons.tar.gz 2>/dev/null; fi; [ -L /data/harmony/icons.tar.gz ] || ln -sf /cache/icons.tar.gz /data/harmony/icons.tar.gz 2>/dev/null"
        }
    }
    # Prune individual icons directory on flash to preserve JFFS2 partition space
    & ssh @sshArgs $Target "rm -rf /cache/www/assets/icons 2>/dev/null; chmod 755 /cache/www /cache/www/assets 2>/dev/null || true"
}

# 4. Update /etc/init.d/rcS.local
$rcsSrc = Join-Path $ScriptsDir "rcS.local"
if (Test-Path $rcsSrc) {
    Write-Host "[*] Linking startup script /etc/init.d/rcS.local (MD5 check)..." -ForegroundColor Yellow
    Install-HubFileIfChanged -LocalFile $rcsSrc -RemoteFile "/etc/init.d/rcS.local" -Perm "755" | Out-Null
}

# 5. Post-deployment: safe reboot OR hot service restart (default)
if ($Reboot) {
    Write-Host "[*] Pre-reboot verification and safe reboot (flush SPI NOR flash)..." -ForegroundColor Yellow
    $rebootCmd = 'if ! /bin/sh -c "true" 2>/dev/null; then echo "[-] CRITICAL: /bin/sh verification failed. Aborting reboot!"; exit 1; fi; chmod +x /data/harmony/safe_reboot.sh 2>/dev/null || true; ln -sf /data/harmony/safe_reboot.sh /data/harmony/bin/safe_reboot.sh 2>/dev/null || true; ln -sf /data/harmony/bin/hub_sync /data/harmony/bin/sync 2>/dev/null || true; ln -sf /data/harmony/bin/hub_sync /cache/bin/sync 2>/dev/null || true; (sleep 2 && /data/harmony/safe_reboot.sh) >/dev/null 2>&1 &'
    & ssh @sshArgs $Target "$rebootCmd"
    Write-Host "[+] Deployment complete! Hub is rebooting safely." -ForegroundColor Green
}
else {
    Write-Host "[*] Flushing NOR flash buffers and hot-restarting services (no reboot)..." -ForegroundColor Yellow
    $restartCmd = @'
chmod +x /data/harmony/bin/* /data/harmony/*.sh /data/harmony/init.sh 2>/dev/null || true
mkdir -p /data/harmony/scripts 2>/dev/null || true
ln -sf /data/harmony/init.sh /data/harmony/scripts/init.sh 2>/dev/null || true
ln -sf /data/harmony/bin/hub_sync /data/harmony/bin/sync 2>/dev/null || true
ln -sf /data/harmony/bin/hub_sync /cache/bin/sync 2>/dev/null || true
/data/harmony/bin/hub_sync 2>/dev/null || /cache/bin/sync 2>/dev/null || sync 2>/dev/null || true
rm -f /tmp/harmony_stop 2>/dev/null || true
if [ -x /data/harmony/init.sh ]; then
    /data/harmony/init.sh start >/dev/null 2>&1 &
fi
'@.Replace("`r", "")
    & ssh @sshArgs $Target "$restartCmd"
    Start-Sleep -Seconds 3
    $runningPids = & ssh @sshArgs $Target "pidof hub_daemon hub_rf hub_btstack 2>/dev/null || true"
    if ($runningPids) {
        Write-Host "[+] Deployment complete! Hub services running (PIDs: $runningPids)." -ForegroundColor Green
    }
    else {
        Write-Host "[+] Deployment complete! Services started." -ForegroundColor Green
    }
}
