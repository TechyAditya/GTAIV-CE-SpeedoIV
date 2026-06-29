# Build script for SpeedoIV-CE
# Requires 32-bit MinGW GCC (i686-w64-mingw32)

param(
    [string]$GCC = "",
    [string]$OutDir = "build"
)

# Auto-detect 32-bit GCC
if (-not $GCC) {
    $GCC = Get-Command "i686-w64-mingw32-g++.exe" -ErrorAction SilentlyContinue |
           Select-Object -ExpandProperty Source
}
if (-not $GCC) {
    # Check winget-installed MinGW
    $wingetPkg = "$env:TEMP\opencode\mingw32\mingw32\bin\g++.exe"
    if (Test-Path $wingetPkg) { $GCC = $wingetPkg }
}
if (-not $GCC) {
    Write-Error "32-bit MinGW GCC not found. Install i686 MinGW or pass -GCC path."
    exit 1
}

Write-Host "Using GCC: $GCC"

New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

$src = Join-Path $PSScriptRoot "speedometer.cpp"
$out = Join-Path $OutDir "SpeedoIV-CE.asi"
$gamePlugins = "J:\Games\Grand Theft Auto IV - The Complete Edition\plugins"

& $GCC $src `
    -shared `
    -o $out `
    -std=c++17 `
    -O2 `
    -s `
    -static `
    -ld3d9 `
    -lgdi32 `
    -luser32 `
    -lkernel32 `
    -Wl,--kill-at `
    -DUNICODE -D_UNICODE

if ($LASTEXITCODE -eq 0) {
    $size = (Get-Item $out).Length / 1KB
    Write-Host "Build OK: $out ($([math]::Round($size,1)) KB)"
    if (Test-Path $gamePlugins) {
        Copy-Item $out (Join-Path $gamePlugins "SpeedoIV-CE.asi") -Force
        Write-Host "Installed to: $gamePlugins\SpeedoIV-CE.asi"
    }
} else {
    Write-Error "Build failed."
    exit 1
}
