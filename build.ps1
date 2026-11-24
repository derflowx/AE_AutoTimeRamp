# Build helper for Auto Time Ramp.
#   .\build.ps1            -> build the effect (Release|x64)
#   .\build.ps1 -Tests     -> build and run the RampMath unit tests
#   .\build.ps1 -Config Debug
param(
    [switch]$Tests,
    [ValidateSet('Release', 'Debug')][string]$Config = 'Release'
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
# Prefer VS2022 (toolset v143, which the AE SDK samples use); fall back to newest with C++ tools.
$vs = & $vswhere -version '[17.0,18.0)' -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (-not $vs) {
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $vs) { throw 'No Visual Studio with C++ tools found.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

function Invoke-InVsEnv([string]$cmdline) {
    $bat = Join-Path $env:TEMP ("atr_build_{0}.cmd" -f [guid]::NewGuid())
    $vswhereDir = Split-Path $vswhere
    "@echo off`r`nset `"PATH=$vswhereDir;%PATH%`"`r`ncall `"$vcvars`" >nul`r`nif errorlevel 1 exit /b 1`r`n$cmdline`r`n" | Set-Content -Path $bat -Encoding ascii
    try {
        & cmd.exe /c $bat
        if ($LASTEXITCODE -ne 0) { throw "Command failed with exit code $LASTEXITCODE" }
    } finally { Remove-Item $bat -ErrorAction SilentlyContinue }
}

if ($Tests) {
    $out = Join-Path $root 'build\tests'
    New-Item -ItemType Directory -Force $out | Out-Null
    Invoke-InVsEnv "cl /nologo /std:c++17 /EHsc /W4 /WX /O2 `"$root\tests\test_rampmath.cpp`" `"$root\src\core\RampMath.cpp`" /Fo`"$out\\`" /Fe`"$out\test_rampmath.exe`""
    & "$out\test_rampmath.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
    return
}

Invoke-InVsEnv "msbuild `"$root\win\AutoTimeRamp.vcxproj`" /nologo /v:minimal /p:Configuration=$Config /p:Platform=x64"
