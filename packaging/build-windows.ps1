[CmdletBinding()]
param(
    [string]$QtDir = $env:QTDIR,
    [string]$BuildDir = "build-windows",
    [string]$OutputDir = "dist",
    [string]$WebPRuntimeDir,
    [switch]$Installer,
    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program failed with exit code $LASTEXITCODE"
    }
}
function Resolve-ProjectPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return $Path }
    return Join-Path $root $Path
}
$build = Resolve-ProjectPath $BuildDir
$output = Resolve-ProjectPath $OutputDir
$configure = @("-S", $root, "-B", $build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release")
if ($QtDir) {
    $QtDir = (Resolve-Path $QtDir).Path
    $env:PATH = (Join-Path $QtDir "bin") + ";" + $env:PATH
    $configure += "-DCMAKE_PREFIX_PATH=$QtDir"
}
if ($WebPRuntimeDir) {
    $configure += "-DIMPASTO_WEBP_RUNTIME_DIR=$((Resolve-Path $WebPRuntimeDir).Path)"
}
foreach ($tool in @("cmake", "ninja", "windeployqt")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool is missing. Run from the Qt/compiler development shell; see packaging/windows/BUILD.md."
    }
}
if ($Installer -and -not (Get-Command makensis -ErrorAction SilentlyContinue)) {
    $nsis = Join-Path ${env:ProgramFiles(x86)} "NSIS"
    if (Test-Path (Join-Path $nsis "makensis.exe")) {
        $env:PATH = $nsis + ";" + $env:PATH
    } else {
        throw "NSIS 3.03 or newer is required for -Installer. Install NSIS or omit -Installer for a portable ZIP."
    }
}
Invoke-Checked "cmake" $configure
Invoke-Checked "cmake" @("--build", $build, "--parallel")
if ($SelfTest) {
    $testOutput = Join-Path $build "selftest.png"
    Invoke-Checked (Join-Path $build "impasto.exe") @("--selftest", $testOutput)
}
New-Item -ItemType Directory -Force -Path $output | Out-Null
$generators = if ($Installer) { "ZIP;NSIS" } else { "ZIP" }
Invoke-Checked "cpack" @("--config", (Join-Path $build "CPackConfig.cmake"), "-C", "Release", "-G", $generators, "-B", $output)
Get-ChildItem $output -File | Where-Object { $_.Name -match '^Impasto-.*-windows-x64\.(zip|exe)$' } | ForEach-Object {
    $hash = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText($_.FullName + ".sha256", "$hash  $($_.Name)`n", [Text.UTF8Encoding]::new($false))
    Write-Output $_.FullName
}
