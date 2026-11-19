# Hellwalker transactional session launcher:
# Keeps DOOMTheDarkAges 100% vanilla at rest and stores runtime caches/tools
# under %LOCALAPPDATA%\DOOMTheDarkAges\SurvivalShield\.
# On launch:
#   1. Recovers any previous unclean session from session-backup\.
#   2. Auto-builds or validates u5_patch_manifest.json + DOOMTheDarkAges.exe.u5.mod.
#   3. Runs menu & intro-skip generators against SurvivalShieldMod\mods\.
#   4. Snapshots ONLY 3 vanilla files (exe, meta.resources, packagemapspec.json).
#   5. Mounts temporary directory junction <GAME>\mods -> SurvivalShieldMod\mods,
#      swaps in .u5.mod + cached (or freshly built) Atlan archive.
#   6. Launches DOOM + SurvivalShieldLoader (skipped under -BuildCache).
#   7. Automatically rolls back <GAME> to verified vanilla on exit/error.
param(
    [switch]$BuildCache,
    [switch]$ForceRebuild,
    [switch]$Restore,
    [string]$GameDir = '',
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ExtraArgs
)

$ErrorActionPreference = 'Stop'

if ($ExtraArgs) {
    foreach ($arg in $ExtraArgs) {
        switch ($arg.ToLowerInvariant()) {
            '--build-cache'   { $BuildCache = $true }
            '--force-rebuild' { $ForceRebuild = $true }
            '--restore'       { $Restore = $true }
            default { Write-Host "[hellwalker] Argumento ignorado: $arg" }
        }
    }
}

