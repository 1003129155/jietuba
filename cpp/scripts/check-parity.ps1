param(
    [string]$ReferencePython = '',
    [switch]$Regenerate
)
# Lists the Python changes since the recorded baseline, optionally regenerates the expected results
# from the Python version, then builds and runs the tests.
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
if (-not $ReferencePython) { $ReferencePython = Join-Path (Split-Path $projectRoot -Parent) 'venv311\Scripts\python.exe' }
$ReferencePython = (Resolve-Path -LiteralPath $ReferencePython).Path
& $ReferencePython (Join-Path $PSScriptRoot 'python_baseline.py') verify
if ($LASTEXITCODE -ne 0) { throw 'Baseline check failed.' }
if ($Regenerate) {
    & $ReferencePython (Join-Path $PSScriptRoot 'generate_setting_accessors.py')
    if ($LASTEXITCODE -ne 0) { throw 'Accessor translation failed.' }
    & $ReferencePython (Join-Path $PSScriptRoot 'generate_parity_fixtures.py')
    if ($LASTEXITCODE -ne 0) { throw 'Python oracle generation failed.' }
    & $ReferencePython (Join-Path $PSScriptRoot 'generate_canvas_fixtures.py') --platform offscreen
    if ($LASTEXITCODE -ne 0) { throw 'Python canvas oracle generation failed.' }
    if ($env:OS -eq 'Windows_NT') {
        & $ReferencePython (Join-Path $PSScriptRoot 'generate_canvas_fixtures.py') --platform windows --output tests/fixtures/canvas-windows.json
        if ($LASTEXITCODE -ne 0) { throw 'Python Windows canvas oracle generation failed.' }
    }
}
& (Join-Path $PSScriptRoot 'build.ps1')
