param(
    [switch]$CreateZip
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root "dist\CPM2_Vinyl_Studio"

Write-Host "Cleaning previous dist folder: $dist"
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory -Force `
    (Join-Path $dist "bin"), `
    (Join-Path $dist "sprites_v4"), `
    (Join-Path $dist "tools"), `
    (Join-Path $dist "cache\cars"), `
    (Join-Path $dist "work") | Out-Null

# Copy ONLY clean runtime binaries, sprite masks, exporter script & documentation
# (ZERO personal images, ZERO work/art_v4_*, ZERO logs, ZERO personal studio_config.json!)
Copy-Item (Join-Path $root "studio\build\cpm2_studio.exe") (Join-Path $dist "cpm2_studio.exe")
Copy-Item (Join-Path $root "tools\geometrize_cli\build\geometrize_cli.exe") (Join-Path $dist "bin\geometrize_cli.exe")
Copy-Item (Join-Path $root "tools\vinyl_injector\build\CPM2_Vinyls_Injector.dll") (Join-Path $dist "bin\CPM2_Vinyls_Injector.dll")
Copy-Item (Join-Path $root "tools\export_car_3d.py") (Join-Path $dist "tools\export_car_3d.py")
Copy-Item (Join-Path $root "studio\README.md") (Join-Path $dist "README.md")

# Copy 20 high-res 256x256 sprite masks + metadata
Copy-Item (Join-Path $root "tools\geometrize_cli\sprites_v4\*.a256") (Join-Path $dist "sprites_v4\")
Copy-Item (Join-Path $root "tools\geometrize_cli\sprites_v4\sprites_meta.json") (Join-Path $dist "sprites_v4\")

# Create convenient double-click launcher
@"
@echo off
cd /d "%~dp0"
start "" "%~dp0cpm2_studio.exe"
"@ | Set-Content -Path (Join-Path $dist "Lancer_CPM2_Vinyl_Studio.bat") -Encoding ASCII

# Verify zero hardcoded personal paths or personal files leaked into dist/
$bad = Get-ChildItem $dist -Recurse -File | Where-Object {
    $_.Name -match "bluegirl|zero2|rem|miku|verify_|live_|studio_config|inject_done|\.log$|\.obj$"
}
if ($bad) {
    throw "Audit securite echoue : fichiers personnels trouves dans dist/: $($bad.Name -join ', ')"
}

$totalMB = ((Get-ChildItem $dist -Recurse -File | Measure-Object -Property Length -Sum).Sum / 1MB)
Write-Host ("Package portable propre cree : {0} ({1:N2} MB, 0 fichier personnel)" -f $dist, $totalMB)

if ($CreateZip) {
    $zipPath = Join-Path $root "dist\CPM2_Vinyl_Studio.zip"
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
    Compress-Archive -Path "$dist\*" -DestinationPath $zipPath -CompressionLevel Optimal
    Write-Host ("Archive ZIP prete a partager : {0} ({1:N2} MB)" -f $zipPath, ((Get-Item $zipPath).Length / 1MB))
}