function Resolve-GameDir([string]$ExplicitDir) {
    $candidates = New-Object System.Collections.Generic.List[string]
    if ($ExplicitDir) { [void]$candidates.Add($ExplicitDir) }
    if ($env:DOOM_DARK_AGES_DIR) { [void]$candidates.Add($env:DOOM_DARK_AGES_DIR) }

    $steamRoots = New-Object System.Collections.Generic.List[string]
    foreach ($regPath in @(
        'HKCU:\Software\Valve\Steam',
        'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam',
        'HKLM:\SOFTWARE\Valve\Steam'
    )) {
        try {
            if (Test-Path -LiteralPath $regPath) {
                $props = Get-ItemProperty -LiteralPath $regPath -ErrorAction SilentlyContinue
                foreach ($propName in @('SteamPath', 'InstallPath')) {
                    $val = $props.$propName
                    if ($val -and (Test-Path -LiteralPath $val)) {
                        $norm = [System.IO.Path]::GetFullPath(($val -replace '/', '\'))
                        if (-not $steamRoots.Contains($norm)) { [void]$steamRoots.Add($norm) }
                    }
                }
            }
        } catch {}
    }

    $allRoots = New-Object System.Collections.Generic.List[string]
    foreach ($r in $steamRoots) {
        if (-not $allRoots.Contains($r)) { [void]$allRoots.Add($r) }
        $vdf = Join-Path $r 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf) {
            $vdfText = Get-Content -LiteralPath $vdf -Raw -ErrorAction SilentlyContinue
            if ($vdfText) {
                foreach ($m in [regex]::Matches($vdfText, '"path"\s+"([^"]+)"')) {
                    $libPath = $m.Groups[1].Value -replace '\\\\', '\'
                    if ($libPath -and -not $allRoots.Contains($libPath)) {
                        [void]$allRoots.Add($libPath)
                    }
                }
            }
        }
    }

    foreach ($root in $allRoots) {
        [void]$candidates.Add((Join-Path $root 'steamapps\common\DOOMTheDarkAges'))
    }
    foreach ($drive in @('C', 'D', 'E', 'F', 'G', 'Z')) {
        [void]$candidates.Add("${drive}:\SteamLibrary\steamapps\common\DOOMTheDarkAges")
        [void]$candidates.Add("${drive}:\Program Files (x86)\Steam\steamapps\common\DOOMTheDarkAges")
        [void]$candidates.Add("${drive}:\Program Files\Steam\steamapps\common\DOOMTheDarkAges")
    }

    foreach ($cand in $candidates) {
        if ($cand -and (Test-Path -LiteralPath (Join-Path $cand 'DOOMTheDarkAges.exe'))) {
            return $cand
        }
    }
    throw "No se encontro DOOMTheDarkAges.exe. Pasa -GameDir '<ruta>' o defini la variable de entorno DOOM_DARK_AGES_DIR."
}

$BinDir           = $PSScriptRoot
$ModRoot          = Split-Path -Parent $BinDir
$ModsDir          = Join-Path $ModRoot 'mods'
$AppDataRoot      = Join-Path $env:LOCALAPPDATA 'DOOMTheDarkAges\SurvivalShield'
$ExeCacheDir      = Join-Path $AppDataRoot 'exe-cache'
$AtlanCacheDir    = Join-Path $AppDataRoot 'atlan-cache'
$AtlanStagingDir  = Join-Path $AppDataRoot 'atlan-staging'
$SessionBakDir    = Join-Path $AppDataRoot 'session-backup'
$ToolsDir         = Join-Path $AppDataRoot 'tools'
$SessionStateFile = Join-Path $SessionBakDir 'session_active.json'
$ManifestPath     = Join-Path $ExeCacheDir 'u5_patch_manifest.json'
$ModExePath       = Join-Path $ExeCacheDir 'DOOMTheDarkAges.exe.u5.mod'

New-Item -ItemType Directory -Path $AppDataRoot -Force | Out-Null
New-Item -ItemType Directory -Path $ToolsDir -Force | Out-Null
$GameDir          = Resolve-GameDir $GameDir
$OoSource         = Join-Path $ToolsDir 'oo2core_9_win64.dll'

function Ensure-Shortcuts {
    try {
        $targetBat = Join-Path $BinDir 'PlaySurvivalShield.bat'
        $iconPath  = Join-Path $ModRoot 'assets\hellwalker_icon.ico'
        if (-not (Test-Path -LiteralPath $targetBat)) { return }
        $wsh = New-Object -ComObject WScript.Shell
        foreach ($lnkPath in @(
            (Join-Path $ModRoot 'PlaySurvivalShield.lnk'),
            (Join-Path $BinDir 'PlaySurvivalShield.lnk')
        )) {
            $sc = $wsh.CreateShortcut($lnkPath)
            $sc.TargetPath       = $targetBat
            $sc.WorkingDirectory = $BinDir
            $sc.Description      = 'Iniciar DOOM: The Dark Ages con Hellwalker y Biblioteca de Mods'
            if (Test-Path -LiteralPath $iconPath) {
                $sc.IconLocation = "$iconPath,0"
            }
            $sc.Save()
        }
    } catch {}
}

Ensure-Shortcuts

function Get-FileSha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
}

function Remove-ModsJunction([string]$GameModsPath) {
    if (-not (Test-Path -LiteralPath $GameModsPath)) {
        return
    }
    $item = Get-Item -LiteralPath $GameModsPath -Force
    $isReparse = (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
    if (-not $isReparse) {
        throw "SEGURIDAD: '$GameModsPath' existe como carpeta real (no junction). No se elimina automaticamente."
    }
    cmd /c rmdir "$GameModsPath"
    if (Test-Path -LiteralPath $GameModsPath) {
        throw "No se pudo quitar el junction '$GameModsPath'."
    }
}

function Clear-AtlanStaging {
    if (-not (Test-Path -LiteralPath $AtlanStagingDir)) {
        return
    }
    $children = @(Get-ChildItem -LiteralPath $AtlanStagingDir -Force -ErrorAction SilentlyContinue)
    foreach ($child in $children) {
        Remove-ModsJunction $child.FullName
    }
    $remaining = @(Get-ChildItem -LiteralPath $AtlanStagingDir -Force -ErrorAction SilentlyContinue)
    if ($remaining.Count -eq 0) {
        Remove-Item -LiteralPath $AtlanStagingDir -Force -ErrorAction SilentlyContinue
    }
}

function Mount-GameModsJunction([string]$GameModsPath, [string]$TargetDir) {
    if (Test-Path -LiteralPath $GameModsPath) {
        Remove-ModsJunction $GameModsPath
    }
    cmd /c mklink /J "$GameModsPath" "$TargetDir" | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $GameModsPath)) {
        throw "No se pudo crear el directory junction '$GameModsPath' -> '$TargetDir'."
    }
}

