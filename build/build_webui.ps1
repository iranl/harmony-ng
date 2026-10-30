<#
.SYNOPSIS
    Builds the Web UI frontend via Vite and prepares compressed assets in payload/www.
.PARAMETER Ui
    Choice of Web UI to build: 'new' (default, modular v3 in frontend/) or 'legacy' (original in android-app/).
#>
param(
    [ValidateSet('new', 'v3', 'frontend', 'legacy', 'old', 'android-app')]
    [string]$Ui = "legacy"
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RootDir = Split-Path -Parent $ScriptDir

$isLegacy = $Ui -in @('legacy', 'old', 'android-app')
$AppDir = if ($isLegacy) { Join-Path $RootDir "android-app" } else { Join-Path $RootDir "frontend" }
$UiLabel = if ($isLegacy) { "Legacy WebUI (android-app)" } else { "New Modular WebUI v3 (frontend)" }
$WwwDir = Join-Path (Join-Path $RootDir "payload") "www"

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  Building Web UI Static Assets (Vite & Gzip Compression)   " -ForegroundColor Cyan
Write-Host "  Selected UI: $UiLabel" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

# 1. Run Vite build
Write-Host "[*] Compiling frontend bundle with Vite in $AppDir..." -ForegroundColor Yellow
Push-Location $AppDir
try {
    & npm run build
    if ($LASTEXITCODE -ne 0) {
        throw "Vite build failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}

# 2. Sync to payload/www (preserving assets/icons)
$DistDir = Join-Path $AppDir "dist"
Write-Host "[*] Syncing compiled assets to $WwwDir..." -ForegroundColor Yellow
if (-not (Test-Path $WwwDir)) {
    New-Item -ItemType Directory -Path $WwwDir | Out-Null
}
Get-ChildItem -Path $WwwDir -File | Remove-Item -Force
$AssetsDir = Join-Path $WwwDir "assets"
if (Test-Path $AssetsDir) {
    Get-ChildItem -Path $AssetsDir -File | Remove-Item -Force
}
Copy-Item -Recurse -Force (Join-Path $DistDir "*") $WwwDir
if (Test-Path (Join-Path $AppDir "favicon.png")) {
    Copy-Item -Force (Join-Path $AppDir "favicon.png") (Join-Path $WwwDir "favicon.png")
}
# Flash safety: Icons live in /cache/icons.tar.gz and are unpacked to /tmp/www on Hub.
# Never copy individual icon files into payload/www flash partition.
$PayloadIconsDir = Join-Path $AssetsDir "icons"
if (Test-Path $PayloadIconsDir) {
    Remove-Item -Recurse -Force $PayloadIconsDir
}

# 3. Gzip compress assets for embedded HTTP server
Write-Host "[*] Gzipping assets (.gz) for fast embedded delivery..." -ForegroundColor Yellow
$pyScriptPath = Join-Path $ScriptDir "gzip_assets.py"
& python $pyScriptPath $WwwDir

Write-Host "[+] $UiLabel assets built and compressed successfully in payload/www!" -ForegroundColor Green
