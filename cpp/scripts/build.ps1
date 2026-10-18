param(
    [string]$QtRoot = '',
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [switch]$SkipTests,
    # Single-file lite build against the SDK from build_static_qt.ps1: no PP-OCR, no tests
    # (the same sources are tested by the normal build), deployed to dist\<Configuration>-static.
    [switch]$Static,
    # ARM64 is cross-compiled, for -Static only; it needs the x64 static SDK for its tools.
    [ValidateSet('x64', 'arm64')][string]$Arch = 'x64',
    # The updater helper (JIETUBA_UPDATES) is bundled in -Static builds, the released lite package;
    # -Updates adds it to the default build too, -NoUpdates leaves it out of either.
    [switch]$Updates,
    [switch]$NoUpdates,
    # The full package, jietuba_pp.exe, instead of the lite one: the single file also carries the
    # ONNX Runtime and the package gets the default PP-OCR models in models\. The DLL comes from
    # the model catalog (scripts\fetch_ort_runtime.ps1); -OrtDll uses a folder's onnxruntime.dll and
    # its onnxruntime-LICENSE.txt / onnxruntime-ThirdPartyNotices.txt instead.
    [switch]$Full,
    [string]$OrtDll = ''
)
if ($Arch -eq 'arm64' -and -not $Static) { throw 'ARM64 is built only as the single file: add -Static.' }
if ($Full -and -not $Static) { throw 'The full package is the single file: add -Static.' }
$Updates = ($Updates -or $Static) -and -not $NoUpdates
$ErrorActionPreference = 'Stop'
$env:VSLANG = '1033'
$projectRoot = Split-Path $PSScriptRoot -Parent
$hostQt = Join-Path $projectRoot '.sdk\6.11.0\msvc2022_64_static'
if (-not $QtRoot) { $QtRoot = Join-Path $projectRoot $(if ($Arch -eq 'arm64') { '.sdk\6.11.0\msvc2022_arm64_static' } elseif ($Static) { '.sdk\6.11.0\msvc2022_64_static' } else { '.sdk\6.11.0\msvc2022_64' }) }
if (-not (Test-Path -LiteralPath (Join-Path $QtRoot 'lib\cmake\Qt6\Qt6Config.cmake'))) {
    if ($Static) { throw 'Static Qt SDK missing. Run scripts\build_static_qt.ps1 first.' }
    throw 'Qt6 C++ SDK missing. Pass -QtRoot with an MSVC x64 Qt6 SDK directory.'
}
$QtRoot = (Resolve-Path -LiteralPath $QtRoot).Path
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'Install Visual Studio C++ Desktop tools first.' }
if ($Arch -eq 'arm64' -and -not (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath)) {
    throw 'Install the Visual Studio MSVC ARM64 build tools first.'
}
Import-Module (Join-Path $vsRoot 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments "-arch=$Arch -host_arch=x64" | Out-Null
$cmake = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest = Join-Path (Split-Path $cmake -Parent) 'ctest.exe'
$vsVersion = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion).Split('.')[0]
$generator = if ($vsVersion -eq '18') { 'Visual Studio 18 2026' } else { 'Visual Studio 17 2022' }
# MSBuild tracks headers independently of localized /showIncludes output. The
# local Chinese-only compiler made Ninja's detected prefix unusable.
$suffix = if ($Full) { '-full' } else { '' }
$buildDir = Join-Path $projectRoot $(if ($Arch -eq 'arm64') { "build\static-arm64$suffix" } elseif ($Static) { "build\static$suffix" } else { 'build\msvc' })
$configureArgs = @('-DBUILD_TESTING=ON')
if ($Static) {
    # The static Qt links the static MSVC runtime; every C/C++ and Rust object must match it.
    $configureArgs = @('-DBUILD_TESTING=OFF', '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded', '-DCMAKE_POLICY_DEFAULT_CMP0091=NEW')
    $env:RUSTFLAGS = '-C target-feature=+crt-static'
}
if ($Arch -eq 'arm64') { $configureArgs += "-DQT_HOST_PATH=$hostQt" }
$ortPath = ''
if ($Full) {
    $ortPath = if ($OrtDll) { (Resolve-Path -LiteralPath $OrtDll).Path } else { & (Join-Path $PSScriptRoot 'fetch_ort_runtime.ps1') -Arch $Arch -OutDir (Join-Path $projectRoot "build\ort\$Arch") }
    foreach ($name in 'onnxruntime-LICENSE.txt', 'onnxruntime-ThirdPartyNotices.txt') {
        if (-not (Test-Path -LiteralPath (Join-Path (Split-Path $ortPath -Parent) $name))) { throw "$name must be beside $ortPath; the full package ships ONNX Runtime." }
    }
}
# Passed every time so an earlier configuration cannot leave them on. The single file is the
# released lite package, or with -Full the full one, and takes that package's exe name.
$configureArgs += "-DJIETUBA_UPDATES=$(if ($Updates) { 'ON' } else { 'OFF' })"
$configureArgs += "-DJIETUBA_LITE_EXE=$(if ($Static -and -not $Full) { 'ON' } else { 'OFF' })"
$configureArgs += "-DJIETUBA_FULL_EXE=$(if ($Full) { 'ON' } else { 'OFF' })"
$configureArgs += "-DJIETUBA_ORT_DLL=$ortPath"
$env:PATH = "$QtRoot\bin;$env:PATH"
# A cache made with another Qt SDK keeps finding that SDK; configure from scratch then.
$cache = Join-Path $buildDir 'CMakeCache.txt'
if ((Test-Path -LiteralPath $cache) -and -not (Select-String -LiteralPath $cache -SimpleMatch "Qt6_DIR:PATH=$($QtRoot -replace '\\', '/')/" -Quiet)) {
    Remove-Item -LiteralPath $cache
}
& $cmake -S $projectRoot -B $buildDir -G $generator -A $(if ($Arch -eq 'arm64') { 'ARM64' } else { 'x64' }) "-DCMAKE_GENERATOR_INSTANCE=$vsRoot" "-DCMAKE_PREFIX_PATH=$QtRoot" @configureArgs
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $cmake --build $buildDir --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed.' }
if (-not $SkipTests -and -not $Static) {
    & $ctest --test-dir $buildDir -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
}
$exeName = (Select-String -LiteralPath $cache -Pattern '^JIETUBA_EXE_NAME:INTERNAL=(.+)$').Matches[0].Groups[1].Value + '.exe'
$deployDir = Join-Path $projectRoot $(if ($Arch -eq 'arm64') { "dist\$Configuration-static-arm64$suffix" } elseif ($Static) { "dist\$Configuration-static$suffix" } else { "dist\$Configuration" })
function Copy-Notices {
    Copy-Item -LiteralPath (Join-Path (Split-Path $projectRoot -Parent) 'LICENSE') -Destination $deployDir
    Copy-Item -LiteralPath (Join-Path $projectRoot 'THIRD-PARTY-NOTICES.md') -Destination $deployDir
    Copy-Item -LiteralPath (Join-Path $projectRoot 'licenses') -Destination $deployDir -Recurse -Force
    $licenseDir = Join-Path $deployDir 'licenses'
    # The repository's notices for oneocr and the icons, and those of the Rust libraries this build
    # links; its other license files describe PyPI packages, which this build does not ship.
    $reference = Split-Path $projectRoot -Parent
    foreach ($name in 'oneocr-LICENSE.txt', 'lucide-LICENSE.txt', 'tabler-icons-LICENSE.txt') {
        Copy-Item -LiteralPath (Join-Path $reference "licenses\$name") -Destination $licenseDir
    }
    # The lite exe does not carry ONNX Runtime, so its notices travel with the PP-OCR pack that
    # brings the DLL; the full exe does.
    if ($Full) {
        foreach ($name in 'onnxruntime-LICENSE.txt', 'onnxruntime-ThirdPartyNotices.txt') {
            Copy-Item -LiteralPath (Join-Path (Split-Path $ortPath -Parent) $name) -Destination $licenseDir
        }
    }
    $crates = @('gifrecorder', 'longstitch', 'pyclipboard', 'hdrcapture', 'inputhub', 'video_recorder', 'ppocr_rust')
    if ($Updates) { $crates += 'updater' }
    foreach ($crate in $crates) {
        Copy-Item -LiteralPath (Join-Path $reference "rust_libs\$crate\THIRD-PARTY-NOTICES.txt") -Destination (Join-Path $licenseDir "$crate-THIRD-PARTY-NOTICES.txt")
    }
    Copy-Item -LiteralPath (Join-Path $buildDir 'zxing-cpp-LICENSE.txt') -Destination $licenseDir
    # Qt files named as in the SBOM: the Qt libraries the exe links and the Qt DLLs deployed beside it.
    $project = [xml](Get-Content -LiteralPath (Join-Path $buildDir 'jietuba_cpp.vcxproj') -Raw)
    $linkInputs = $project.Project.ItemDefinitionGroup | Where-Object { $_.Condition -like "*'$Configuration|*" } | ForEach-Object { $_.Link.AdditionalDependencies }
    $qtFiles = @($linkInputs -split ';' | Where-Object { $_ -like "$QtRoot\*.lib" } | ForEach-Object { './' + $_.Substring($QtRoot.Length + 1).Replace('\', '/') })
    $qtFiles += @(Get-ChildItem -LiteralPath $deployDir -Recurse -Filter '*.dll' | ForEach-Object {
        $relative = $_.FullName.Substring($deployDir.Length + 1).Replace('\', '/')
        if ($relative.Contains('/')) { "./plugins/$relative" } elseif ($relative -like 'Qt6*') { "./bin/$relative" }
    })
    & (Join-Path $PSScriptRoot 'qt_notices.ps1') -QtRoot $QtRoot -Files ($qtFiles | Sort-Object -Unique) `
        -LicenseDir (Join-Path $projectRoot 'licenses\qt') -Output (Join-Path $licenseDir 'qt-THIRD-PARTY-NOTICES.txt')
}
# Start empty so files dropped from the deployment list do not linger.
if (Test-Path -LiteralPath $deployDir) { Remove-Item -LiteralPath $deployDir -Recurse -Force }
New-Item -ItemType Directory -Path $deployDir -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $buildDir "$Configuration\$exeName") -Destination $deployDir
if ($Static) {
    # Qt, its plugins and the runtime are linked in; only the exe and the notices ship.
    Copy-Notices
    if ($Full) {
        # The default models sit beside the exe; an update replaces only the exe and leaves models\ in place.
        $modelDir = Join-Path $deployDir 'models'
        New-Item -ItemType Directory -Path $modelDir | Out-Null
        foreach ($name in 'PP-OCRv6_det_small.onnx', 'PP-OCRv6_rec_small.onnx') {
            Copy-Item -LiteralPath (Join-Path (Split-Path $projectRoot -Parent) "models\$name") -Destination $modelDir
        }
    }
    Write-Host "Ready: $deployDir\$exeName"
    return
}
$mode = if ($Configuration -eq 'Release') { '--release' } else { '--debug' }
# Windows PowerShell 5 turns native stderr warnings into terminating errors
# when the caller redirects output. Check the tool's exit code instead.
$ErrorActionPreference = 'Continue'
# Widgets-only app: no shader compilers or network plugins.
# The CRT is copied app-local below instead of shipping the redist installer.
& (Join-Path $QtRoot 'bin\windeployqt.exe') $mode --no-compiler-runtime --no-translations --no-opengl-sw `
    --no-system-d3d-compiler --no-system-dxc-compiler `
    --skip-plugin-types 'networkinformation,tls,generic' `
    (Join-Path $deployDir $exeName)
$deployExit = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($deployExit -ne 0) { throw 'Qt deployment failed.' }
# windeployqt copies Qt Core's ICU dependency out of System32; it is part of Windows, not ours to ship.
Remove-Item -LiteralPath (Join-Path $deployDir 'icuuc.dll') -ErrorAction SilentlyContinue
$crtDir = Get-ChildItem -LiteralPath (Join-Path $env:VCToolsRedistDir 'x64') -Directory -Filter 'Microsoft.VC*.CRT' | Select-Object -First 1
if (-not $crtDir) { throw 'MSVC CRT redistributable directory missing.' }
foreach ($dll in 'msvcp140.dll', 'msvcp140_1.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') {
    Copy-Item -LiteralPath (Join-Path $crtDir.FullName $dll) -Destination $deployDir
}
Copy-Notices
Write-Host "Ready: $deployDir\$exeName"
