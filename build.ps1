# Build helper for Auto Time Ramp.
#   .\build.ps1            -> build the effect (Release|x64)
#   .\build.ps1 -Tests     -> build and run the RampMath unit tests
#   .\build.ps1 -Config Debug
#   .\build.ps1 -AETest    -> run tests\ae\selftest.jsx in the running AE 2026 and compare with RampMath
#                             (use an empty project; needs "Allow Scripts to Write Files" enabled)
param(
    [switch]$Tests,
    [switch]$AETest,
    [string]$AfterFX = 'C:\Program Files\Adobe\Adobe After Effects 2026\Support Files\AfterFX.exe',
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

if ($AETest) {
    $out = Join-Path $root 'build\tests'
    New-Item -ItemType Directory -Force $out | Out-Null
    Invoke-InVsEnv "cl /nologo /std:c++17 /EHsc /W4 /O2 `"$root\tests\ae\compare_selftest.cpp`" `"$root\src\core\RampMath.cpp`" /Fo`"$out\\`" /Fe`"$out\compare_selftest.exe`" >nul"
    # AfterFX -r silently ignores script paths containing spaces, so run from a temp folder.
    $tmp = Join-Path $env:TEMP 'atr_aetest'
    New-Item -ItemType Directory -Force $tmp | Out-Null
    Copy-Item "$root\tests\ae\selftest.jsx" $tmp -Force
    $csv = Join-Path $tmp 'selftest_result.csv'
    Remove-Item $csv -ErrorAction SilentlyContinue
    & $AfterFX -r (Join-Path $tmp 'selftest.jsx')
    $deadline = (Get-Date).AddMinutes(5)
    while (-not (Test-Path $csv) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
    if (-not (Test-Path $csv)) { throw 'No result from After Effects (is it running, with file access allowed for scripts?)' }
    Start-Sleep -Milliseconds 500
    & "$out\compare_selftest.exe" $csv
    if ($LASTEXITCODE -ne 0) { throw 'AE self-test failed.' }
    return
}

Invoke-InVsEnv "msbuild `"$root\win\AutoTimeRamp.vcxproj`" /nologo /v:minimal /p:Configuration=$Config /p:Platform=x64"
