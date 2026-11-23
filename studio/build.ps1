$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$studio = $PSScriptRoot
$build = Join-Path $studio "build"
New-Item -ItemType Directory -Force $build | Out-Null

$vs = (Get-ChildItem "C:\Program Files (x86)\Microsoft Visual Studio" -Recurse -Filter VsDevCmd.bat -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
if (!$vs) {
    $vs = (Get-ChildItem "C:\Program Files\Microsoft Visual Studio" -Recurse -Filter VsDevCmd.bat -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
}
if (!$vs) { throw "VsDevCmd.bat introuvable (Visual Studio Build Tools x64 requis)." }

$imgui = Join-Path $root "third_party\imgui"
$guizmo = Join-Path $root "third_party\ImGuizmo\src"
$srcDir = Join-Path $studio "src"
$outExe = Join-Path $build "cpm2_studio.exe"

$sources = @(
    (Join-Path $srcDir "main.cpp"),
    (Join-Path $srcDir "car_model.cpp"),
    (Join-Path $srcDir "projector.cpp"),
    (Join-Path $srcDir "converter.cpp"),
    (Join-Path $srcDir "garage_sync.cpp"),
    (Join-Path $srcDir "renderer_dx11.cpp"),
    (Join-Path $imgui "imgui.cpp"),
    (Join-Path $imgui "imgui_draw.cpp"),
    (Join-Path $imgui "imgui_tables.cpp"),
    (Join-Path $imgui "imgui_widgets.cpp"),
    (Join-Path $imgui "backends\imgui_impl_win32.cpp"),
    (Join-Path $imgui "backends\imgui_impl_dx11.cpp"),
    (Join-Path $guizmo "ImGuizmo.cpp")
)

$quotedSrc = ($sources | ForEach-Object { "`"$_`"" }) -join " "
$incFlags = "/I`"$imgui`" /I`"$imgui\backends`" /I`"$guizmo`" /I`"$root\third_party`""
$libs = "user32.lib gdi32.lib advapi32.lib comdlg32.lib shell32.lib d3d11.lib dxgi.lib d3dcompiler.lib"

$cmd = "call `"$vs`" -arch=x64 >nul && cd /d `"$build`" && cl /nologo /EHsc /std:c++17 /O2 /MP /utf-8 /DUNICODE /D_UNICODE $incFlags $quotedSrc /link /SUBSYSTEM:WINDOWS /OUT:`"$outExe`" $libs"
Write-Host "Compiling CPM2 Vinyl Studio (C++17 + DirectX 11 + Dear ImGui + ImGuizmo)..."
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "Echec compilation Studio (code $LASTEXITCODE)." }
Write-Host "Build OK: $outExe"
