# Dot-sourced by package_msix.ps1. The extensions the Explorer command is registered for and its CLSIDs
# live in the C++ headers (src/core/explorer_menu_contract.h, explorer_command/explorer_command_core.h);
# the manifest is filled from them here, so the two never have to be kept in step by hand.

$script:ExplorerCommandDllName = 'jietuba_explorer_command.dll'

function Get-ExplorerMenuExtensions([string]$CppDir) {
    $header = Get-Content -LiteralPath (Join-Path $CppDir 'src\core\explorer_menu_contract.h') -Raw -Encoding UTF8
    $extensions = @()
    foreach ($list in 'IMAGE_EXTENSIONS', 'TEXT_EXTENSIONS') {
        $block = [regex]::Match($header, $list + '\[\]\s*=\s*\{([^}]*)\}')
        if (-not $block.Success) { throw "$list not found in explorer_menu_contract.h" }
        $found = @([regex]::Matches($block.Groups[1].Value, 'L"([a-z0-9]+)"') | ForEach-Object { $_.Groups[1].Value })
        if ($found.Count -eq 0) { throw "$list is empty in explorer_menu_contract.h" }
        $extensions += $found
    }
    return @($extensions | Select-Object -Unique)
}

function Get-ExplorerCommandClsid([string]$CppDir, [ValidateSet('lite', 'full')][string]$Variant) {
    $header = Get-Content -LiteralPath (Join-Path $CppDir 'explorer_command\explorer_command_core.h') -Raw -Encoding UTF8
    $name = 'COMMAND_CLSID_' + $Variant.ToUpperInvariant()
    $match = [regex]::Match($header, $name + '\s*=\s*L"\{([0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12})\}"')
    if (-not $match.Success) { throw "$name not found in explorer_command_core.h" }
    return $match.Groups[1].Value.ToUpperInvariant()
}

# Fills the Explorer command's placeholders in the manifest template text.
function Expand-ExplorerMenuManifest([string]$Manifest, [string]$CppDir, [ValidateSet('lite', 'full')][string]$Variant) {
    $clsid = Get-ExplorerCommandClsid $CppDir $Variant
    $types = @(Get-ExplorerMenuExtensions $CppDir | ForEach-Object {
        "            <desktop5:ItemType Type=`".$_`">`n" +
        "              <desktop5:Verb Id=`"JietubaPin`" Clsid=`"$clsid`" />`n" +
        '            </desktop5:ItemType>'
    })
    return $Manifest.Replace('@EXPLORER_COMMAND_CLSID@', $clsid).Replace('@EXPLORER_COMMAND_DLL@', $script:ExplorerCommandDllName).Replace('@EXPLORER_ITEM_TYPES@', ($types -join "`n"))
}
