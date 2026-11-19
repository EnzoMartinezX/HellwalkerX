# Hellwalker: refleja el estado validado de HellwalkerIntroSkip en el cvar del
# motor y en settings.json. La fuente de verdad es la preferencia persistida en
# mods_menu_state.json (y la carpeta del mod tras generate_mods_menu.ps1).
param(
    [string]$ModsDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'mods')
)

$ErrorActionPreference = 'Continue'
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$appDir = Join-Path $env:LOCALAPPDATA 'DOOMTheDarkAges\SurvivalShield'
$stateJson = Join-Path $appDir 'mods_menu_state.json'
$settingsJson = Join-Path $appDir 'settings.json'

$introModBase = $null
if (Test-Path -LiteralPath $ModsDir) {
    Get-ChildItem -LiteralPath $ModsDir -Directory | ForEach-Object {
        $base = $_.Name -replace '\.(off|disabled)$', ''
        if ($base -eq 'Hellwalker') { return }
        $hasLogos = (Test-Path -LiteralPath (Join-Path $_.FullName 'decls\uiwidget\ui\screens\menu_bethesda_logo.decl')) -or
                    (Test-Path -LiteralPath (Join-Path $_.FullName 'decls\uiwidget\ui\screens\menu_id_logo.decl'))
        if ($hasLogos -or ($base -match '^(?i)(hellwalker)?(intro_?skip|skip_?intro)$')) {
            $introModBase = $base
        }
    }
}

$introEnabled = $null
if (Test-Path -LiteralPath $stateJson) {
    try {
        $rawState = [System.IO.File]::ReadAllText($stateJson, $utf8NoBom)
        if (-not [string]::IsNullOrWhiteSpace($rawState)) {
            $desired = $rawState | ConvertFrom-Json
            if ($null -ne $desired) {
                foreach ($prop in $desired.PSObject.Properties) {
                    $base = $prop.Name -replace '\.(off|disabled)$', ''
                    if (($introModBase -and $base -eq $introModBase) -or
                        ($base -match '^(?i)(hellwalker)?(intro_?skip|skip_?intro)$')) {
                        $introEnabled = [bool]$prop.Value
                    }
                }
            }
        }
    } catch {}
}

if ($null -eq $introEnabled) {
    if ($introModBase) {
        $introEnabled = Test-Path -LiteralPath (Join-Path $ModsDir $introModBase)
    } else {
        $introEnabled = $false
    }
}

$want = if ($introEnabled) { 'on' } else { 'off' }

$cfg = "$env:USERPROFILE\Saved Games\id Software\DOOMTheDarkAges\base\DOOMTheDarkAgesConfig.local"
if (Test-Path -LiteralPath $cfg) {
    $raw = [System.IO.File]::ReadAllText($cfg, $utf8NoBom)
    $lines = @($raw -split "\r?\n")
    if ($lines.Count -gt 0 -and $lines[-1] -eq '') {
        $lines = $lines[0..($lines.Count - 2)]
    }
} else {
    $lines = @('// This file is for local settings only and will not be cloud synchronized', 'configVersion 19')
}

$lines = @($lines | Where-Object { $_ -notmatch '^\s*com_skipIntroVideo\b' })
$lines += if ($introEnabled) { 'com_skipIntroVideo "1"' } else { 'com_skipIntroVideo "0"' }
$outText = ($lines -join "`r`n") + "`r`n"
[System.IO.File]::WriteAllText($cfg, $outText, $utf8NoBom)

if (Test-Path -LiteralPath $settingsJson) {
    try {
        $sRaw = [System.IO.File]::ReadAllText($settingsJson, $utf8NoBom)
        $boolStr = if ($introEnabled) { 'true' } else { 'false' }
        if ($sRaw -match '"bHellwalkerSkipIntro"\s*:\s*(true|false)') {
            $sUpdated = [regex]::Replace($sRaw, '("bHellwalkerSkipIntro"\s*:\s*)(true|false)', "`$1$boolStr")
        } else {
            $sUpdated = [regex]::Replace($sRaw, '\r?\n\}\s*$', ",`r`n  `"bHellwalkerSkipIntro`": $boolStr`r`n}`r`n")
        }
        [System.IO.File]::WriteAllText($settingsJson, $sUpdated, $utf8NoBom)
    } catch {}
}

Write-Host ("Hellwalker intro skip: " + $want + " (validado contra mods_menu_state.json + carpeta, com_skipIntroVideo)")
