[CmdletBinding()]
param(
    [switch]$Install,
    [switch]$SkipBuild,
    [string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " Gaslightr Production Packaging & Distribution Automation" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

# 1. Locate CMake binary
$cmake = 'cmake'
$knownCMakePaths = @(
    "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\CMake\bin\cmake.exe"
)
if (-not (Get-Command $cmake -ErrorAction SilentlyContinue)) {
    foreach ($candidate in $knownCMakePaths) {
        if (Test-Path $candidate) {
            $cmake = $candidate
            break
        }
    }
}

# 2. Build gaslightr_uxpaddon in requested configuration
if (-not $SkipBuild) {
    Write-Host "[1/4] Building gaslightr_uxpaddon ($Config x64)..." -ForegroundColor Yellow
    & $cmake --build build --config $Config --target gaslightr_uxpaddon
    if ($LASTEXITCODE -ne 0) {
        throw "CMake build failed with exit code $LASTEXITCODE"
    }
} else {
    Write-Host "[1/4] Skipping build as requested (-SkipBuild)" -ForegroundColor DarkGray
}

# 3. Locate compiled addon binaries
$distRoot = Join-Path $repo 'dist'
$distDir = Join-Path $distRoot 'Gaslightr'

$addonSource = Join-Path $repo "build\native\$Config\gaslightr.uxpaddon"
$pdbSource = Join-Path $repo "build\native\$Config\gaslightr.pdb"

if (-not (Test-Path $addonSource)) {
    $addonSource = Join-Path $repo 'uxp-frontend\win\x64\gaslightr.uxpaddon'
    $pdbSource = Join-Path $repo 'uxp-frontend\win\x64\gaslightr.pdb'
}
if (-not (Test-Path $addonSource)) {
    throw "Addon binary not found at $addonSource. Run build first."
}

# 4. Clean and prepare dist staging directory
Write-Host "[2/4] Staging production assets in $distDir..." -ForegroundColor Yellow
if (Test-Path $distDir) {
    Remove-Item -LiteralPath $distDir -Recurse -Force
}
New-Item -ItemType Directory -Path (Join-Path $distDir 'win\x64') -Force | Out-Null

$productionFiles = @(
    'manifest.json',
    'index.html',
    'index.jsx',
    'styles.css',
    'bridge.js',
    'pixels.js',
    'native-checks.js',
    'preview-image.js'
)

foreach ($file in $productionFiles) {
    $srcPath = Join-Path $repo "uxp-frontend\$file"
    if (-not (Test-Path $srcPath)) {
        throw "Required production file missing: $srcPath"
    }
    Copy-Item -LiteralPath $srcPath -Destination $distDir -Force
    Write-Host "  -> Staged $file" -ForegroundColor DarkGray
}

# Copy native addon and pdb to root and win/x64/
Copy-Item -LiteralPath $addonSource -Destination $distDir -Force
if (Test-Path $pdbSource) { Copy-Item -LiteralPath $pdbSource -Destination $distDir -Force }

$win64Dir = Join-Path $distDir 'win\x64'
Copy-Item -LiteralPath $addonSource -Destination $win64Dir -Force
if (Test-Path $pdbSource) { Copy-Item -LiteralPath $pdbSource -Destination $win64Dir -Force }
Write-Host "  -> Staged native binaries (gaslightr.uxpaddon, gaslightr.pdb)" -ForegroundColor DarkGray

# 5. Package into compliant .ccx archive
Write-Host "[3/4] Creating Adobe UXP .ccx package..." -ForegroundColor Yellow
$ccxPath = Join-Path $distRoot 'Gaslightr.ccx'
$zipTemp = Join-Path $distRoot 'Gaslightr.zip'

if (Test-Path $ccxPath) { Remove-Item -LiteralPath $ccxPath -Force }
if (Test-Path $zipTemp) { Remove-Item -LiteralPath $zipTemp -Force }

Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($distDir, $zipTemp, [System.IO.Compression.CompressionLevel]::Optimal, $false)
Move-Item -LiteralPath $zipTemp -Destination $ccxPath -Force

$ccxItem = Get-Item $ccxPath
$sizeKb = [math]::Round($ccxItem.Length / 1KB, 2)
Write-Host "  [OK] Package created: $ccxPath ($sizeKb KB)" -ForegroundColor Green

# 6. Install junction if requested
if ($Install) {
    Write-Host "[4/4] Establishing Photoshop UXP directory junction..." -ForegroundColor Yellow
    $appData = [Environment]::GetFolderPath('ApplicationData')
    $externalPluginsDir = Join-Path $appData 'Adobe\UXP\Plugins\External'
    if (-not (Test-Path $externalPluginsDir)) {
        New-Item -ItemType Directory -Path $externalPluginsDir -Force | Out-Null
    }
    $targetJunction = Join-Path $externalPluginsDir 'Gaslightr'

    if (Test-Path -LiteralPath $targetJunction) {
        $item = Get-Item -LiteralPath $targetJunction -Force
        if ($item.LinkType -eq 'Junction' -or ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
            cmd /c rmdir "$targetJunction"
        } else {
            Remove-Item -LiteralPath $targetJunction -Recurse -Force
        }
    }

    cmd /c mklink /J "$targetJunction" "$distDir"
    if (Test-Path -LiteralPath $targetJunction) {
        Write-Host "  [OK] Junction established: $targetJunction -> $distDir" -ForegroundColor Green
    } else {
        throw "Directory junction creation failed."
    }
} else {
    Write-Host "[4/4] Installation skipped (use -Install to establish directory junction)" -ForegroundColor DarkGray
}

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " Production Packaging Complete: $ccxPath" -ForegroundColor Green
Write-Host "============================================================" -ForegroundColor Cyan
