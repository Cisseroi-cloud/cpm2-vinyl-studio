$ErrorActionPreference = "Stop"
$src = Join-Path $PSScriptRoot "src_geometrize_cli.cpp"
$build = Join-Path $PSScriptRoot "build"
New-Item -ItemType Directory -Force $build | Out-Null
$out = Join-Path $build "geometrize_cli.exe"
$vs = (Get-ChildItem "C:\Program Files (x86)\Microsoft Visual Studio" -Recurse -Filter VsDevCmd.bat -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
if (!$vs) { throw "VsDevCmd.bat introuvable (Visual Studio Build Tools x64 requis)." }
$line = "call `"$vs`" -arch=x64 >nul && cd /d `"$build`" && cl /nologo /EHsc /std:c++17 /O2 /fp:fast /utf-8 `"$src`" /link /OUT:`"$out`""
cmd /c $line
if ($LASTEXITCODE -ne 0) { throw "Echec compilation (code $LASTEXITCODE)." }
Write-Host "Build OK: $out"
