# 行覆盖率：单独做一份关掉优化的构建（优化后内联、合并会让行号映射失真），
# 用 VS 自带的 Microsoft.CodeCoverage.Console（没有时用 dotnet-coverage）包住 ctest 跑测试，按 src 下的文件汇总。
# CI 也用这份构建跑测试，不再另编 Release。
#   .\scripts\coverage.ps1                    # 构建并统计
#   .\scripts\coverage.ps1 -TestFilter gif    # 只跑名字匹配的测试
#   .\scripts\coverage.ps1 -TestExclude '_desktop$|_windows_parity$'   # 跳过名字匹配的测试
#   .\scripts\coverage.ps1 -SkipBuild         # 沿用上次的覆盖率构建
#   .\scripts\coverage.ps1 -BuildOnly         # 只构建
#   .\scripts\coverage.ps1 -SkipBuild -Append # 再跑一批测试，和上一批合并统计
#   .\scripts\coverage.ps1 -Jobs 4            # 并行跑测试（桌面测试共用屏幕和键鼠，别并行）
# 构建在 build\coverage-build，结果在 build\coverage\coverage.xml 与 summary.txt（按未覆盖行数排序）。
# 测试有失败时脚本照样写出统计，最后以非零退出。
param(
    [string]$QtRoot = '',
    [string]$TestFilter = '',
    [string]$TestExclude = '',
    [switch]$SkipBuild,
    [switch]$BuildOnly,
    [switch]$Append,
    [int]$Jobs = 1,
    [double]$FailUnder = 0
)
$ErrorActionPreference = 'Stop'
$env:VSLANG = '1033'
$projectRoot = Split-Path $PSScriptRoot -Parent
if (-not $QtRoot) { $QtRoot = Join-Path $projectRoot '.sdk\6.11.0\msvc2022_64' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
Import-Module (Join-Path $vsRoot 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$cmake = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest = Join-Path (Split-Path $cmake -Parent) 'ctest.exe'
$vsVersion = (& $vswhere -latest -products '*' -property installationVersion).Split('.')[0]
$generator = if ($vsVersion -eq '18') { 'Visual Studio 18 2026' } else { 'Visual Studio 17 2022' }
$buildDir = Join-Path $projectRoot 'build\coverage-build'
# RelWithDebInfo keeps the release runtime the Qt DLLs use; optimisation is off for exact line mapping.
$config = 'RelWithDebInfo'
$outDir = Join-Path $projectRoot 'build\coverage'
$rawDir = Join-Path $outDir 'raw'
$env:PATH = "$QtRoot\bin;$env:PATH"

if (-not $SkipBuild) {
    # A cache made with another Qt SDK keeps finding that SDK; configure from scratch then.
    $cache = Join-Path $buildDir 'CMakeCache.txt'
    if ((Test-Path -LiteralPath $cache) -and -not (Select-String -LiteralPath $cache -SimpleMatch "Qt6_DIR:PATH=$($QtRoot -replace '\\', '/')/" -Quiet)) {
        Remove-Item -LiteralPath $cache
    }
    # The Rust libraries are only exercised here, never shipped: no link-time optimisation.
    $env:CARGO_PROFILE_RELEASE_LTO = 'false'
    $env:CARGO_PROFILE_RELEASE_CODEGEN_UNITS = '16'
    # /Z7 keeps debug info in the objects, so parallel compiles do not queue on one PDB writer.
    & $cmake -S $projectRoot -B $buildDir -G $generator -A x64 "-DCMAKE_GENERATOR_INSTANCE=$vsRoot" "-DCMAKE_PREFIX_PATH=$QtRoot" '-DBUILD_TESTING=ON' `
        '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=/Z7 /Od /Ob0 /DNDEBUG' '-DCMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO=/DEBUG /INCREMENTAL:NO'
    if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
    & $cmake --build $buildDir --config $config --parallel
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
}
if ($BuildOnly) { return }

$console = Get-ChildItem (Join-Path $vsRoot 'Common7\IDE\Extensions\Microsoft\CodeCoverage.Console') -Filter 'Microsoft.CodeCoverage.Console.exe' -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
if (-not $console) { $console = (Get-Command dotnet-coverage -ErrorAction SilentlyContinue).Source }
if (-not $console) { throw 'Neither Microsoft.CodeCoverage.Console nor dotnet-coverage was found.' }
if (-not $Append -and (Test-Path $rawDir)) { Remove-Item -Recurse -Force $rawDir }
New-Item -ItemType Directory -Force $rawDir | Out-Null
$raw = Join-Path $rawDir ("run-{0}.xml" -f @(Get-ChildItem $rawDir -Filter '*.xml').Count)
$ctestArgs = @('--test-dir', $buildDir, '-C', $config, '--output-on-failure', '--timeout', '600', '-j', $Jobs)
if ($TestFilter) { $ctestArgs += @('-R', $TestFilter) }
if ($TestExclude) { $ctestArgs += @('-E', $TestExclude) }
& $console collect $ctest @ctestArgs -o $raw -f cobertura
$testsFailed = $LASTEXITCODE -ne 0
if (-not (Test-Path $raw)) { throw 'No coverage report was written.' }

# Line coverage per source file under src: a line counts once, covered if any process hit it.
$reports = @(Get-ChildItem $rawDir -Filter '*.xml' | ForEach-Object FullName)
& python (Join-Path $PSScriptRoot 'coverage_report.py') @reports --out (Join-Path $outDir 'coverage.xml') --summary (Join-Path $outDir 'summary.txt') --fail-under $FailUnder
$belowFloor = $LASTEXITCODE -ne 0
if ($testsFailed) { throw 'Some tests failed.' }
if ($belowFloor) { exit 1 }
