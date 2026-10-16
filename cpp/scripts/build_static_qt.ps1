param(
    [string]$Version = '6.11.0',
    [ValidateSet('x64', 'arm64')][string]$Arch = 'x64'
)
# Builds static qtbase, qtsvg and qtimageformats from the official sources into .sdk\<version>\msvc2022_64_static
# (or msvc2022_arm64_static), the SDK for the single-file build (build.ps1 -Static). On an existing SDK
# only the modules it lacks (no SBOM of theirs) are built.
# ARM64 is cross-compiled on x64 and runs the tools of the x64 static build, so build that first.
$ErrorActionPreference = 'Stop'
$env:VSLANG = '1033'
$projectRoot = Split-Path $PSScriptRoot -Parent
$sdk = Join-Path $projectRoot '.sdk'
$hostQt = Join-Path $sdk "$Version\msvc2022_64_static"
$prefix = if ($Arch -eq 'arm64') { Join-Path $sdk "$Version\msvc2022_arm64_static" } else { $hostQt }
if ($Arch -eq 'arm64' -and -not (Test-Path -LiteralPath (Join-Path $hostQt 'bin\moc.exe'))) { throw "Build the x64 static Qt first: $hostQt" }
$sources = Join-Path $sdk "src\$Version"
$build = Join-Path $sdk "build-static-$Version-$Arch"
$base = "https://download.qt.io/official_releases/qt/$($Version.Substring(0, $Version.LastIndexOf('.')))/$Version/submodules"
# Added to qtbase with qt-configure-module; qtimageformats brings the WebP and TIFF plugins.
$addons = 'qtsvg', 'qtimageformats'

$ProgressPreference = 'SilentlyContinue'
New-Item -ItemType Directory -Force $sources | Out-Null
$sumsFile = Join-Path $sources 'md5sums.txt'
Invoke-WebRequest -UseBasicParsing "$base/md5sums.txt" -OutFile $sumsFile
$sums = Get-Content -LiteralPath $sumsFile
foreach ($module in @('qtbase') + $addons) {
    $name = "$module-everywhere-src-$Version"
    if (Test-Path -LiteralPath (Join-Path $sources $name)) { continue }
    $archive = Join-Path $sources "$name.tar.xz"
    Invoke-WebRequest -UseBasicParsing "$base/$name.tar.xz" -OutFile $archive
    $expected = ($sums | Where-Object { $_ -match "\s$name\.tar\.xz$" }) -replace '\s.*$', ''
    $actual = (Get-FileHash -Algorithm MD5 -LiteralPath $archive).Hash.ToLowerInvariant()
    if (-not $expected -or $actual -ne $expected) { throw "$name.tar.xz checksum mismatch" }
    & tar.exe -xf $archive -C $sources
    if ($LASTEXITCODE -ne 0) { throw "extracting $name failed" }
    Remove-Item -LiteralPath $archive
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'Install Visual Studio C++ Desktop tools first.' }
Import-Module (Join-Path $vsRoot 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments "-arch=$Arch -host_arch=x64" | Out-Null
$cmakeBin = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$env:PATH = "$cmakeBin\CMake\bin;$cmakeBin\Ninja;$env:PATH"
# Windows PowerShell 5 turns native stderr into terminating errors; check exit codes instead.
$ErrorActionPreference = 'Continue'

function Invoke-Step([string]$What, [scriptblock]$Step) {
    & $Step
    if ($LASTEXITCODE -ne 0) { throw "$What failed: $LASTEXITCODE" }
}

# -static-runtime: no MSVC runtime DLLs next to the exe. -no-opengl: the app draws with the raster
# engine only. C++20: zxing-cpp is C++20, and MSVC refuses to link its coroutine ABI with C++17 code.
function Test-Installed([string]$Module) { Test-Path -LiteralPath (Join-Path $prefix "sbom\$Module-$Version.spdx") }
if (-not (Test-Installed 'qtbase')) {
    $qtbase = Join-Path $build 'qtbase'
    New-Item -ItemType Directory -Force $qtbase | Out-Null
    Push-Location $qtbase
    $cross = @()
    if ($Arch -eq 'arm64') { $cross = @('-platform', 'win32-arm64-msvc', '-qt-host-path', $hostQt, '--', '-DCMAKE_SYSTEM_NAME=Windows', '-DCMAKE_SYSTEM_PROCESSOR=ARM64') }
    Invoke-Step 'qtbase configure' { & (Join-Path $sources "qtbase-everywhere-src-$Version\configure.bat") -prefix $prefix -static -static-runtime -release -nomake examples -nomake tests -no-opengl -c++std c++20 @cross }
    Invoke-Step 'qtbase build' { & cmake --build . --parallel }
    Invoke-Step 'qtbase install' { & cmake --install . }
    Pop-Location
}

foreach ($module in $addons) {
    if (Test-Installed $module) { continue }
    $moduleBuild = Join-Path $build $module
    New-Item -ItemType Directory -Force $moduleBuild | Out-Null
    Push-Location $moduleBuild
    Invoke-Step "$module configure" { & (Join-Path $prefix 'bin\qt-configure-module.bat') (Join-Path $sources "$module-everywhere-src-$Version") }
    Invoke-Step "$module build" { & cmake --build . --parallel }
    Invoke-Step "$module install" { & cmake --install . }
    Pop-Location
}

New-Item -ItemType Directory -Force (Join-Path $prefix 'licenses') | Out-Null
foreach ($module in @('qtbase') + $addons) {
    Copy-Item -LiteralPath (Join-Path $sources "$module-everywhere-src-$Version\LICENSES") -Destination (Join-Path $prefix "licenses\$module") -Recurse -Force -ErrorAction Stop
}
if (Test-Path -LiteralPath $build) { Remove-Item -LiteralPath $build -Recurse -Force }
Write-Host "Ready: $prefix"
