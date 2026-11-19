# Empaqueta una version lista para jugadores (GitHub Releases v1.0.0)
# con los binarios compilados (SurvivalShield.dll + SurvivalShieldLoader.exe),
# los scripts, assets, docs, patch_u5.py y los mods base (Hellwalker + SkipIntro),
# excluyendo native/build, archivos .pdb/.lib/.obj y mods de terceros.
param(
    [string]$Version = 'v1.0.0',
    [string]$OutZip = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) "release\Hellwalker_${Version}_Release.zip")
)

$ErrorActionPreference = 'Stop'

$BinDir   = $PSScriptRoot
$ModRoot  = Split-Path -Parent $BinDir
$RepoRoot = Split-Path -Parent $ModRoot

$requiredBins = @(
    (Join-Path $BinDir 'SurvivalShield.dll'),
    (Join-Path $BinDir 'SurvivalShieldLoader.exe'),
    (Join-Path $BinDir 'PlaySurvivalShield.bat'),
    (Join-Path $BinDir 'hellwalker_session.ps1'),
    (Join-Path $BinDir 'generate_mods_menu.ps1'),
    (Join-Path $BinDir 'apply_intro_skip.ps1')
)
foreach ($req in $requiredBins) {
    if (-not (Test-Path -LiteralPath $req)) {
        throw "Falta archivo requerido para el Release: '$req'"
    }
}

$stageRoot = Join-Path $RepoRoot ".release-stage"
if (Test-Path -LiteralPath $stageRoot) {
    Remove-Item -LiteralPath $stageRoot -Recurse -Force
}
$stageMod  = Join-Path $stageRoot 'SurvivalShieldMod'

try {
    New-Item -ItemType Directory -Path $stageMod -Force | Out-Null

    foreach ($rootFile in @('README.md', 'LICENSE', 'NOTICE')) {
        $srcRoot = Join-Path $RepoRoot $rootFile
        if (Test-Path -LiteralPath $srcRoot) {
            Copy-Item -LiteralPath $srcRoot -Destination (Join-Path $stageRoot $rootFile) -Force
        }
    }
    foreach ($topFile in @('README.md', 'PlaySurvivalShield.bat')) {
        $src = Join-Path $ModRoot $topFile
        if (Test-Path -LiteralPath $src) {
            Copy-Item -LiteralPath $src -Destination (Join-Path $stageMod $topFile) -Force
        }
    }

    # Copy assets/ and docs/
    foreach ($folder in @('assets', 'docs')) {
        $srcDir = Join-Path $ModRoot $folder
        if (Test-Path -LiteralPath $srcDir) {
            Copy-Item -LiteralPath $srcDir -Destination (Join-Path $stageMod $folder) -Recurse -Force
        }
    }

    # Copy bin/ (excluding .lnk which is auto-generated per user machine)
    $stageBin = Join-Path $stageMod 'bin'
    New-Item -ItemType Directory -Path $stageBin -Force | Out-Null
    Get-ChildItem -LiteralPath $BinDir -Force | Where-Object { $_.Extension -ne '.lnk' } | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $stageBin $_.Name) -Recurse -Force
    }

    # Copy mods/Hellwalker and mods/SkipIntro
    $stageMods = Join-Path $stageMod 'mods'
    New-Item -ItemType Directory -Path $stageMods -Force | Out-Null
    foreach ($modName in @('Hellwalker', 'SkipIntro')) {
        $srcMod = Join-Path (Join-Path $ModRoot 'mods') $modName
        if (Test-Path -LiteralPath $srcMod) {
            Copy-Item -LiteralPath $srcMod -Destination (Join-Path $stageMods $modName) -Recurse -Force
        }
    }

    # Copy native/tools/patch_u5.py (needed on first launch to build local .u5.mod cache)
    $stageTools = Join-Path $stageMod 'native\tools'
    New-Item -ItemType Directory -Path $stageTools -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $ModRoot 'native\tools\patch_u5.py') -Destination (Join-Path $stageTools 'patch_u5.py') -Force

    $outDir = Split-Path -Parent $OutZip
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
    if (Test-Path -LiteralPath $OutZip) {
        Remove-Item -LiteralPath $OutZip -Force
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory(
        $stageRoot,
        $OutZip,
        [System.IO.Compression.CompressionLevel]::Optimal,
        $false
    )
    $zipItem = Get-Item -LiteralPath $OutZip
    Write-Host ("[release] Paquete listo: {0} ({1:N2} MB)" -f $zipItem.FullName, ($zipItem.Length / 1MB))
} finally {
    if (Test-Path -LiteralPath $stageRoot) {
        Remove-Item -LiteralPath $stageRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