function Get-ActiveModDirectories {
    $result = @()
    $dirs = Get-ChildItem -LiteralPath $ModsDir -Directory | Sort-Object Name
    foreach ($dir in $dirs) {
        if ($dir.Name -match '\.(off|disabled)$') {
            continue
        }
        $manifestFile = Join-Path $dir.FullName 'darkagesmod.txt'
        if (-not (Test-Path -LiteralPath $manifestFile)) {
            continue
        }
        $result += $dir
    }
    return $result
}

function Wait-GameExit {
    while (Get-Process -Name 'DOOMTheDarkAges' -ErrorAction SilentlyContinue) {
        Start-Sleep -Seconds 3
    }
}

function Remove-AtlanLeftovers {
    $leftovers = @(
        (Join-Path $GameDir 'oo2core_9_win64.dll'),
        (Join-Path $GameDir 'AtlanPatcher.txt'),
        (Join-Path $GameDir 'DOOMTheDarkAges.exe.backup'),
        (Join-Path $GameDir 'base\meta.resources.backup'),
        (Join-Path $GameDir 'base\packagemapspec.json.backup')
    )
    foreach ($f in $leftovers) {
        if (Test-Path -LiteralPath $f) {
            Remove-Item -LiteralPath $f -Force -ErrorAction SilentlyContinue
        }
    }
}

function Restore-Session {
    Wait-GameExit

    $gameExe  = Join-Path $GameDir 'DOOMTheDarkAges.exe'
    $gameMeta = Join-Path $GameDir 'base\meta.resources'
    $gamePkg  = Join-Path $GameDir 'base\packagemapspec.json'
    $gameMods = Join-Path $GameDir 'mods'
    $modArc   = Join-Path $GameDir 'base\modarchives'
    $modRes   = Join-Path $modArc 'common_mod.resources'

    if (Test-Path -LiteralPath $SessionStateFile) {
        $state = Get-Content -LiteralPath $SessionStateFile -Raw | ConvertFrom-Json
        $bakExe  = Join-Path $SessionBakDir 'DOOMTheDarkAges.exe'
        $bakMeta = Join-Path $SessionBakDir 'meta.resources'
        $bakPkg  = Join-Path $SessionBakDir 'packagemapspec.json'

        if (Test-Path -LiteralPath $bakExe)  { Copy-Item -LiteralPath $bakExe  -Destination $gameExe  -Force }
        if (Test-Path -LiteralPath $bakMeta) { Copy-Item -LiteralPath $bakMeta -Destination $gameMeta -Force }
        if (Test-Path -LiteralPath $bakPkg)  { Copy-Item -LiteralPath $bakPkg  -Destination $gamePkg  -Force }

        $okExe  = (Get-FileSha256 $gameExe)  -eq $state.exeSha256
        $okMeta = (Get-FileSha256 $gameMeta) -eq $state.metaSha256
        $okPkg  = (Get-FileSha256 $gamePkg)  -eq $state.pkgSha256
        if (-not ($okExe -and $okMeta -and $okPkg)) {
            throw "ERROR DE ROLLBACK: mismatch al verificar SHA-256 restaurado (exe=$okExe meta=$okMeta pkg=$okPkg). Conservado session-backup\."
        }
    }

    if (Test-Path -LiteralPath $modRes) {
        Remove-Item -LiteralPath $modRes -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $modArc) {
        $remaining = @(Get-ChildItem -LiteralPath $modArc -Force -ErrorAction SilentlyContinue)
        if ($remaining.Count -eq 0) {
            Remove-Item -LiteralPath $modArc -Force -ErrorAction SilentlyContinue
        }
    }

    Remove-AtlanLeftovers
    if (Test-Path -LiteralPath $gameMods) {
        Remove-ModsJunction $gameMods
    }
    Clear-AtlanStaging

    if (Test-Path -LiteralPath $SessionStateFile) {
        Remove-Item -LiteralPath $SessionStateFile -Force -ErrorAction SilentlyContinue
    }
    foreach ($tmp in @('DOOMTheDarkAges.exe', 'meta.resources', 'packagemapspec.json')) {
        $p = Join-Path $SessionBakDir $tmp
        if (Test-Path -LiteralPath $p) {
            Remove-Item -LiteralPath $p -Force -ErrorAction SilentlyContinue
        }
    }
    Write-Host '[hellwalker] Carpeta del juego restaurada a 100% vanilla.'
}

