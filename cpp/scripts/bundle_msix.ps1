# Bundles the per-architecture .msix files of one product (package_msix.ps1 output) into the
# .msixbundle that is uploaded to Partner Center.
param(
    [Parameter(Mandatory)][string]$PackageDir,
    [Parameter(Mandatory)][string]$Output,
    [string]$MakeAppx = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'msix_common.ps1')
$MakeAppx = Find-MakeAppx $MakeAppx
$PackageDir = (Resolve-Path -LiteralPath $PackageDir).Path
$packages = @(Get-ChildItem -LiteralPath $PackageDir -Filter '*.msix')
if ($packages.Count -eq 0) { throw "No .msix in $PackageDir" }
# <identity>_<version>_<arch>.msix: one identity and one version across the bundle.
$stems = @($packages | ForEach-Object { $_.BaseName -replace '_(x64|arm64)$', '' } | Sort-Object -Unique)
if ($stems.Count -ne 1) { throw "The packages are not one product and version: $($stems -join ', ')" }
$version = $stems[0].Split('_')[-1]
& $MakeAppx bundle /d $PackageDir /p $Output /bv $version /o
if ($LASTEXITCODE -ne 0) { throw 'makeappx bundle failed.' }
Write-Host "Ready: $Output"
