<#
.SYNOPSIS
    Builds the Web UI / Android frontend via Vite and prepares compressed assets in payload/www.
#>
param()

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RootDir = Split-Path -Parent $ScriptDir
$AppDir = Join-Path $RootDir "android-app"
$WwwDir = Join-Path (Join-Path $RootDir "payload") "www"

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  Building Web UI Static Assets (Vite & Gzip Compression)   " -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

# 1. Run Vite build
Write-Host "[*] Compiling frontend bundle with Vite..." -ForegroundColor Yellow
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
    Get-ChildItem -Path $AssetsDir -File | Where-Object { $_.Name -match '^index-|^web-' } | Remove-Item -Force
}
Copy-Item -Recurse -Force (Join-Path $DistDir "*") $WwwDir
if (Test-Path (Join-Path $AppDir "favicon.png")) {
    Copy-Item -Force (Join-Path $AppDir "favicon.png") (Join-Path $WwwDir "favicon.png")
}

# 3. Gzip compress assets for embedded HTTP server
Write-Host "[*] Gzipping assets (.gz) for fast embedded delivery..." -ForegroundColor Yellow
$pyScriptPath = Join-Path $ScriptDir "gzip_assets.py"
& python $pyScriptPath $WwwDir

Write-Host "[+] Web UI assets built and compressed successfully in payload/www!" -ForegroundColor Green