function Compute-ModsCacheKey([string]$VanillaSha, [string]$MetaSha, [string]$PkgSha) {
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine("staging=active_only_v1")
    [void]$sb.AppendLine("vanilla=$VanillaSha")
    [void]$sb.AppendLine("meta=$MetaSha")
    [void]$sb.AppendLine("pkg=$PkgSha")
    foreach ($modDir in (Get-ActiveModDirectories)) {
        $modPrefixLen = $modDir.FullName.TrimEnd('\').Length + 1
        [void]$sb.AppendLine("MOD=$($modDir.Name)")
        $files = Get-ChildItem -LiteralPath $modDir.FullName -Recurse -File | Sort-Object FullName
        foreach ($f in $files) {
            $rel = "$($modDir.Name)\" + $f.FullName.Substring($modPrefixLen)
            $h = Get-FileSha256 $f.FullName
            [void]$sb.AppendLine("$rel|$($f.Length)|$h")
        }
    }
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($sb.ToString())
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $hashBytes = $sha.ComputeHash($bytes)
        return ([System.BitConverter]::ToString($hashBytes) -replace '-', '').ToUpperInvariant()
    } finally {
        $sha.Dispose()
    }
}

# 1) Crash recovery if a prior session left session_active.json behind
if (Test-Path -LiteralPath $SessionStateFile) {
    Write-Host '[hellwalker] Detectada sesion previa sin cerrar; restaurando juego a vanilla...'
    Restore-Session
}
if ($Restore) {
    Write-Host '[hellwalker] -Restore completado.'
    exit 0
}

# 2) Preflight checks
if (Get-Process -Name 'DOOMTheDarkAges' -ErrorAction SilentlyContinue) {
    throw 'DOOMTheDarkAges.exe ya esta corriendo. Cerralo antes de iniciar una sesion.'
}
if (-not (Test-Path -LiteralPath $ModsDir)) {
    throw "No se encontro la carpeta de mods '$ModsDir'."
}

$gameExe  = Join-Path $GameDir 'DOOMTheDarkAges.exe'
$gameMeta = Join-Path $GameDir 'base\meta.resources'
$gamePkg  = Join-Path $GameDir 'base\packagemapspec.json'
$gameMods = Join-Path $GameDir 'mods'

if (-not (Test-Path -LiteralPath $ManifestPath) -or -not (Test-Path -LiteralPath $ModExePath)) {
    $patchScript = Join-Path $ModRoot 'native\tools\patch_u5.py'
    if (-not (Test-Path -LiteralPath $patchScript)) {
        throw "Falta el exe parcheado en '$ExeCacheDir' y no se encontro '$patchScript'."
    }
    Write-Host "[hellwalker] Generando cache de ejecutable parcheado (Update 5) en '$ExeCacheDir'..."
    & python $patchScript --vanilla $gameExe --out-dir $ExeCacheDir
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $ManifestPath) -or -not (Test-Path -LiteralPath $ModExePath)) {
        throw "patch_u5.py fallo al generar '$ModExePath' (codigo $LASTEXITCODE)."
    }
}

