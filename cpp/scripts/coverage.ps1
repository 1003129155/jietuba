# 行覆盖率：单独做一份关掉优化的构建（优化后内联、合并会让行号映射失真），
# 用 VS 自带的 Microsoft.CodeCoverage.Console 包住 ctest 跑全部测试，按 src 下的文件汇总。
#   .\scripts\coverage.ps1                    # 构建并统计
#   .\scripts\coverage.ps1 -TestFilter gif    # 只跑名字匹配的测试
#   .\scripts\coverage.ps1 -SkipBuild         # 沿用上次的覆盖率构建
# 构建在 build\coverage-build，结果在 build\coverage\coverage.cobertura.xml 与 summary.txt（按未覆盖行数排序）。
param(
    [string]$QtRoot = '',
    [string]$TestFilter = '',
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$env:VSLANG = '1033'
$projectRoot = Split-Path $PSScriptRoot -Parent
if (-not $QtRoot) { $QtRoot = Join-Path $projectRoot '.sdk\6.11.0\msvc2022_64' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$console = Get-ChildItem (Join-Path $vsRoot 'Common7\IDE\Extensions\Microsoft\CodeCoverage.Console') -Filter 'Microsoft.CodeCoverage.Console.exe' -Recurse | Select-Object -First 1 -ExpandProperty FullName
if (-not $console) { throw 'Microsoft.CodeCoverage.Console not found in this Visual Studio.' }
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
New-Item -ItemType Directory -Force $outDir | Out-Null
$report = Join-Path $outDir 'coverage.cobertura.xml'
$env:PATH = "$QtRoot\bin;$env:PATH"

if (-not $SkipBuild) {
    # A cache made with another Qt SDK keeps finding that SDK; configure from scratch then.
    $cache = Join-Path $buildDir 'CMakeCache.txt'
    if ((Test-Path -LiteralPath $cache) -and -not (Select-String -LiteralPath $cache -SimpleMatch "Qt6_DIR:PATH=$($QtRoot -replace '\\', '/')/" -Quiet)) {
        Remove-Item -LiteralPath $cache
    }
    & $cmake -S $projectRoot -B $buildDir -G $generator -A x64 "-DCMAKE_GENERATOR_INSTANCE=$vsRoot" "-DCMAKE_PREFIX_PATH=$QtRoot" '-DBUILD_TESTING=ON' `
        '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=/Zi /Od /Ob0 /DNDEBUG' '-DCMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO=/DEBUG /INCREMENTAL:NO'
    if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
    & $cmake --build $buildDir --config $config --parallel
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
}

$ctestArgs = @('--test-dir', $buildDir, '-C', $config, '--output-on-failure')
if ($TestFilter) { $ctestArgs += @('-R', $TestFilter) }
if (Test-Path $report) { Remove-Item $report }
& $console collect $ctest @ctestArgs -o $report -f cobertura
if (-not (Test-Path $report)) { throw 'No coverage report was written.' }

# Line coverage per source file under src: a line counts once, covered if any process hit it.
[xml]$xml = Get-Content $report -Raw
$srcRoot = (Join-Path $projectRoot 'src').ToLower()
$files = @{}
foreach ($class in $xml.SelectNodes('//class')) {
    $full = $class.filename.ToLower()
    if (-not $full.StartsWith($srcRoot)) { continue }
    if (-not $files.ContainsKey($full)) { $files[$full] = @{} }
    foreach ($line in $class.SelectNodes('lines/line')) {
        $number = [int]$line.number
        $hit = [int]$line.hits -gt 0
        if (-not $files[$full].ContainsKey($number) -or $hit) { $files[$full][$number] = $hit }
    }
}
$rows = foreach ($entry in $files.GetEnumerator()) {
    $total = $entry.Value.Count
    $covered = @($entry.Value.Values | Where-Object { $_ }).Count
    [pscustomobject]@{
        File = $entry.Key.Substring($srcRoot.Length + 1)
        Lines = $total
        Covered = $covered
        Missed = $total - $covered
        Percent = if ($total) { [math]::Round(100.0 * $covered / $total, 1) } else { 0 }
    }
}
$totalLines = ($rows | Measure-Object Lines -Sum).Sum
$totalCovered = ($rows | Measure-Object Covered -Sum).Sum
$summary = Join-Path $outDir 'summary.txt'
$rows | Sort-Object Missed -Descending | Format-Table -AutoSize | Out-String -Width 200 | Set-Content $summary -Encoding UTF8
"TOTAL $totalCovered / $totalLines lines = $([math]::Round(100.0 * $totalCovered / [math]::Max(1, $totalLines), 1))%" | Add-Content $summary -Encoding UTF8
Get-Content $summary -Encoding UTF8 | Select-Object -Last 1
