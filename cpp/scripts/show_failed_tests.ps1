# 打印上一次 ctest 里失败的 QtTest 当时写下的报告，再把它重跑一遍并打印重跑的报告。
# ctest 只显示进程退出码，QtTest 的失败位置和断言内容要靠 -o 写到文件；偶发失败只有第一份报告里有。
# 用法（在 cpp 目录下，Qt 的 bin 要能找到）：.\scripts\show_failed_tests.ps1
param(
    [string]$BuildDir = 'build\coverage-build',
    [string]$Config = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'
$qtBin = Join-Path $PWD '.sdk\6.11.0\msvc2022_64\bin'
$env:PATH = "$qtBin;$env:PATH"
$reportDir = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { $env:TEMP }

$tests = (ctest --test-dir $BuildDir -C $Config --show-only=json-v1 | ConvertFrom-Json).tests
$failedLog = Join-Path $BuildDir 'Testing\Temporary\LastTestsFailed.log'
foreach ($line in Get-Content $failedLog -ErrorAction SilentlyContinue) {
    $name = ($line -split ':', 2)[1]
    $test = $tests | Where-Object { $_.name -eq $name }
    if (-not $test) { continue }
    foreach ($property in $test.properties | Where-Object { $_.name -eq 'ENVIRONMENT' }) {
        foreach ($pair in $property.value) {
            $key, $value = $pair -split '=', 2
            Set-Item "env:$key" $value
        }
    }
    # 失败那次写下的报告；下面重跑会覆盖同一个文件，所以先打印。
    $firstReport = Join-Path $BuildDir "test_reports\$name.txt"
    Write-Host "===== $name (failed run)"
    Get-Content $firstReport -ErrorAction SilentlyContinue
    $report = Join-Path $reportDir "$name.txt"
    $testArgs = @($test.command | Select-Object -Skip 1) + @('-o', "$report,txt")
    & $test.command[0] @testArgs | Out-Null
    Write-Host "===== $name (rerun, exit $LASTEXITCODE)"
    Get-Content $report -ErrorAction SilentlyContinue
}
