# Fetches the ONNX Runtime that ppocr_rust's model catalog lists for an architecture and unpacks it:
# onnxruntime.dll plus the licence files of the wheel it comes from (the full package ships both).
# Addresses, sizes and hashes come from rust_libs/ppocr_rust/catalog.json, the same entries the
# application's own download checks. Prints the path of the DLL.
param(
    [Parameter(Mandatory)][ValidateSet('x64', 'arm64')][string]$Arch,
    [Parameter(Mandatory)][string]$OutDir,
    # Tests use a catalog whose addresses are file:// URLs.
    [string]$Catalog = ''
)
$ErrorActionPreference = 'Stop'
# $PSScriptRoot is empty in a parameter default under Windows PowerShell 5.1.
if (-not $Catalog) { $Catalog = Join-Path (Split-Path $PSScriptRoot -Parent) '..\rust_libs\ppocr_rust\catalog.json' }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

function Get-Sha256([string]$Path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant() }

$runtime = (Get-Content -LiteralPath $Catalog -Raw -Encoding UTF8 | ConvertFrom-Json).runtimes | Where-Object { $_.arch -eq $Arch }
if (-not $runtime) { throw "The catalog has no ONNX Runtime for $Arch." }
$file = $runtime.file
$notices = [ordered]@{ 'onnxruntime-LICENSE.txt' = 'onnxruntime/LICENSE'; 'onnxruntime-ThirdPartyNotices.txt' = 'onnxruntime/ThirdPartyNotices.txt' }
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$dll = Join-Path $OutDir $file.name
$done = (Test-Path -LiteralPath $dll) -and (Get-Sha256 $dll) -eq $file.sha256 -and -not ($notices.Keys | Where-Object { -not (Test-Path -LiteralPath (Join-Path $OutDir $_)) })
if ($done) { return $dll }

$wheel = Join-Path ([IO.Path]::GetTempPath()) ([IO.Path]::GetRandomFileName() + '.whl')
try {
    $fetched = $false
    foreach ($url in $file.urls) {
        try {
            (New-Object Net.WebClient).DownloadFile($url, $wheel)
        } catch {
            Write-Host "Could not fetch ${url}: $($_.Exception.Message)"
            continue
        }
        if ((Get-Sha256 $wheel) -eq $file.archive.sha256) { $fetched = $true; break }
        Write-Host "The wheel from $url does not match the catalog's hash."
    }
    if (-not $fetched) { throw "No address in the catalog gave the expected ONNX Runtime wheel for $Arch." }
    $zip = [IO.Compression.ZipFile]::OpenRead($wheel)
    try {
        function Save-Member([string]$Member, [string]$Target) {
            $entry = $zip.GetEntry($Member)
            if (-not $entry) { throw "The wheel has no $Member." }
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $Target, $true)
        }
        Save-Member $file.archive.member $dll
        foreach ($name in $notices.Keys) { Save-Member $notices[$name] (Join-Path $OutDir $name) }
    } finally {
        $zip.Dispose()
    }
    if ((Get-Item -LiteralPath $dll).Length -ne $file.size -or (Get-Sha256 $dll) -ne $file.sha256) { throw "$($file.name) from the wheel does not match the catalog." }
} finally {
    Remove-Item -LiteralPath $wheel -Force -ErrorAction SilentlyContinue
}
$dll
