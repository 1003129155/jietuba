# Shared by package_msix.ps1 and bundle_msix.ps1: the path of makeappx.exe.
function Find-MakeAppx([string]$Explicit) {
    if ($Explicit) { return $Explicit }
    $cmd = Get-Command makeappx.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $kits = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue).KitsRoot10
    if (-not $kits) { $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\' }
    $found = Get-ChildItem -Path (Join-Path $kits 'bin\*\x64\makeappx.exe') -ErrorAction SilentlyContinue | Sort-Object FullName | Select-Object -Last 1
    if (-not $found) { throw 'makeappx.exe not found; install the Windows SDK or pass -MakeAppx.' }
    return $found.FullName
}
