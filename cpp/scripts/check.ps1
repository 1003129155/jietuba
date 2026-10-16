# 分析构建：Asan 跑测试并抓内存错误；Analyze 编译时做静态分析（空指针、悬空、移动后使用）；
# Leak 用调试堆把每个测试跑三遍，报第二遍分配、第三遍跑完仍未释放的堆块（tests/leak_check.cpp）。
#   .\scripts\check.ps1 -Mode Asan
#   .\scripts\check.ps1 -Mode Analyze
#   .\scripts\check.ps1 -Mode Leak
# 各用独立构建目录，不影响 build\msvc 和 dist。
param(
    [Parameter(Mandatory)][ValidateSet('Asan', 'Analyze', 'Leak')][string]$Mode,
    [string]$QtRoot = '',
    [string]$TestFilter = ''
)
$ErrorActionPreference = 'Stop'
$env:VSLANG = '1033'
$projectRoot = Split-Path $PSScriptRoot -Parent
if (-not $QtRoot) { $QtRoot = Join-Path $projectRoot '.sdk\6.11.0\msvc2022_64' }
$QtRoot = (Resolve-Path -LiteralPath $QtRoot).Path
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'Install Visual Studio C++ Desktop tools first.' }
Import-Module (Join-Path $vsRoot 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$cmake = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest = Join-Path (Split-Path $cmake -Parent) 'ctest.exe'
$vsVersion = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion).Split('.')[0]
$generator = if ($vsVersion -eq '18') { 'Visual Studio 18 2026' } else { 'Visual Studio 17 2022' }
$buildDir = Join-Path $projectRoot "build\$($Mode.ToLower())"
$env:PATH = "$QtRoot\bin;$env:PATH"
# A cache made with another Qt SDK keeps finding that SDK; configure from scratch then.
$cache = Join-Path $buildDir 'CMakeCache.txt'
if ((Test-Path -LiteralPath $cache) -and -not (Select-String -LiteralPath $cache -SimpleMatch "Qt6_DIR:PATH=$($QtRoot -replace '\\', '/')/" -Quiet)) {
    Remove-Item -LiteralPath $cache
}
$flag = @{ Asan = '-DJIETUBA_SANITIZE=ON'; Analyze = '-DJIETUBA_ANALYZE=ON'; Leak = '-DJIETUBA_LEAK_CHECK=ON' }[$Mode]
# Asan 用 RelWithDebInfo：与 Qt 发行版同为 /MD，且带符号，报告里有行号。Leak 要调试堆，只能 Debug，连同 Qt 的 Debug 库。
$config = if ($Mode -eq 'Leak') { 'Debug' } else { 'RelWithDebInfo' }
& $cmake -S $projectRoot -B $buildDir -G $generator -A x64 "-DCMAKE_GENERATOR_INSTANCE=$vsRoot" "-DCMAKE_PREFIX_PATH=$QtRoot" '-DBUILD_TESTING=ON' '-DJIETUBA_PPOCR=OFF' $flag
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
# 项目没开 /MP，一个库的源文件默认逐个编；多工具任务让项目内部也并行。
& $cmake --build $buildDir --config $config --parallel -- /p:UseMultiToolTask=true /p:EnforceProcessCountAcrossBuilds=true
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
if ($Mode -ne 'Analyze') {
    # 运行库 clang_rt.asan_dynamic-x86_64.dll 在 VC 工具目录，开发者 shell 的 PATH 里已有。
    if ($Mode -eq 'Asan') { $env:ASAN_OPTIONS = 'halt_on_error=1:abort_on_error=0' }
    $ctestArgs = @('--test-dir', $buildDir, '-C', $config, '--output-on-failure')
    if ($TestFilter) { $ctestArgs += @('-R', $TestFilter) }
    & $ctest @ctestArgs
    if ($LASTEXITCODE -ne 0) { throw $(if ($Mode -eq 'Asan') { 'Tests failed under AddressSanitizer.' } else { 'Tests failed or leaked.' }) }
}