$LoaderExe = Join-Path $BinDir 'SurvivalShieldLoader.exe'
$ModDll    = Join-Path $BinDir 'SurvivalShield.dll'
if (-not $BuildCache) {
    if (-not (Test-Path -LiteralPath $LoaderExe)) {
        throw ("Falta '$LoaderExe'. Windows Defender suele ponerlo en cuarentena por falso positivo sobre el inyector DLL. " +
               "Restauralo o agrega una exclusion para '$ModRoot' en Seguridad de Windows -> Proteccion contra virus y amenazas -> Historial de proteccion / Exclusiones y volve a abrir.")
    }
    if (-not (Test-Path -LiteralPath $ModDll)) {
        throw "Falta '$ModDll' en '$BinDir'."
    }
}

$manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
$expectedVanillaSha = $manifest.vanillaSha256.ToUpperInvariant()
$expectedModSha     = $manifest.modSha256.ToUpperInvariant()

$actualVanillaSha = Get-FileSha256 $gameExe
if ($actualVanillaSha -ne $expectedVanillaSha) {
    throw "ABORTADO (preflight): DOOMTheDarkAges.exe en disco ($actualVanillaSha) no coincide con el vanilla esperado ($expectedVanillaSha). Si Steam actualizo el juego, hay que portar patch_u5.py y BuildGuard."
}
$actualModSha = Get-FileSha256 $ModExePath
if ($actualModSha -ne $expectedModSha) {
    throw "ABORTADO (preflight): $ModExePath ($actualModSha) no coincide con u5_patch_manifest.json ($expectedModSha)."
}

if (Test-Path -LiteralPath $gameMods) {
    Remove-ModsJunction $gameMods
}

# 3) Run generators directly on SurvivalShieldMod\mods\
powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $BinDir 'generate_mods_menu.ps1') -GameDir $GameDir -ModsDir $ModsDir
if ($LASTEXITCODE -ne 0) { throw "generate_mods_menu.ps1 fallo con codigo $LASTEXITCODE" }
powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $BinDir 'apply_intro_skip.ps1') -ModsDir $ModsDir
if ($LASTEXITCODE -ne 0) { throw "apply_intro_skip.ps1 fallo con codigo $LASTEXITCODE" }

# 3b) Pre-launch validation of persisted user preferences vs disk state
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$stateJsonPath = Join-Path $env:LOCALAPPDATA 'DOOMTheDarkAges\SurvivalShield\mods_menu_state.json'
if (Test-Path -LiteralPath $stateJsonPath) {
    $rawState = [System.IO.File]::ReadAllText($stateJsonPath, $utf8NoBom)
    if (-not [string]::IsNullOrWhiteSpace($rawState)) {
        $savedPrefs = $rawState | ConvertFrom-Json
        foreach ($prop in $savedPrefs.PSObject.Properties) {
            $base = $prop.Name -replace '\.(off|disabled)$', ''
            $want = [bool]$prop.Value
            $onPath  = Join-Path $ModsDir $base
            $offPath = "$onPath.off"
            $disPath = "$onPath.disabled"
            $onExists  = Test-Path -LiteralPath $onPath
            $offExists = (Test-Path -LiteralPath $offPath) -or (Test-Path -LiteralPath $disPath)
            if (-not $onExists -and -not $offExists) { continue }
            if ($want -and (-not $onExists -or $offExists)) {
                throw "Validacion pre-arranque fallo para '$base': preferencia=SI pero carpeta en disco no coincide."
            }
            if ((-not $want) -and ($onExists -or -not $offExists)) {
                throw "Validacion pre-arranque fallo para '$base': preferencia=NO pero carpeta en disco no coincide."
            }
        }
    }
}

