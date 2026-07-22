<#
    SA2Extractor build script.

    Usage:
        powershell -ExecutionPolicy Bypass -File build.ps1
        powershell -ExecutionPolicy Bypass -File build.ps1 -Config Debug
        powershell -ExecutionPolicy Bypass -File build.ps1 -Clean

    The MSVC 2019 Build Tools are not on PATH on this machine, so this script
    imports the vcvars64 environment before invoking the CMake that ships with
    the Build Tools.
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Config = 'Release',

    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

# --- Absolute toolchain paths (MSVC 2019 Build Tools) ----------------------
$VcVars   = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$CMakeExe = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$NinjaDir = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'

$ProjectRoot = $PSScriptRoot
if ([string]::IsNullOrEmpty($ProjectRoot)) {
    $ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Definition
}
$BuildDir = Join-Path $ProjectRoot 'build'

# --- Validate toolchain ----------------------------------------------------
foreach ($p in @($VcVars, $CMakeExe)) {
    if (-not (Test-Path -LiteralPath $p)) {
        Write-Error "Required tool not found: $p"
        exit 1
    }
}

Write-Host "=== SA2Extractor build ===" -ForegroundColor Cyan
Write-Host "Project : $ProjectRoot"
Write-Host "Config  : $Config"
Write-Host "CMake   : $CMakeExe"

if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
    Write-Host "Cleaning $BuildDir ..." -ForegroundColor Yellow
    Remove-Item -LiteralPath $BuildDir -Recurse -Force -Confirm:$false
}
if (-not (Test-Path -LiteralPath $BuildDir)) {
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
}

# --- Import the vcvars64 environment into this PowerShell session ----------
Write-Host "Importing MSVC x64 environment ..." -ForegroundColor Cyan
$vcvarsCmd = '"{0}" && set' -f $VcVars
$envLines  = & $env:ComSpec /c $vcvarsCmd

if ($LASTEXITCODE -ne 0) {
    Write-Error "vcvars64.bat failed with exit code $LASTEXITCODE"
    exit 1
}

foreach ($line in $envLines) {
    # 'set' output is NAME=VALUE; ignore any banner/warning noise.
    if ($line -match '^([^=]+)=(.*)$') {
        Set-Item -Path ("Env:" + $Matches[1]) -Value $Matches[2]
    }
}

if (-not $env:VSCMD_ARG_TGT_ARCH) {
    Write-Error "MSVC environment did not initialize (VSCMD_ARG_TGT_ARCH unset)."
    exit 1
}
Write-Host "  target arch: $($env:VSCMD_ARG_TGT_ARCH)"

# Ninja ships with the Build Tools' CMake but is not added to PATH by vcvars.
if (Test-Path -LiteralPath $NinjaDir) {
    $env:PATH = $NinjaDir + ';' + $env:PATH
}

# --- Configure -------------------------------------------------------------
# NOTE: the -D arguments MUST be quoted strings. An unquoted PowerShell token
# such as -DCMAKE_BUILD_TYPE=$Config is passed through literally (the variable
# is NOT expanded), which poisons the CMake cache with the text "$Config".
Write-Host "Configuring ..." -ForegroundColor Cyan
$configureArgs = @(
    '-S', $ProjectRoot,
    '-B', $BuildDir,
    '-G', 'Ninja',
    "-DCMAKE_BUILD_TYPE=$Config",
    '-DCMAKE_POLICY_DEFAULT_CMP0091=NEW'
)
& $CMakeExe @configureArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake configure failed (exit $LASTEXITCODE)."
    exit $LASTEXITCODE
}

# --- Build -----------------------------------------------------------------
Write-Host "Building ..." -ForegroundColor Cyan
& $CMakeExe --build $BuildDir --config $Config
if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed (exit $LASTEXITCODE)."
    exit $LASTEXITCODE
}

# --- Report ----------------------------------------------------------------
$BinDir = Join-Path $BuildDir 'bin'
Write-Host ""
Write-Host "=== Build succeeded ===" -ForegroundColor Green
if (Test-Path -LiteralPath $BinDir) {
    Get-ChildItem -LiteralPath $BinDir -Filter *.exe |
        ForEach-Object { Write-Host ("  {0}  ({1:N0} bytes)" -f $_.FullName, $_.Length) }
}
exit 0
