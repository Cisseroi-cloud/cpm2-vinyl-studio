param(
    [string]$Config = "work\car187_config.json",
    [switch]$CaptureAfter
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    $cfg = Get-Content $Config -Raw | ConvertFrom-Json
    $carId = [int]$cfg.car_id
    $layersOut = $cfg.output
    $vinylType = if ($null -ne $cfg.vinyl_type) { [int]$cfg.vinyl_type } else { 1 }
    $plateSide = if ($null -ne $cfg.plate) { [string]$cfg.plate } else { "both" }
    $plateId   = if ($null -ne $cfg.plate_id) { [int]$cfg.plate_id } else { -1 }

    python tools\build_livery.py $Config

    $absLayers = [System.IO.Path]::GetFullPath($layersOut)
    $cmdLines = @(
        "inject $absLayers",
        "car=$carId",
        "type=$vinylType"
    )
    if ($vinylType -eq 2) {
        $cmdLines += "plate=$plateSide"
        if ($plateId -gt 0) { $cmdLines += "plateId=$plateId" }
    }
    if ($CaptureAfter) {
        $cmdLines += "capture=1"
    }

    $cmdLines | Set-Content -Path "work\cmd.txt" -Encoding ASCII
    Remove-Item "work\inject_done.txt","work\vinyl_injector.log" -ErrorAction SilentlyContinue
    powershell -ExecutionPolicy Bypass -File "calibration\inject.ps1" -DllPath "tools\vinyl_injector\build\CPM2_Vinyls_Injector.dll"
    for ($i = 0; $i -lt 80 -and !(Test-Path "work\inject_done.txt"); $i++) { Start-Sleep -Milliseconds 250 }
    if (!(Test-Path "work\inject_done.txt")) { throw "Timeout en attente de inject_done.txt" }
    Get-Content "work\inject_done.txt"
    if ($CaptureAfter) {
        Write-Host "Captures updated in work\live_*_rt.png"
    }
} finally {
    Pop-Location
}
