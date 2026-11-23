param(
    [string]$ShapesJson = "work\shapes_hq.json",
    [string]$LayersOut = "work\hood_layers.txt",
    [double]$OffsetX = -0.172,
    [double]$OffsetY = -0.130,
    [double]$RegionScale = 0.195,
    [double]$CanvasRot = -90,
    [string]$BgRect = "-0.172,-0.130,0.245,0.295,255,255,255,255",
    [string]$ClipWhiteBox = "0.18,0.47,0.20,0.53",
    [int]$MaxLayers = 300,
    [switch]$CaptureAfter
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    python tools\convert_shapes.py `
        --input $ShapesJson --output $LayersOut `
        --offset-x $OffsetX --offset-y $OffsetY `
        --region-scale $RegionScale --canvas-rot $CanvasRot `
        --bg-rect $BgRect --clip-white-box $ClipWhiteBox `
        --max-layers $MaxLayers

    $absLayers = [System.IO.Path]::GetFullPath($LayersOut)
    "inject $absLayers" | Set-Content -Path "work\cmd.txt" -Encoding ASCII
    Remove-Item "work\inject_done.txt","work\vinyl_injector.log" -ErrorAction SilentlyContinue
    powershell -ExecutionPolicy Bypass -File "calibration\inject.ps1" -DllPath "tools\vinyl_injector\build\CPM2_Vinyls_Injector.dll"
    for ($i = 0; $i -lt 60 -and !(Test-Path "work\inject_done.txt"); $i++) { Start-Sleep -Milliseconds 250 }
    if (!(Test-Path "work\inject_done.txt")) { throw "Timeout en attente de inject_done.txt" }
    Get-Content "work\inject_done.txt"

    if ($CaptureAfter) {
        Start-Sleep -Seconds 2
        "capture" | Set-Content -Path "work\cmd.txt" -Encoding ASCII
        Remove-Item "work\inject_done.txt","work\vinyl_injector.log" -ErrorAction SilentlyContinue
        powershell -ExecutionPolicy Bypass -File "calibration\inject.ps1" -DllPath "tools\vinyl_injector\build\CPM2_Vinyls_Injector.dll"
        for ($i = 0; $i -lt 60 -and !(Test-Path "work\inject_done.txt"); $i++) { Start-Sleep -Milliseconds 250 }
        Write-Host "Capture: work\live_vinyl_rt.png"
    }
} finally {
    Pop-Location
}
