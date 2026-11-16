# Packs a built single-exe package (build.ps1 -Static [-Full]) as an unsigned .msix for the Microsoft Store.
# The Store signs the package on ingestion; sideload tests need a signature the machine trusts (-Sign).
# Identity values come from Partner Center (Product identity); without them the package is a local test one.
param(
    [Parameter(Mandatory)][string]$ExeDir,
    [ValidateSet('lite', 'full')][string]$Variant = 'lite',
    [ValidateSet('x64', 'arm64')][string]$Arch = 'x64',
    [string]$Version = '',
    [string]$IdentityName = '',
    [string]$Publisher = '',
    [string]$PublisherDisplayName = '',
    [string]$DisplayName = '',
    [Parameter(Mandatory)][string]$OutDir,
    [string]$MakeAppx = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$exe = if ($Variant -eq 'full') { 'jietuba_pp.exe' } else { 'jietuba_lite.exe' }
$ExeDir = (Resolve-Path -LiteralPath $ExeDir).Path
if (-not (Test-Path -LiteralPath (Join-Path $ExeDir $exe))) { throw "$exe is not in $ExeDir" }

if (-not $Version) { $Version = (Get-Content -LiteralPath (Join-Path $root 'VERSION') -Raw).Trim() }
# The Store reserves the fourth part for itself and requires it to be 0.
$parts = @($Version.Split('.'))
if ($parts.Count -ne 3 -or ($parts | Where-Object { $_ -notmatch '^\d+$' })) { throw "VERSION must be major.minor.patch, got '$Version'" }
$msixVersion = "$Version.0"

$local = -not $IdentityName
if ($local) {
    $IdentityName = "LocalTest.Jietuba.$Variant"
    $Publisher = 'CN=LocalTest'
    $PublisherDisplayName = 'Local Test'
}
if (-not $Publisher -or -not $PublisherDisplayName) { throw '-Publisher and -PublisherDisplayName are required with -IdentityName (both are on the Partner Center identity page).' }
if (-not $DisplayName) { $DisplayName = if ($Variant -eq 'full') { 'Jietuba PP' } else { 'Jietuba Lite' } }

. (Join-Path $PSScriptRoot 'msix_common.ps1')
$MakeAppx = Find-MakeAppx $MakeAppx

$stage = Join-Path $OutDir "stage-$Variant-$Arch"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null
Copy-Item -Path (Join-Path $ExeDir '*') -Destination $stage -Recurse

# Tile and store logos drawn from the application icon.
Add-Type -AssemblyName System.Drawing
$assets = Join-Path $stage 'Assets'
New-Item -ItemType Directory -Path $assets | Out-Null
# System.Drawing.Icon cannot decode PNG frames; the 256 px frame (width byte 0) is a PNG file inside the .ico.
$ico = [System.IO.File]::ReadAllBytes((Join-Path $root 'app.ico'))
$frame = $null
for ($i = 0; $i -lt [BitConverter]::ToUInt16($ico, 4); $i++) {
    $entry = 6 + 16 * $i
    if ($ico[$entry] -eq 0) { $frame = New-Object byte[] ([BitConverter]::ToUInt32($ico, $entry + 8)); [Array]::Copy($ico, [BitConverter]::ToUInt32($ico, $entry + 12), $frame, 0, $frame.Length) }
}
if (-not $frame -or $frame[0] -ne 0x89) { throw 'app.ico has no 256 px PNG frame.' }
$source = [System.Drawing.Bitmap]::FromStream((New-Object System.IO.MemoryStream(, $frame)))
function Save-Logo([string]$name, [int]$width, [int]$height, [double]$fill) {
    $bitmap = New-Object System.Drawing.Bitmap($width, $height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $graphics.Clear([System.Drawing.Color]::Transparent)
    $side = [int]([Math]::Min($width, $height) * $fill)
    $graphics.DrawImage($source, [int](($width - $side) / 2), [int](($height - $side) / 2), $side, $side)
    $graphics.Dispose()
    $bitmap.Save((Join-Path $assets $name), [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
}
Save-Logo 'StoreLogo.png' 50 50 1.0
Save-Logo 'Square44x44Logo.png' 44 44 1.0
Save-Logo 'Square150x150Logo.png' 150 150 0.8
Save-Logo 'Wide310x150Logo.png' 310 150 0.8
$source.Dispose()

function Escape-Xml([string]$text) { [System.Security.SecurityElement]::Escape($text) }
$manifest = Get-Content -LiteralPath (Join-Path $root 'cpp\packaging\msix\AppxManifest.xml.in') -Raw -Encoding UTF8
$values = @{
    IDENTITY_NAME = $IdentityName; PUBLISHER = $Publisher; VERSION = $msixVersion; ARCH = $Arch
    DISPLAY_NAME = $DisplayName; PUBLISHER_DISPLAY_NAME = $PublisherDisplayName; EXE = $exe
    DESCRIPTION = 'Screenshot, OCR, pin, annotation, translation and clipboard tools'
}
foreach ($key in $values.Keys) { $manifest = $manifest.Replace("@$key@", (Escape-Xml $values[$key])) }
if ($manifest -match '@[A-Z_]+@') { throw "Unfilled manifest field: $($Matches[0])" }
[System.IO.File]::WriteAllText((Join-Path $stage 'AppxManifest.xml'), $manifest, (New-Object System.Text.UTF8Encoding($false)))

New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$package = Join-Path $OutDir "$IdentityName`_$msixVersion`_$Arch.msix"
& $MakeAppx pack /d $stage /p $package /o
if ($LASTEXITCODE -ne 0) { throw 'makeappx pack failed.' }
Remove-Item -LiteralPath $stage -Recurse -Force
Write-Host "Ready: $package"
