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

# Support running directly from extracted release archive or from repo build/ directory
if (Test-Path (Join-Path $ScriptDir "bin")) {
    $BinDir = Join-Path $ScriptDir "bin"
    $ScriptsDir = Join-Path $ScriptDir "scripts"
    $ModulesDir = Join-Path $ScriptDir "modules"
    $WwwDir = Join-Path $ScriptDir "www"
} else {
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
        $bbCmd = 'ln -sf /cache/bin/busybox /data/codex/bin/busybox 2>/dev/null; /cache/bin/busybox --install -s /cache/bin 2>/dev/null || true; for _app in $(/cache/bin/busybox --list 2>/dev/null); do [ ! -e "/bin/$_app" ] && [ ! -e "/usr/bin/$_app" ] && ln -sf /cache/bin/busybox "/usr/bin/$_app" 2>/dev/null || true; done; mkdir -p /etc/profile.d 2>/dev/null; echo "export PATH=/cache/bin:/data/codex/bin:\$PATH" > /etc/profile.d/00-codex.sh 2>/dev/null; chmod 644 /etc/profile.d/00-codex.sh 2>/dev/null || true'
        & ssh @sshArgs $Target "$bbCmd"
    }
}
# Ensure root profile uses extended BusyBox in cache if present
$profCmd = 'for prof in /root/.profile /home/root/.profile; do mkdir -p "$(dirname "$prof")" 2>/dev/null; echo "export PATH=/cache/bin:/data/codex/bin:\$PATH" > "$prof"; chmod 644 "$prof" 2>/dev/null || true; done'
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

# 3. Upload Web UI static bundle (gzip assets, only when MD5 differs) -> Store on /cache/www
if (Test-Path $WwwDir) {
    Write-Host "[*] Building & compressing Web UI assets with build_webui.ps1..." -ForegroundColor Yellow
    $buildScript = Join-Path $ScriptDir "build_webui.ps1"
    if (Test-Path $buildScript) {
        & powershell -ExecutionPolicy Bypass -File $buildScript
    }
    Write-Host "[*] Syncing Web UI static assets (/cache/www, MD5 check)..." -ForegroundColor Yellow
    & ssh @sshArgs $Target "mkdir -p /cache/www/assets /cache/www/assets/icons 2>/dev/null; if [ -d /data/codex/www ] && [ ! -L /data/codex/www ]; then rm -rf /data/codex/www 2>/dev/null; fi; mkdir -p /data/codex 2>/dev/null; [ -L /data/codex/www ] || ln -sfn /cache/www /data/codex/www 2>/dev/null"
    $wwwFiles = Get-ChildItem -Path $WwwDir -Recurse -File | Where-Object { $_.FullName -notmatch '[\\/]icons([\\/]|$)' }
    $localRelNames = @()
    foreach ($f in $wwwFiles) {
        $relPath = $f.FullName.Substring($WwwDir.Length).TrimStart('\', '/') -replace '\\', '/'
        $remotePath = "/cache/www/$relPath"
        $localRelNames += $remotePath
        Deploy-HubFileIfChanged -LocalFile $f.FullName -RemoteFile $remotePath -Perm "644" | Out-Null
    }
    # Prune orphaned bundles on Hub (prevent hash rotation accumulation)
    $activeAssetNames = $wwwFiles | Where-Object { $_.DirectoryName -match 'assets' } | ForEach-Object { $_.Name }
    if ($activeAssetNames) {
        $activeCases = ($activeAssetNames | ForEach-Object { "$_*" }) -join '|'
        $pruneCmd = "if [ -d /cache/www/assets ]; then cd /cache/www/assets && for f in index-*.js* index-*.css* web-*.js*; do [ -f `"`$f`" ] || continue; case `"`$f`" in $activeCases) ;; *) rm -f `"`$f`" ;; esac; done; fi"
        & ssh @sshArgs $Target $pruneCmd
    }
    # Upload icons archive to /data/codex/icons.tar.gz (served on-demand from RAM tmpfs)
    $iconsTar = $null
    if ($RepoRoot -and (Test-Path (Join-Path $RepoRoot "build\icons.tar.gz"))) {
        $iconsTar = Join-Path $RepoRoot "build\icons.tar.gz"
    } elseif (Test-Path (Join-Path $ScriptDir "icons.tar.gz")) {
        $iconsTar = Join-Path $ScriptDir "icons.tar.gz"
    }
    if ($iconsTar -and (Test-Path $iconsTar)) {
        Deploy-HubFileIfChanged -LocalFile $iconsTar -RemoteFile "/data/codex/icons.tar.gz" -Perm "644" | Out-Null
    }
    # Prune individual icons directory on flash to preserve JFFS2 partition space
    & ssh @sshArgs $Target "rm -rf /cache/www/assets/icons 2>/dev/null; chmod 755 /cache/www /cache/www/assets 2>/dev/null || true"
}

# 4. Update /etc/init.d/rcS.local
$rcsSrc = Join-Path $ScriptsDir "rcS.local"
if (Test-Path $rcsSrc) {
    Write-Host "[*] Linking startup script /etc/init.d/rcS.local (MD5 check)..." -ForegroundColor Yellow
    Deploy-HubFileIfChanged -LocalFile $rcsSrc -RemoteFile "/etc/init.d/rcS.local" -Perm "755" | Out-Null
}

# 5. Reboot Hub cleanly
Write-Host "[*] Rebooting Hub..." -ForegroundColor Yellow
& ssh @sshArgs $Target "sync 2>/dev/null || true; (sleep 1 && reboot -f) >/dev/null 2>&1 &"

Write-Host "[+] Deployment complete! Hub is rebooting." -ForegroundColor Green