$activeMods = @()
$disabledMods = @()
Get-ChildItem -LiteralPath $ModsDir -Directory | Sort-Object Name | ForEach-Object {
    if (-not (Test-Path -LiteralPath (Join-Path $_.FullName 'darkagesmod.txt'))) { return }
    $base = $_.Name -replace '\.(off|disabled)$', ''
    if ($base -eq 'Hellwalker') { return }
    if ($_.Name -match '\.(off|disabled)$') {
        $disabledMods += $base
    } else {
        $activeMods += $base
    }
}
$skipIntroEnabled = $false
foreach ($aMod in $activeMods) {
    $aDir = Join-Path $ModsDir $aMod
    if (($aMod -match '^(?i)(hellwalker)?(intro_?skip|skip_?intro)$') -or
        (Test-Path -LiteralPath (Join-Path $aDir 'decls\uiwidget\ui\screens\menu_bethesda_logo.decl'))) {
        $skipIntroEnabled = $true
    }
}
Write-Host ("[hellwalker] Preferencias validadas: IntroSkip=" + $(if ($skipIntroEnabled) {'SI'} else {'NO'}) +
            " | Activos=[" + ($activeMods -join ', ') + "]" +
            " | Desactivados=[" + ($disabledMods -join ', ') + "]")

# 4) Compute Atlan cache key (after generators updated Hellwalker decls)
$metaSha = Get-FileSha256 $gameMeta
$pkgSha  = Get-FileSha256 $gamePkg
$cacheKey = Compute-ModsCacheKey $actualVanillaSha $metaSha $pkgSha

$cacheKeyFile = Join-Path $AtlanCacheDir 'cache_key.txt'
$cacheModRes  = Join-Path $AtlanCacheDir 'common_mod.resources'
$cacheMeta    = Join-Path $AtlanCacheDir 'meta.resources'
$cachePkg     = Join-Path $AtlanCacheDir 'packagemapspec.json'

$cacheValid = (-not $ForceRebuild) -and
              (Test-Path -LiteralPath $cacheKeyFile) -and
              (Test-Path -LiteralPath $cacheModRes) -and
              (Test-Path -LiteralPath $cacheMeta) -and
              (Test-Path -LiteralPath $cachePkg) -and
              (([System.IO.File]::ReadAllText($cacheKeyFile)).Trim() -eq $cacheKey)

# 5) Create transactional snapshot of the 3 vanilla files
New-Item -ItemType Directory -Path $SessionBakDir -Force | Out-Null
Copy-Item -LiteralPath $gameExe  -Destination (Join-Path $SessionBakDir 'DOOMTheDarkAges.exe') -Force
Copy-Item -LiteralPath $gameMeta -Destination (Join-Path $SessionBakDir 'meta.resources') -Force
Copy-Item -LiteralPath $gamePkg  -Destination (Join-Path $SessionBakDir 'packagemapspec.json') -Force

$stateObj = @{
    startedAt  = (Get-Date).ToString('o')
    exeSha256  = $actualVanillaSha
    metaSha256 = $metaSha
    pkgSha256  = $pkgSha
}
[System.IO.File]::WriteAllText($SessionStateFile, ($stateObj | ConvertTo-Json), [System.Text.UTF8Encoding]::new($false))

