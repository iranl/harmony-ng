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
param(
    [string]$HubIp = "192.168.1.1",
    [string]$KeyPath = "",
    [switch]$DebugBusybox,
    [switch]$DevMode,
    [switch]$FactoryReset
)

$ErrorActionPreference = "Continue"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ScriptDir
$PayloadDir = Join-Path $RepoRoot "payload"
$BinDir = Join-Path $PayloadDir "bin"
$ScriptsDir = Join-Path $PayloadDir "scripts"
$ModulesDir = Join-Path $PayloadDir "modules"
$WwwDir = Join-Path $PayloadDir "www"

# Local Development: Fallback to build/output or android-app/dist if payload directories are empty
if (-not (Test-Path (Join-Path $BinDir "codex_daemon")) -and (Test-Path (Join-Path $RepoRoot "build\output\codex_daemon"))) {
    $BinDir = Join-Path $RepoRoot "build\output"
}
if (-not (Test-Path (Join-Path $WwwDir "index.html")) -and (Test-Path (Join-Path $RepoRoot "android-app\dist\index.html"))) {
    $WwwDir = Join-Path $RepoRoot "android-app\dist"
}

$Target = "root@$HubIp"

# Auto-detect SSH key if not specified
if (-not $KeyPath) {
    $candidates = @(
        (Get-ChildItem -Path "$HOME\.ssh\harmony_owner_*" -File -ErrorAction SilentlyContinue | Select-Object -First 1).FullName,
        (Get-ChildItem -Path "$HOME\.ssh\id_rsa" -File -ErrorAction SilentlyContinue | Select-Object -First 1).FullName,
        (Get-ChildItem -Path "$HOME\.ssh\id_ed25519" -File -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
    )
    foreach ($cand in $candidates) {
        if ($cand -and (Test-Path $cand)) {
            $KeyPath = $cand
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
    Write-Host "  This will remove all Codex software, restore original stock scripts," -ForegroundColor Yellow
    Write-Host "  and reboot the Hub back into official Logitech stock firmware." -ForegroundColor Yellow
    $ans = Read-Host "Type 'RESET' to confirm"
    if ($ans -ne "RESET") {
        Write-Host "[-] Confirmation mismatch. Aborting reset." -ForegroundColor Red
        exit 1
    }
    Write-Host "[*] Reverting Hub to stock (preserving config files)..." -ForegroundColor Yellow
    & ssh @sshArgs $Target "killall -9 init.sh codex_daemon codex_btstack codex_rf codex_dhcpd codex_portal hal luaworks luadraws lua 2>/dev/null || true; if [ -f /data/stock_backup/rcS.local.orig ]; then cp -f /data/stock_backup/rcS.local.orig /etc/init.d/rcS.local; chmod 755 /etc/init.d/rcS.local; echo '[+] Restored stock rcS.local'; else rm -f /etc/init.d/rcS.local; fi; chmod +x /usr/bin/watchdog /opt/luaworks/luaworks /usr/bin/hal /usr/sbin/bluetoothd 2>/dev/null || true; rm -f /etc/nowatchdog /usr/sbin/dropbear /usr/sbin/dropbearkey /data/rf_pair_test /data/test_hub_tx /data/codex_rf 2>/dev/null || true; mkdir -p /data/codex_p 2>/dev/null; cp -rf /data/codex/config/* /data/codex_p/ 2>/dev/null || true; rm -rf /data/codex/bin /data/codex/www /data/codex-backups /cache/*.log /cache/crashlog-*.json 2>/dev/null || true; rm -f /data/codex/*.conf /data/codex/current_activity /data/codex/config_version 2>/dev/null || true; mkdir -p /data/codex/config 2>/dev/null; cp -rf /data/codex_p/* /data/codex/config/ 2>/dev/null; rm -rf /data/codex_p 2>/dev/null; sync 2>/dev/null || true; reboot -f 2>/dev/null &"
    Write-Host "[+] Factory reset complete! Hub is rebooting into stock firmware (configs preserved)." -ForegroundColor Green
    exit 0
}

$scpArgs = @("-O") + $sshArgs

Write-Host "[*] Preserving stock configuration backup..." -ForegroundColor Yellow
& ssh @sshArgs $Target "mkdir -p /data/stock_backup; if [ -f /etc/init.d/rcS.local ] && [ ! -f /data/stock_backup/rcS.local.orig ]; then if ! grep -q 'codex' /etc/init.d/rcS.local 2>/dev/null; then cp -f /etc/init.d/rcS.local /data/stock_backup/rcS.local.orig; echo '[+] Saved /etc/init.d/rcS.local to /data/stock_backup/rcS.local.orig'; fi; fi"

Write-Host "[*] Stopping existing daemons (killall -9)..." -ForegroundColor Yellow
& ssh @sshArgs $Target "killall -9 init.sh codex_daemon codex_btstack codex_rf codex_dhcpd codex_portal hal luaworks luadraws lua 2>/dev/null || true"

Write-Host "[*] Ensuring remote directories..." -ForegroundColor Yellow
& ssh @sshArgs $Target "mkdir -p /data/codex/bin /data/codex/www /data/codex/www/assets /etc/init.d /cache/www/assets/icons; [ -L /data/codex/www/assets/icons ] || ln -s /cache/www/assets/icons /data/codex/www/assets/icons"

& ssh @sshArgs $Target "rm -rf /data/codex-backups /data/codex/resource-backups /cache/crashlog-*.json 2>/dev/null; rm -f /data/rf_pair_test /data/test_hub_tx /data/codex_rf 2>/dev/null || true"

Write-Host "[*] Querying remote checksums (md5sum)..." -ForegroundColor Yellow
$remoteMd5Raw = & ssh @sshArgs $Target "md5sum /data/codex/bin/* /data/codex/* /data/codex/www/* /data/codex/www/assets/* /cache/bin/busybox /etc/init.d/rcS.local 2>/dev/null || true"
$remoteHashes = @{}
if ($remoteMd5Raw) {
    foreach ($line in $remoteMd5Raw) {
        $parts = $line.Trim() -split '\s+'
        if ($parts.Length -ge 2) {
            $remoteHashes[$parts[1].Trim()] = $parts[0].Trim().ToLower()
        }
    }
}

function Deploy-HubFileIfChanged {
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

# 1. Upload binaries (standard core binaries only)
Write-Host "[*] Uploading native MIPS binaries (MD5 check)..." -ForegroundColor Yellow
$bins = Get-ChildItem -Path $BinDir -File | Where-Object { 
    $_.Name -notmatch '\.txt$|^FILES$' -and $_.Name -ne 'busybox'
}
foreach ($b in $bins) {
    Deploy-HubFileIfChanged -LocalFile $b.FullName -RemoteFile "/data/codex/bin/$($b.Name)" -Perm "755" | Out-Null
}

# Extended BusyBox (default deployment)
$bb = Join-Path $BinDir "busybox"
if (Test-Path $bb) {
    Write-Host "[*] Syncing extended BusyBox to /cache/bin/..." -ForegroundColor Yellow
    & ssh @sshArgs $Target "mkdir -p /cache/bin 2>/dev/null"
    $changed = Deploy-HubFileIfChanged -LocalFile $bb -RemoteFile "/cache/bin/busybox" -Perm "755"
    if ($changed) {
        & ssh @sshArgs $Target "ln -sf /cache/bin/busybox /data/codex/bin/busybox 2>/dev/null; /cache/bin/busybox --install -s /cache/bin 2>/dev/null || true"
    }
}
# Ensure root profile uses extended BusyBox in cache if present
$profCmd = 'for prof in /root/.profile /home/root/.profile; do mkdir -p "$(dirname "$prof")" 2>/dev/null; cat <<''EOF'' > "$prof"
BB_PATH=""
if [ -x /cache/bin/busybox ]; then
    BB_PATH=/cache/bin/busybox
elif [ -x /cache/busybox ]; then
    BB_PATH=/cache/busybox
fi
if [ -n "$BB_PATH" ] && [ -z "$BB_EXTENDED" ]; then
    export BB_EXTENDED=1
    export PATH=$(dirname "$BB_PATH"):$PATH
    exec "$BB_PATH" sh -l
fi
export PATH=/cache/bin:/cache:/data/codex/bin:$PATH
EOF
chmod 644 "$prof" 2>/dev/null || true; done'
& ssh @sshArgs $Target "$profCmd"


# 2. Upload scripts
Write-Host "[*] Uploading scripts (MD5 check)..." -ForegroundColor Yellow
$scripts = Get-ChildItem -Path $ScriptsDir -File
foreach ($s in $scripts) {
    Deploy-HubFileIfChanged -LocalFile $s.FullName -RemoteFile "/data/codex/$($s.Name)" -Perm "755" | Out-Null
}

# 2b. Upload USB Ethernet kernel modules to /cache/modules (symlinked to /data/codex/modules)
if (Test-Path $ModulesDir) {
    $modFiles = Get-ChildItem -Path $ModulesDir -Filter *.ko -File
    if ($modFiles) {
        Write-Host "[*] Syncing USB Ethernet kernel modules (/cache/modules)..." -ForegroundColor Yellow
        & ssh @sshArgs $Target "mkdir -p /cache/modules 2>/dev/null; [ -L /data/codex/modules ] || ln -sf /cache/modules /data/codex/modules"
        foreach ($m in $modFiles) {
            Deploy-HubFileIfChanged -LocalFile $m.FullName -RemoteFile "/cache/modules/$($m.Name)" -Perm "644" | Out-Null
        }
    }
}

# 3. Upload Web UI static bundle (gzip assets, only when MD5 differs)
if (Test-Path $WwwDir) {
    Write-Host "[*] Building & compressing Web UI assets with build_webui.ps1..." -ForegroundColor Yellow
    $buildScript = Join-Path $ScriptDir "build_webui.ps1"
    if (Test-Path $buildScript) {
        & powershell -ExecutionPolicy Bypass -File $buildScript
    }
    Write-Host "[*] Syncing Web UI static assets (/data/codex/www, MD5 check)..." -ForegroundColor Yellow
    & ssh @sshArgs $Target "mkdir -p /data/codex/www/assets 2>/dev/null"
    $wwwFiles = Get-ChildItem -Path $WwwDir -Recurse -File | Where-Object { $_.FullName -notmatch '[\\/]icons([\\/]|$)' }
    $localRelNames = @()
    foreach ($f in $wwwFiles) {
        $relPath = $f.FullName.Substring($WwwDir.Length).TrimStart('\', '/') -replace '\\', '/'
        $remotePath = "/data/codex/www/$relPath"
        $localRelNames += $remotePath
        Deploy-HubFileIfChanged -LocalFile $f.FullName -RemoteFile $remotePath -Perm "644" | Out-Null
    }
    # Prune orphaned bundles on Hub (prevent hash rotation accumulation)
    $activeAssetNames = $wwwFiles | Where-Object { $_.DirectoryName -match 'assets' } | ForEach-Object { $_.Name }
    if ($activeAssetNames) {
        $activeCases = ($activeAssetNames | ForEach-Object { "$_*" }) -join '|'
        $pruneCmd = "if [ -d /data/codex/www/assets ]; then cd /data/codex/www/assets && for f in index-*.js* index-*.css* web-*.js*; do [ -f `"`$f`" ] || continue; case `"`$f`" in $activeCases) ;; *) rm -f `"`$f`" ;; esac; done; fi"
        & ssh @sshArgs $Target $pruneCmd
    }
    # Upload core static icons to /cache/www/assets/icons
    $iconDir = Join-Path $WwwDir "assets\icons"
    if (Test-Path $iconDir) {
        $iconFiles = Get-ChildItem -Path $iconDir -File
        if ($iconFiles) {
            Write-Host "[*] Syncing core Web UI icons (/cache/www/assets/icons)..." -ForegroundColor Yellow
            & ssh @sshArgs $Target "mkdir -p /cache/www/assets/icons 2>/dev/null"
            foreach ($ic in $iconFiles) {
                Deploy-HubFileIfChanged -LocalFile $ic.FullName -RemoteFile "/cache/www/assets/icons/$($ic.Name)" -Perm "644" | Out-Null
            }
        }
    }
    # If Elite icons are present on Hub, ensure flattened symlinks and semantic aliases
    & ssh @sshArgs $Target "if [ -d /cache/www/assets/icons ]; then cd /cache/www/assets/icons && for f in */*.png; do [ -f `"`$f`" ] || continue; base=`"`${f##*/}`"; [ ! -e `"`$base`" ] && ln -sf `"`$f`" `"`$base`" 2>/dev/null || true; done; [ -e juniperDevice/1.png ] && [ ! -e tv.png ] && ln -sf juniperDevice/1.png tv.png 2>/dev/null; [ -e notification/tv.png ] && ln -sf notification/tv.png tv.png 2>/dev/null; [ -e juniperDevice/5.png ] && [ ! -e receiver.png ] && ln -sf juniperDevice/5.png receiver.png 2>/dev/null; [ -e juniperDevice/default.png ] && [ ! -e generic.png ] && ln -sf juniperDevice/default.png generic.png 2>/dev/null; [ -e commands/116.png ] && [ ! -e poweroff.png ] && ln -sf commands/116.png poweroff.png 2>/dev/null; [ -e notification/switch.png ] && [ ! -e switch.png ] && ln -sf notification/switch.png switch.png 2>/dev/null; fi"
    & ssh @sshArgs $Target "chmod 755 /data/codex/www /data/codex/www/assets /cache/www/assets/icons 2>/dev/null || true"
}

# 4. Update /etc/init.d/rcS.local
$rcsSrc = Join-Path $ScriptsDir "rcS.local"
if (Test-Path $rcsSrc) {
    Write-Host "[*] Linking startup script /etc/init.d/rcS.local (MD5 check)..." -ForegroundColor Yellow
    Deploy-HubFileIfChanged -LocalFile $rcsSrc -RemoteFile "/etc/init.d/rcS.local" -Perm "755" | Out-Null
}

# 5. Restart services
Write-Host "[*] Launching services (/data/codex/init.sh)..." -ForegroundColor Yellow
& ssh @sshArgs $Target "/data/codex/init.sh >/dev/null 2>&1 &"
Start-Sleep -Seconds 2

Write-Host "[+] Deployment complete! Hub services running." -ForegroundColor Green
