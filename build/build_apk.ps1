# Build script for Android APK
param (
    [switch]$Release
)

$ErrorActionPreference = "Stop"

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  Building Harmony Remote Android APK" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

$RepoRoot = Split-Path -Parent $PSScriptRoot
$AndroidAppDir = Join-Path $RepoRoot "android-app"
$AndroidDir = Join-Path $AndroidAppDir "android"
$OutputDir = Join-Path $RepoRoot "build\output"

if (-not (Test-Path $AndroidAppDir)) {
    Write-Error "android-app directory not found at $AndroidAppDir"
}

# Ensure compatible JAVA_HOME (e.g. Android Studio JBR 21 vs System Java 25)
$studioJbr = "C:\Program Files\Android\Android Studio\jbr"
if (Test-Path $studioJbr) {
    $env:JAVA_HOME = $studioJbr
    $env:PATH = "$studioJbr\bin;$env:PATH"
}

# 1. Build web bundle with Vite
Write-Host "[1/4] Building Vite web bundle..." -ForegroundColor Yellow
Push-Location $AndroidAppDir
try {
    npm run build
    if ($LASTEXITCODE -ne 0) { throw "Vite build failed" }
} finally {
    Pop-Location
}

# Also sync web assets to payload/www for Hub web server
Write-Host "[+] Syncing web assets to payload/www..." -ForegroundColor Green
Copy-Item -Path "$AndroidAppDir\dist\*" -Destination "$RepoRoot\payload\www\" -Recurse -Force
$gzipScript = Join-Path (Join-Path $RepoRoot "build") "gzip_assets.py"
if (Test-Path $gzipScript) {
    & python $gzipScript "$RepoRoot\payload\www"
}

# 2. Capacitor sync
Write-Host "[2/4] Running Capacitor sync..." -ForegroundColor Yellow
Push-Location $AndroidAppDir
try {
    npx cap sync android
    if ($LASTEXITCODE -ne 0) { throw "Capacitor sync failed" }
} finally {
    Pop-Location
}

# 3. Gradle build
Write-Host "[3/4] Running Gradle assemble..." -ForegroundColor Yellow
Push-Location $AndroidDir
try {
    $gradleTask = if ($Release) { "assembleRelease" } else { "assembleDebug" }
    cmd.exe /c "gradlew.bat $gradleTask"
    if ($LASTEXITCODE -ne 0) { throw "Gradle build failed" }
} finally {
    Pop-Location
}

# 4. Copy output APK
$flavor = if ($Release) { "release" } else { "debug" }
$apkName = if ($Release) { "app-release-unsigned.apk" } else { "app-debug.apk" }
$builtApk = Join-Path $AndroidDir "app\build\outputs\apk\$flavor\$apkName"

if (Test-Path $builtApk) {
    if (-not (Test-Path $OutputDir)) {
        New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
    }
    $destApk = Join-Path $OutputDir "app-$flavor.apk"
    Copy-Item -Path $builtApk -Destination $destApk -Force
    $sizeMb = [math]::Round((Get-Item $destApk).Length / 1MB, 2)
    Write-Host "============================================================" -ForegroundColor Cyan
    Write-Host "[+] SUCCESS! APK built:" -ForegroundColor Green
    Write-Host "    $destApk ($sizeMb MB)" -ForegroundColor Green
    Write-Host "============================================================" -ForegroundColor Cyan
} else {
    Write-Error "Could not find built APK at $builtApk"
}
