# build.ps1 - Build SpeedoIV-CE and (optionally) the dev tools
#
# Requires 32-bit MinGW GCC (i686-w64-mingw32) on PATH or at the bundled location.
#
# Usage:
#   .\build.ps1            -> build SpeedoIV-CE.asi
#   .\build.ps1 -Tools     -> also build every tools\*.cpp into build\*.exe
#   .\build.ps1 -ToolsOnly -> skip the ASI, just rebuild the tools

param(
    [string]$GCC = "",
    [string]$OutDir = "build",
    [switch]$Tools,
    [switch]$ToolsOnly,
    [switch]$NoInstall
)

# Auto-detect 32-bit GCC
if (-not $GCC) {
    $GCC = Get-Command "i686-w64-mingw32-g++.exe" -ErrorAction SilentlyContinue |
           Select-Object -ExpandProperty Source
}
if (-not $GCC) {
    $bundled = "$env:TEMP\opencode\mingw32\mingw32\bin\g++.exe"
    if (Test-Path $bundled) { $GCC = $bundled }
}
if (-not $GCC) {
    Write-Error "32-bit MinGW GCC not found. Install i686 MinGW or pass -GCC path."
    exit 1
}

New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
Write-Host "GCC: $GCC"

# --- ASI ---
if (-not $ToolsOnly) {
    $src = Join-Path $PSScriptRoot "speedometer.cpp"
    $out = Join-Path $OutDir "SpeedoIV-CE.asi"
    $asiArgs = @(
        $src,
        '-shared',
        '-o', $out,
        '-std=c++17',
        '-O2', '-s', '-static',
        '-I', $PSScriptRoot,
        '-ld3dx9_40', '-ld3d9',
        '-lgdi32', '-luser32', '-lkernel32',
        '-Wl,--kill-at'
    )
    & $GCC $asiArgs
    if ($LASTEXITCODE -ne 0) { Write-Error "Build of SpeedoIV-CE.asi failed"; exit 1 }
    $kb = [math]::Round((Get-Item $out).Length / 1KB, 1)
    Write-Host "Built: $out ($kb KB)"

    # Optional install to the game's plugins folder when ENVIRONMENT.md is set up
    $envPath = Join-Path $PSScriptRoot "ENVIRONMENT.md"
    if (-not $NoInstall -and (Test-Path $envPath)) {
        $envContent = Get-Content $envPath -Raw
        if ($envContent -match '(?m)^\s*GAME_DIR\s*=\s*"?([^"\r\n]+)"?') {
            $gameDir = $Matches[1].Trim()
            $pluginDir = Join-Path $gameDir "plugins"
            if (Test-Path $pluginDir) {
                Copy-Item $out (Join-Path $pluginDir "SpeedoIV-CE.asi") -Force
                Write-Host "Installed to $pluginDir"
            }
        }
    }
}

# --- Tools ---
if ($Tools -or $ToolsOnly) {
    Get-ChildItem (Join-Path $PSScriptRoot "tools") -Filter "*.cpp" | ForEach-Object {
        $exeName = [System.IO.Path]::ChangeExtension($_.Name, ".exe")
        $exePath = Join-Path $OutDir $exeName
        $toolArgs = @(
            $_.FullName,
            '-m32', '-O1', '-std=c++17', '-static',
            '-I', $PSScriptRoot,
            '-o', $exePath
        )
        & $GCC $toolArgs 2>&1 | Out-Null
        if ($LASTEXITCODE -eq 0) {
            Write-Host "  tool: $exeName"
        } else {
            Write-Warning "  tool FAILED: $exeName"
        }
    }
}
