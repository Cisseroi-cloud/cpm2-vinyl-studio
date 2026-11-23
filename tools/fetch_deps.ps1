$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$tp = Join-Path $root "third_party"

New-Item -ItemType Directory -Force (Join-Path $tp "stb"), (Join-Path $tp "nlohmann") | Out-Null

if (!(Test-Path (Join-Path $tp "imgui"))) {
    Write-Host "Cloning Dear ImGui (docking)..."
    git clone --depth 1 -b docking https://github.com/ocornut/imgui.git (Join-Path $tp "imgui")
}
if (!(Test-Path (Join-Path $tp "ImGuizmo"))) {
    Write-Host "Cloning ImGuizmo..."
    git clone --depth 1 https://github.com/CedricGuillemet/ImGuizmo.git (Join-Path $tp "ImGuizmo")
}
if (!(Test-Path (Join-Path $tp "nlohmann\json.hpp"))) {
    Write-Host "Downloading nlohmann/json..."
    Invoke-WebRequest -Uri "https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp" -OutFile (Join-Path $tp "nlohmann\json.hpp")
}
if (!(Test-Path (Join-Path $tp "stb\stb_image.h"))) {
    Invoke-WebRequest -Uri "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h" -OutFile (Join-Path $tp "stb\stb_image.h")
}
if (!(Test-Path (Join-Path $tp "stb\stb_image_write.h"))) {
    Invoke-WebRequest -Uri "https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h" -OutFile (Join-Path $tp "stb\stb_image_write.h")
}
if (!(Test-Path (Join-Path $tp "stb\stb_truetype.h"))) {
    Invoke-WebRequest -Uri "https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h" -OutFile (Join-Path $tp "stb\stb_truetype.h")
}

Write-Host "Dependances third_party pretes."
