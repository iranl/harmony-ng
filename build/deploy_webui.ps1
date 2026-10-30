param(
    [string]$HubIp = "10.234.234.98",
    [string]$KeyPath = "$HOME\.ssh\id_rsa",
    [ValidateSet("new", "v3", "frontend", "legacy", "old", "android-app")][string]$Ui = "new",
    [switch]$SkipBuild,
    [switch]$RestartServer
)

$ErrorActionPreference = "Stop"

$deployScript = Join-Path $PSScriptRoot "deploy_hub.ps1"
if (-not (Test-Path $deployScript)) {
    Write-Error "deploy_hub.ps1 not found at $deployScript"
    exit 1
}

$deployParams = @{
    HubIp = $HubIp
    KeyPath = $KeyPath
    Ui = $Ui
    WebUiOnly = $true
}

if ($SkipBuild) {
    $deployParams["SkipBuild"] = $true
}
if ($RestartServer) {
    $deployParams["RestartServer"] = $true
}

& $deployScript @deployParams
