[CmdletBinding()]
param([string]$SdkRoot = 'C:\SDKs\Adobe\UXP-Hybrid')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$destination = Join-Path $repo 'vendor\adobe\uxp'
$files = @(
    'src\api\UxpAddonShared.h', 'src\api\UxpAddonTypes.h',
    'src\utilities\UxpAddon.h', 'src\utilities\UxpAddon.cpp',
    'src\utilities\UxpTask.h', 'src\utilities\UxpTask.cpp',
    'src\utilities\UxpValue.h', 'src\utilities\UxpValue.cpp'
)
foreach ($relative in $files) {
    $source = Join-Path $SdkRoot $relative
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing SDK file: $source" }
}
foreach ($relative in $files) {
    $source = Join-Path $SdkRoot $relative
    $target = Join-Path $destination $relative.Substring(4)
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    Copy-Item -LiteralPath $source -Destination $target -Force
    if ((Get-FileHash -LiteralPath $source).Hash -ne (Get-FileHash -LiteralPath $target).Hash) { throw "Copy verification failed: $target" }
}
Copy-Item -LiteralPath (Join-Path $SdkRoot 'README.md') -Destination (Join-Path $destination 'SDK-README.md') -Force
Get-ChildItem -LiteralPath $SdkRoot -Filter '*.pdf' | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $destination -Force
}
Write-Output "Staged verified Adobe SDK files in $destination"
