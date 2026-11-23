$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$src = Join-Path $PSScriptRoot "src_vinyl_injector.cpp"
$build = Join-Path $PSScriptRoot "build"
New-Item -ItemType Directory -Force $build | Out-Null
$out = Join-Path $build "CPM2_Vinyls_Injector.dll"
if (Test-Path $out) {
    try {
        Remove-Item $out -Force -ErrorAction Stop
    } catch {
        $prev = Join-Path $build ("CPM2_Vinyls_Injector.prev_" + [DateTime]::Now.Ticks + ".dll")
        Move-Item $out $prev -Force -ErrorAction SilentlyContinue
    }
}
$vs = (Get-ChildItem "C:\Program Files (x86)\Microsoft Visual Studio" -Recurse -Filter VsDevCmd.bat -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
if (!$vs) { throw "VsDevCmd.bat introuvable." }
$line = "call `"$vs`" -arch=x64 >nul && cd /d `"$build`" && cl /nologo /LD /EHsc /std:c++17 /O2 /utf-8 /DUNICODE /D_UNICODE `"$src`" /link /OUT:`"$out`" user32.lib"
cmd /c $line
if ($LASTEXITCODE -ne 0) { throw "Echec compilation (code $LASTEXITCODE)." }
Write-Host "Build OK: $out"