try {
    $modArchivesDir = Join-Path $GameDir 'base\modarchives'
    $destModRes     = Join-Path $modArchivesDir 'common_mod.resources'
    $cachePatcher   = Join-Path $AtlanCacheDir 'AtlanPatcher.txt'
    $gamePatcher    = Join-Path $GameDir 'AtlanPatcher.txt'

    if ($cacheValid) {
        Write-Host "[hellwalker] Atlan cache HIT ($($cacheKey.Substring(0,12))...); omitiendo recompilacion."
        New-Item -ItemType Directory -Path $modArchivesDir -Force | Out-Null
        Copy-Item -LiteralPath $cacheModRes -Destination $destModRes -Force
        Copy-Item -LiteralPath $cacheMeta   -Destination $gameMeta   -Force
        Copy-Item -LiteralPath $cachePkg    -Destination $gamePkg    -Force
    } else {
        # Stage ONLY active mods (without .off/.disabled) so AtlanModLoader.exe
        # never packs disabled mods into common_mod.resources.
        Clear-AtlanStaging
        New-Item -ItemType Directory -Path $AtlanStagingDir -Force | Out-Null
        $stagedNames = @()
        try {
            foreach ($activeDir in (Get-ActiveModDirectories)) {
                $stageLink = Join-Path $AtlanStagingDir $activeDir.Name
                cmd /c mklink /J "$stageLink" "$($activeDir.FullName)" | Out-Null
                if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $stageLink)) {
                    throw "No se pudo enlazar mod activo en staging '$stageLink' -> '$($activeDir.FullName)'."
                }
                $stagedNames += $activeDir.Name
            }
            Mount-GameModsJunction $gameMods $AtlanStagingDir
            Write-Host "[hellwalker] staging Atlan solo activos: [$($stagedNames -join ', ')]"

            # Run AtlanModLoader while <GAME>\DOOMTheDarkAges.exe is still vanilla
            # so its MD5 matches darkage_steam_11162026 in AtlanPatcher.txt.
            Write-Host "[hellwalker] Compilando mods con AtlanModLoader sobre vanilla..."
            New-Item -ItemType Directory -Path $AtlanCacheDir -Force | Out-Null
            $toolsPatcher = Join-Path $ToolsDir 'AtlanPatcher.txt'
            if (-not (Test-Path -LiteralPath $gamePatcher)) {
                if (Test-Path -LiteralPath $cachePatcher) {
                    Copy-Item -LiteralPath $cachePatcher -Destination $gamePatcher -Force
                } elseif (Test-Path -LiteralPath $toolsPatcher) {
                    Copy-Item -LiteralPath $toolsPatcher -Destination $gamePatcher -Force
                }
            }
            $gameOo = Join-Path $GameDir 'oo2core_9_win64.dll'
            if ((Test-Path -LiteralPath $OoSource) -and -not (Test-Path -LiteralPath $gameOo)) {
                Copy-Item -LiteralPath $OoSource -Destination $gameOo -Force
            }
            $atlanCandidates = @(
                (Join-Path $ToolsDir 'AtlanModLoader.exe'),
                (Join-Path $ModRoot 'tools\AtlanModLoader.exe'),
                (Join-Path $GameDir 'AtlanModLoader.exe')
            )
            $atlanExe = $atlanCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
            if (-not $atlanExe) {
                $destAtlan = Join-Path $ToolsDir 'AtlanModLoader.exe'
                $atlanUrl  = 'https://github.com/FlavorfulGecko5/EntityAtlan/releases/latest/download/AtlanModLoader.exe'
                Write-Host "[hellwalker] Descargando AtlanModLoader.exe desde GitHub ($atlanUrl)..."
                try {
                    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
                    Invoke-WebRequest -Uri $atlanUrl -OutFile $destAtlan -UseBasicParsing
                    if (Test-Path -LiteralPath $destAtlan) {
                        $atlanExe = $destAtlan
                    }
                } catch {
                    Remove-Item -LiteralPath $destAtlan -Force -ErrorAction SilentlyContinue
                }
            }
            if (-not $atlanExe) {
                throw ("Falta AtlanModLoader.exe y no se pudo descargar automaticamente. Copia AtlanModLoader.exe (EntityAtlan: https://github.com/FlavorfulGecko5/EntityAtlan/releases) en '$ToolsDir\AtlanModLoader.exe'.")
            }
            Push-Location $AppDataRoot
            try {
                & $atlanExe --gamedir $GameDir --nolaunch --notimer
                $atlanExit = $LASTEXITCODE
            } finally {
                Pop-Location
            }
            if (Test-Path -LiteralPath $gamePatcher) {
                Copy-Item -LiteralPath $gamePatcher -Destination $cachePatcher -Force
                Copy-Item -LiteralPath $gamePatcher -Destination $toolsPatcher -Force
            }
            if ((Test-Path -LiteralPath $gameOo) -and -not (Test-Path -LiteralPath $OoSource)) {
                Copy-Item -LiteralPath $gameOo -Destination $OoSource -Force
            }
            Remove-AtlanLeftovers
            if ($atlanExit -ne 0) {
                throw "AtlanModLoader.exe fallo con codigo $atlanExit"
            }
            if (-not (Test-Path -LiteralPath $destModRes)) {
                throw "AtlanModLoader no genero '$destModRes'."
            }
            Copy-Item -LiteralPath $destModRes -Destination $cacheModRes -Force
            Copy-Item -LiteralPath $gameMeta   -Destination $cacheMeta   -Force
            Copy-Item -LiteralPath $gamePkg    -Destination $cachePkg    -Force
            [System.IO.File]::WriteAllText($cacheKeyFile, $cacheKey, [System.Text.UTF8Encoding]::new($false))
            Write-Host "[hellwalker] Atlan cache guardada ($($cacheKey.Substring(0,12))...)."
        } finally {
            if (Test-Path -LiteralPath $gameMods) {
                Remove-ModsJunction $gameMods
            }
            Clear-AtlanStaging
        }
    }

    # Mount full library (enabled + .off) on <GAME>\mods for SurvivalShield.dll UI/ModLibrary
    Mount-GameModsJunction $gameMods $ModsDir
    Write-Host "[hellwalker] junction temporal: $gameMods -> $ModsDir"

    # Swap in our prebuilt Update 5 modded exe (Atlan patches + 8 disk thunks)
    Copy-Item -LiteralPath $ModExePath -Destination $gameExe -Force
    $swappedExeSha = Get-FileSha256 $gameExe
    if ($swappedExeSha -ne $expectedModSha) {
        throw "Swap de .u5.mod fallo en verificacion SHA-256 ($swappedExeSha != $expectedModSha)."
    }
    Write-Host "[hellwalker] exe Update 5 (.mod) activo y verificado para esta sesion."

    if ($BuildCache) {
        Write-Host '[hellwalker] Modo -BuildCache: saltando lanzamiento del juego.'
    } else {
        # Ensure Overwolf Vulkan layer env vars are set in the launched game env
        $env:DISABLE_VULKAN_OW_OVERLAY_LAYER = '1'
        $env:DISABLE_VULKAN_OW_OBS_CAPTURE   = '1'

        if (-not (Get-Process -Name 'steam' -ErrorAction SilentlyContinue)) {
            Start-Process 'steam://open/main'
            Start-Sleep -Seconds 10
        }

        $argList = @('+r_rayTracingPipelineUseDeferredOperation', '0')
        if ($skipIntroEnabled) {
            $argList += @('+com_skipIntroVideo', '1')
        } else {
            $argList += @('+com_skipIntroVideo', '0')
        }

        Write-Host "[hellwalker] Lanzando DOOMTheDarkAges.exe ($($argList -join ' '))..."
        Start-Process -FilePath $gameExe -ArgumentList $argList -WorkingDirectory $GameDir

        $deadline = (Get-Date).AddSeconds(20)
        while ((Get-Date) -lt $deadline -and -not (Get-Process -Name 'DOOMTheDarkAges' -ErrorAction SilentlyContinue)) {
            Start-Sleep -Milliseconds 500
        }
        if (-not (Get-Process -Name 'DOOMTheDarkAges' -ErrorAction SilentlyContinue)) {
            Write-Host '[hellwalker] Launch directo no arranco a tiempo; fallback steam://rungameid/3017860'
            Start-Process 'steam://rungameid/3017860'
        }

        & $LoaderExe
    }
} finally {
    Restore-Session
}
