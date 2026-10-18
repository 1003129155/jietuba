param(
    [Parameter(Mandatory)][string]$Exe,
    [int]$Seconds = 10
)
# Starts the single exe inside a throwaway test sandbox (its own settings, data and logs, no
# autostart entry) and checks that it starts, stays up and writes its runtime log.
$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path -LiteralPath $Exe).Path
$root = Join-Path ([IO.Path]::GetTempPath()) ('jietuba-test-smoke-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path "$root\LocalAppData", "$root\AppData" | Out-Null
$env:JIETUBA_TEST_SANDBOX = $root
$env:LOCALAPPDATA = "$root\LocalAppData"
$env:APPDATA = "$root\AppData"
$process = Start-Process -FilePath $Exe -WorkingDirectory $root -PassThru
Start-Sleep -Seconds $Seconds
$alive = -not $process.HasExited
if ($alive) { Stop-Process -Id $process.Id -Force }
$logDir = "$root\LocalAppData\Jietuba\Logs"
$log = Get-ChildItem -LiteralPath $logDir -Filter 'runtime_cpp_*.log' -ErrorAction SilentlyContinue | Select-Object -First 1
$crash = Get-ChildItem -LiteralPath $logDir -Filter 'crash_cpp.log' -ErrorAction SilentlyContinue
if ($log) { Get-Content -LiteralPath $log.FullName | Select-String -Pattern '\[(ERROR|WARNING)\]' | ForEach-Object { Write-Host "log: $($_.Line)" } }
if ($crash) { Get-Content -LiteralPath $crash.FullName | ForEach-Object { Write-Host "crash: $_" } }
if (-not $alive) { throw "The app exited within $Seconds s (exit code $($process.ExitCode))." }
if (-not $log) { throw 'The app wrote no runtime log.' }
if ($crash) { throw 'The app wrote a crash log.' }
# The full exe puts its embedded ONNX Runtime into models\ when none is there; the package has none.
if ((Split-Path $Exe -Leaf) -eq 'jietuba_pp.exe') {
    $runtime = Join-Path (Split-Path $Exe -Parent) 'models\onnxruntime.dll'
    if (-not (Test-Path -LiteralPath $runtime) -or (Get-Item -LiteralPath $runtime).Length -lt 10MB) { throw "The full exe did not put onnxruntime.dll into $runtime." }
}
Write-Host "Smoke test passed: $Exe ran $Seconds s and logged $((Get-Content -LiteralPath $log.FullName).Count) lines."
