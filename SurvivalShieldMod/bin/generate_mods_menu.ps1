# Hellwalker fase 3: genera el apartado "MODS (biblioteca)" del menu del juego.
# Corre ANTES de Atlan (lo llama PlaySurvivalShield.bat):
#   1. aplica mods_menu_state.json (toggles hechos en el menu, renombrando
#      carpetas a .off / devolviendolas) y consume las entradas aplicadas
#   2. lee modlibrary.json (o escanea mods\ si no existe) para el roster
#   3. genera decls/setting/game/modslist_<id>.decl (fila RadioButton por mod,
#      ancla reticleHighVisMode igual que el skip intro) + strings titulo/desc
#      y reescribe la seccion "hellwalker_mods_section" de la pagina de
#      accesibilidad (base respaldada en .bak la primera vez)
# La DLL reconoce el prefijo game/modslist_* en el UiBridge: los toggles se
# guardan como estado deseado y este script los aplica en el proximo arranque.
param(
    [string]$GameDir = '',
    [string]$RepoAssets = (Join-Path (Split-Path -Parent $PSScriptRoot) 'assets'),
    [string]$ModsDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'mods')
)

$ErrorActionPreference = 'Continue'
if (-not $PSBoundParameters.ContainsKey('ModsDir') -and $GameDir) {
    $legacyGameMods = Join-Path $GameDir 'mods'
    if (Test-Path -LiteralPath $legacyGameMods) {
        $modsDir = $legacyGameMods
    } else {
        $modsDir = $ModsDir
    }
} else {
    $modsDir = $ModsDir
}
$hellwalker   = Join-Path $modsDir 'Hellwalker'
$libraryJson  = Join-Path $env:LOCALAPPDATA 'DOOMTheDarkAges\SurvivalShield\modlibrary.json'
$stateJson    = Join-Path $env:LOCALAPPDATA 'DOOMTheDarkAges\SurvivalShield\mods_menu_state.json'
$staticDecls  = Join-Path $RepoAssets 'static\decls'
$maxRows      = 10

if (-not (Test-Path $hellwalker)) { Write-Host '[modmenu] mods\Hellwalker ausente; nada que generar.'; exit 0 }

# --- 0) decls estaticos: boton "Mods" en Extras + pantalla Mods ---
# (assets\static\decls es la fuente versionada; se copia completa cada vez)
if (Test-Path $staticDecls) {
    Copy-Item (Join-Path $staticDecls '*') (Join-Path $hellwalker 'decls') -Recurse -Force
} else {
    Write-Host '[modmenu] ADVERTENCIA: assets\static\decls ausente; sin boton en Extras.'
}
$legacyMainMenuRoot = Join-Path $hellwalker 'decls\uiwidget\ui\screens\main_menu\main_menu_root.decl'
if (Test-Path -LiteralPath $legacyMainMenuRoot) {
    Remove-Item -LiteralPath $legacyMainMenuRoot -Force -ErrorAction SilentlyContinue
}
$legacyMainMenuDir = Join-Path $hellwalker 'decls\uiwidget\ui\screens\main_menu'
if ((Test-Path -LiteralPath $legacyMainMenuDir) -and @(Get-ChildItem -LiteralPath $legacyMainMenuDir -Force).Count -eq 0) {
    Remove-Item -LiteralPath $legacyMainMenuDir -Force -ErrorAction SilentlyContinue
}

# --- menuId: REGLA COMPARTIDA con native/src/ModsMenu.cpp (MenuIdForFolder).
# Se recorta .off/.disabled (X y X.off son el mismo mod y comparten fila). ---
function Get-MenuId([string]$folder) {
    $s = ($folder -replace '\.(off|disabled)$', '').ToLower() -replace '[^a-z0-9_]', '_'
    $s = ($s -replace '_+', '_').Trim('_')
    if ([string]::IsNullOrWhiteSpace($s)) { $s = 'mod' }
    return "modslist_$s"
}

function Test-IsIntroSkipModDir([string]$dirPath, [string]$baseName) {
    if ($baseName -match '^(?i)(hellwalker)?(intro_?skip|skip_?intro)$') {
        return $true
    }
    $bethesdaLogo = Join-Path $dirPath 'decls\uiwidget\ui\screens\menu_bethesda_logo.decl'
    $idLogo       = Join-Path $dirPath 'decls\uiwidget\ui\screens\menu_id_logo.decl'
    return ((Test-Path -LiteralPath $bethesdaLogo) -or (Test-Path -LiteralPath $idLogo))
}

# --- 0b) Auto-normalizar carpetas de mods de terceros ---
# Si el usuario descomprimio un mod dentro de mods\<Nombre>\Subcarpeta\ o si el
# mod tiene decls/ pero le falta darkagesmod.txt, lo normalizamos para que Atlan
# y la biblioteca lo reconozcan directamente con el nombre de la carpeta.
Get-ChildItem -LiteralPath $modsDir -Directory | ForEach-Object {
    $dirPath = $_.FullName
    $base = $_.Name -replace '\.(off|disabled)$', ''
    if ($base -eq 'Hellwalker') { return }
    $manifest = Join-Path $dirPath 'darkagesmod.txt'
    if (Test-Path -LiteralPath $manifest) { return }

    # Si no hay decls/ en la raiz pero hay exactamente 1 subcarpeta con darkagesmod.txt o decls/, promoverla
    $hasRootContent = (Test-Path -LiteralPath (Join-Path $dirPath 'decls')) -or
                      (Test-Path -LiteralPath (Join-Path $dirPath 'generated')) -or
                      (Test-Path -LiteralPath (Join-Path $dirPath 'overrides'))
    if (-not $hasRootContent) {
        $subDirs = @(Get-ChildItem -LiteralPath $dirPath -Directory -Force)
        $rootFiles = @(Get-ChildItem -LiteralPath $dirPath -File -Force)
        if ($subDirs.Count -eq 1 -and $rootFiles.Count -eq 0) {
            $inner = $subDirs[0].FullName
            if ((Test-Path -LiteralPath (Join-Path $inner 'darkagesmod.txt')) -or
                (Test-Path -LiteralPath (Join-Path $inner 'decls'))) {
                Get-ChildItem -LiteralPath $inner -Force | ForEach-Object {
                    Move-Item -LiteralPath $_.FullName -Destination $dirPath -Force
                }
                Remove-Item -LiteralPath $inner -Force -ErrorAction SilentlyContinue
                Write-Host "[modmenu] estructura aplanada automaticamente en '$($_.Name)'."
            }
        }
    }

    if (-not (Test-Path -LiteralPath $manifest)) {
        $hasAnyFiles = @(Get-ChildItem -LiteralPath $dirPath -Recurse -File -Force).Count -gt 0
        if ($hasAnyFiles) {
            $autoManifest = @(
                'modinfo = {',
                "    name = `"$base`"",
                '    game = "Doom The Dark Ages"',
                '    author = "Comunidad"',
                '    description = "Paquete de modificacion externo"',
                '    version = "1.0"',
                '    loadPriority = 1',
                '    requiredVersion = 1',
                '}'
            ) -join "`r`n"
            [System.IO.File]::WriteAllText($manifest, $autoManifest + "`r`n", [System.Text.Encoding]::ASCII)
            Write-Host "[modmenu] generado darkagesmod.txt automatico para '$base'."
        }
    }
}

# --- 1) memoria persistente + aplicar estado deseado (claves sin sufijo .off) ---
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$prefs = [ordered]@{}
$savedIntroSkipPref = $null
if (Test-Path -LiteralPath $stateJson) {
    try {
        $rawState = [System.IO.File]::ReadAllText($stateJson, $utf8NoBom)
        if (-not [string]::IsNullOrWhiteSpace($rawState)) {
            $desired = $rawState | ConvertFrom-Json
            if ($null -ne $desired) {
                foreach ($prop in $desired.PSObject.Properties) {
                    $base = $prop.Name -replace '\.(off|disabled)$', ''
                    if (-not [string]::IsNullOrWhiteSpace($base)) {
                        $prefs[$base] = [bool]$prop.Value
                        if ($base -match '^(?i)(hellwalker)?(intro_?skip|skip_?intro)$') {
                            $savedIntroSkipPref = [bool]$prop.Value
                        }
                    }
                }
            }
        }
    } catch {
        Write-Host "[modmenu] ADVERTENCIA: mods_menu_state.json invalido; reconstruyendo desde disco."
    }
}

# Limpiar entradas huerfanas si el usuario renombro o elimino una carpeta en mods\
foreach ($base in @($prefs.Keys)) {
    $onDir  = Join-Path $modsDir $base
    $offDir = "$onDir.off"
    $disDir = "$onDir.disabled"
    if (-not (Test-Path -LiteralPath $onDir) -and
        -not (Test-Path -LiteralPath $offDir) -and
        -not (Test-Path -LiteralPath $disDir)) {
        $prefs.Remove($base)
    }
}

foreach ($base in @($prefs.Keys)) {
    $want   = [bool]$prefs[$base]
    $onDir  = Join-Path $modsDir $base
    $offDir = "$onDir.off"
    $disDir = "$onDir.disabled"
    $applied = $false
    if ($want) {
        if (Test-Path -LiteralPath $offDir) { Rename-Item -LiteralPath $offDir $base -ErrorAction SilentlyContinue; $applied = $true }
        if (Test-Path -LiteralPath $disDir) { Rename-Item -LiteralPath $disDir $base -ErrorAction SilentlyContinue; $applied = $true }
        if (Test-Path -LiteralPath $onDir)  { $applied = $true }
    } else {
        if (Test-Path -LiteralPath $onDir)  { Rename-Item -LiteralPath $onDir "$base.off" -ErrorAction SilentlyContinue; $applied = $true }
        if ((Test-Path -LiteralPath $offDir) -or (Test-Path -LiteralPath $disDir)) { $applied = $true }
    }
    if ($applied) {
        Write-Host "[modmenu] aplicado: $base -> $(if ($want) {'activo'} else {'.off'})"
    }
}

# Completar memoria para mods presentes en disco que aun no tuvieran preferencia guardada
Get-ChildItem -LiteralPath $modsDir -Directory | ForEach-Object {
    $manifest = Join-Path $_.FullName 'darkagesmod.txt'
    if (-not (Test-Path -LiteralPath $manifest)) { return }
    $base = $_.Name -replace '\.(off|disabled)$', ''
    if ($base -eq 'Hellwalker') { return }
    if (-not $prefs.Contains($base)) {
        if ($null -ne $savedIntroSkipPref -and (Test-IsIntroSkipModDir $_.FullName $base)) {
            $prefs[$base] = [bool]$savedIntroSkipPref
            $onDir  = Join-Path $modsDir $base
            $offDir = "$onDir.off"
            if ($savedIntroSkipPref -and (Test-Path -LiteralPath $offDir)) {
                Rename-Item -LiteralPath $offDir $base -ErrorAction SilentlyContinue
            } elseif ((-not $savedIntroSkipPref) -and (Test-Path -LiteralPath $onDir)) {
                Rename-Item -LiteralPath $onDir "$base.off" -ErrorAction SilentlyContinue
            }
        } else {
            $isEnabled = -not ($_.Name -match '\.(off|disabled)$')
            $prefs[$base] = $isEnabled
        }
    }
}

$stateDir = Split-Path -Parent $stateJson
if (-not (Test-Path -LiteralPath $stateDir)) { New-Item -ItemType Directory -Path $stateDir -Force | Out-Null }
[System.IO.File]::WriteAllText($stateJson, ($prefs | ConvertTo-Json), $utf8NoBom)

# --- 2) roster: escaneo directo de disco enriquecido con modlibrary.json ---
function Read-ManifestField([string]$path, [string]$key) {
    if (-not (Test-Path $path)) { return $null }
    foreach ($line in Get-Content $path -ErrorAction SilentlyContinue) {
        if ($line -match "^\s*$key\s*=\s*`"?(.+?)`"?\s*$") { return $Matches[1].Trim() }
    }
    return $null
}

$libCounts = @{}
if (Test-Path -LiteralPath $libraryJson) {
    try {
        $lib = (Get-Content -LiteralPath $libraryJson -Raw) | ConvertFrom-Json
        foreach ($m in $lib.mods) {
            $b = $m.folder -replace '\.(off|disabled)$', ''
            if (-not [string]::IsNullOrWhiteSpace($b)) {
                $libCounts[$b] = [int]$m.declFiles
            }
        }
    } catch {}
}

$rosterMap = @{}
Get-ChildItem -LiteralPath $modsDir -Directory | ForEach-Object {
    $manifest = Join-Path $_.FullName 'darkagesmod.txt'
    if (Test-Path -LiteralPath $manifest) {
        $base = $_.Name -replace '\.(off|disabled)$', ''
        $off = ($_.Name -match '\.(off|disabled)$')
        $declFiles = if ($libCounts.ContainsKey($base)) { $libCounts[$base] } else { 0 }
        $rosterMap[$base] = @{
            folder      = $base
            name        = (Read-ManifestField $manifest 'name')
            author      = (Read-ManifestField $manifest 'author')
            version     = (Read-ManifestField $manifest 'version')
            description = (Read-ManifestField $manifest 'description')
            isIntroSkip = (Test-IsIntroSkipModDir $_.FullName $base)
            enabled     = (-not $off)
            present     = $true
            declFiles   = $declFiles
        }
    }
}
$roster = @($rosterMap.Values | Sort-Object @{Expression={[bool]$_.isIntroSkip}; Descending=$true}, folder)

# --- 3) generar decls: un tab de settings por mod ---
# Accesibilidad vuelve a vanilla: la fila HELLWALKER (y su "saltar la intro")
# se muda a la pantalla Mods. La intro se controla con el toggle del mod
# HellwalkerIntroSkip en su propio tab.
Remove-Item (Join-Path $hellwalker 'decls\uiwidget\ui\screens\settings\settings_page_accessibility.decl') -ErrorAction SilentlyContinue
Remove-Item (Join-Path $hellwalker 'decls\setting\game\hellwalker_skip_intro.decl') -ErrorAction SilentlyContinue
Remove-Item (Join-Path $hellwalker 'decls\string\ui\settings\game\hellwalker_skip_intro_title.decl') -ErrorAction SilentlyContinue
Remove-Item (Join-Path $hellwalker 'decls\string\ui\settings\game\hellwalker_skip_intro_desc.decl') -ErrorAction SilentlyContinue
Remove-Item (Join-Path $hellwalker 'decls\setting\game\modslist_*.decl') -ErrorAction SilentlyContinue
Remove-Item (Join-Path $hellwalker 'decls\string\ui\mainmenu\screen_modslist_*.decl') -ErrorAction SilentlyContinue
Remove-Item (Join-Path $hellwalker 'decls\string\ui\mainmenu\modslist_tab_*.decl') -ErrorAction SilentlyContinue
Remove-Item (Join-Path $hellwalker 'decls\string\ui\settings\game\modslist_*') -ErrorAction SilentlyContinue
Remove-Item (Join-Path $hellwalker 'decls\uiwidget\ui\screens\settings\settings_page_mods_*.decl') -ErrorAction SilentlyContinue

function Write-StringDecl([string]$relDir, [string]$id, [string]$textEs, [string]$textEn = '') {
    $path = Join-Path $hellwalker "decls\string\$relDir\$id.decl"
    $rawEn = if ([string]::IsNullOrWhiteSpace($textEn)) { $textEs } else { $textEn }
    $esClean = $textEs -replace '%>', '% >'
    $enClean = $rawEn -replace '%>', '% >'
    $content = @"
{
	edit = {
		localizations = {
			"english" = {
				num = 1;
				item[0] = <%$enClean%>;
			}
			"spanish" = {
				num = 1;
				item[0] = <%$esClean%>;
			}
			"latin_spanish" = {
				num = 1;
				item[0] = <%$esClean%>;
			}
		}
	}
}
"@
    Set-Content -Path $path -Value $content -Encoding Ascii
}

# Pool de propiedades bool nativas verificadas en Update 5 (sin solape con hellwalker_*.decl)
# para que cada fila de mod tenga un ancla independiente incluso antes de inyectar la DLL.
$rowAnchors = @(
    @{ dataSet = 'idUIDataSet_Settings_Controls'; propertyName = 'controllerInvertLookHorizontalAutomap' },
    @{ dataSet = 'idUIDataSet_Settings_Controls'; propertyName = 'controllerInvertLookVerticalAutomap' },
    @{ dataSet = 'idUIDataSet_Settings_Controls'; propertyName = 'controllerInvertLookHorizontalDragonOnly' },
    @{ dataSet = 'idUIDataSet_Settings_Controls'; propertyName = 'controllerInvertLookDragonOnly' },
    @{ dataSet = 'idUIDataSet_Settings_Controls'; propertyName = 'mouseInvertLookHorizontalAutomap' },
    @{ dataSet = 'idUIDataSet_Settings_Controls'; propertyName = 'mouseInvertLookVerticalAutomap' },
    @{ dataSet = 'idUIDataSet_Settings_Controls'; propertyName = 'mouseInvertLookHorizontalDragonOnly' },
    @{ dataSet = 'idUIDataSet_Settings_Controls'; propertyName = 'mouseInvertLookDragonOnly' },
    @{ dataSet = 'idUIDataSet_Settings_Controls'; propertyName = 'dragonLockOnPressHoldToggle' },
    @{ dataSet = 'idUIDataSet_Settings_Controls'; propertyName = 'uiPressHoldToggle' }
)

# Fila toggle del mod: titulo = nombre exacto de la carpeta en mods\,
# descripcion = metadatos del paquete (autor/version/nombre interno) + instruccion neutra.
function Write-ModRowDecl([string]$menuId, [int]$rowIndex) {
    $anchor = $rowAnchors[$rowIndex % $rowAnchors.Count]
    $path = Join-Path $hellwalker "decls\setting\game\$menuId.decl"
    $content = @"
{
	edit = {
		dataSet = "$($anchor.dataSet)";
		propertyName = "$($anchor.propertyName)";
		uiData = {
			className = "idUIWidgetModel_Setting_RadioButton";
			object = {
				title = "ui/settings/game/modslist_row_$menuId";
				description = "ui/settings/game/modslist_desc_$menuId";
				items = {
					num = 2;
					item[0] = {
						name = {
							format = "ui/settings/option_off";
						}
					}
					item[1] = {
						name = {
							format = "ui/settings/option_on";
						}
						value = 1;
					}
				}
				useBoolForSetting = true;
			}
		}
	}
}
"@
    Set-Content -Path $path -Value $content -Encoding Ascii
}

# --- 3b) pantalla "Mods": DOS tabs fijas ---
# Tab 0 = Hellwalker (config completa, pagina estatica del repo).
# Tab 1 = "Mods" (biblioteca): UNA pagina, UNA seccion, el toggle de cada
# mod instalado identificado por el nombre de su carpeta en mods\.
$tabs = New-Object System.Collections.ArrayList
[void]$tabs.Add(@{ nonLocalized = 'Hellwalker'; page = 'ui/screens/settings/settings_page_hellwalker' })

$usedIds = @{}
$rows = New-Object System.Text.StringBuilder
$rowCount = 0
foreach ($m in $roster) {
    if ($rowCount -ge $maxRows) { break }
    if ($m.folder -eq 'Hellwalker' -or -not $m.present) { continue }
    $menuId = Get-MenuId $m.folder
    while ($usedIds.ContainsKey($menuId)) { $menuId = $menuId + '_' }
    $usedIds[$menuId] = $true

    # Nombre visible en el menu: siempre el nombre de la carpeta en mods\ (ej. SkipIntro, MoreGloryKill).
    $title = $m.folder

    $metaPartsEs = New-Object System.Collections.Generic.List[string]
    $metaPartsEn = New-Object System.Collections.Generic.List[string]
    if (-not [string]::IsNullOrWhiteSpace($m.name) -and $m.name -ne $m.folder) {
        [void]$metaPartsEs.Add("Paquete: $($m.name)")
        [void]$metaPartsEn.Add("Package: $($m.name)")
    }
    if (-not [string]::IsNullOrWhiteSpace($m.author)) {
        [void]$metaPartsEs.Add("Autor: $($m.author)")
        [void]$metaPartsEn.Add("Author: $($m.author)")
    }
    if (-not [string]::IsNullOrWhiteSpace($m.version)) {
        [void]$metaPartsEs.Add("Version: $($m.version)")
        [void]$metaPartsEn.Add("Version: $($m.version)")
    }

    $baseDescEs = 'Habilita o deshabilita la carga de este mod. Los cambios se aplican al reiniciar el juego.'
    $baseDescEn = 'Enable or disable loading this mod. Changes take effect on the next game launch.'
    $descEs = if ($metaPartsEs.Count -gt 0) { "[$($metaPartsEs -join ' | ')] $baseDescEs" } else { $baseDescEs }
    $descEn = if ($metaPartsEn.Count -gt 0) { "[$($metaPartsEn -join ' | ')] $baseDescEn" } else { $baseDescEn }

    Write-ModRowDecl $menuId $rowCount
    Write-StringDecl 'ui\settings\game' "modslist_row_$menuId" $title $title
    Write-StringDecl 'ui\settings\game' "modslist_desc_$menuId" $descEs $descEn
    [void]$rows.Append("`t`t`t`t`t`t`titem[$rowCount] = `"game/$menuId`";`n")
    $rowCount++
}

if ($rowCount -gt 0) {
    $libraryPage = @"
{
	inherit = "ui/screens/settings/settings_page_template";
	edit = {
		attributes = {
			object = {
				settingsSections = {
					num = 1;
					item[0] = {
						settingsList = {
							num = $rowCount;
$rows						}
						sectionName = "ui/settings/game/hellwalker_mods_section";
					}
				}
			}
		}
	}
}
"@
    $pageOpens  = ([regex]::Matches($libraryPage, '\{')).Count
    $pageCloses = ([regex]::Matches($libraryPage, '\}')).Count
    if ($pageOpens -ne $pageCloses) {
        Write-Host "[modmenu] ERROR: pagina biblioteca desbalanceada ($pageOpens/$pageCloses); no se escribe."
        exit 1
    }
    $libraryPath = Join-Path $hellwalker 'decls\uiwidget\ui\screens\settings\settings_page_mods_library.decl'
    Set-Content -Path $libraryPath -Value $libraryPage -Encoding Ascii
    [void]$tabs.Add(@{ label = 'ui/settings/game/hellwalker_mods_library_tab'; page = 'ui/screens/settings/settings_page_mods_library' })
}

$tabCount = $tabs.Count
$navText = New-Object System.Text.StringBuilder
for ($i = 0; $i -lt $tabCount; $i++) {
    $labelLine = if ($tabs[$i].nonLocalized) {
        "labelNonLocalized = `"$($tabs[$i].nonLocalized)`";"
    } else {
        "label = `"$($tabs[$i].label)`";"
    }
    $itemBlock = @"
						item[$i] = {
							navItem = {
								widget = "ui/elements/button_tab";
								model = {
									className = "idUIWidgetModel_Button";
									object = {
										$labelLine
										icon = "textures/ui/icons/settings/icon_set_general.png";
									}
								}
							}
							contentWidget = "$($tabs[$i].page)";
						}
"@
    [void]$navText.Append($itemBlock + "`n")
}

$screenDecl = @"
{
	inherit = "ui/base/menu_screen";
	edit = {
		attributes = {
			className = "idUIWidgetAttributes";
		}
		model = {
			className = "idUIWidgetModel";
		}
		updateOnFocus = true;
		initialState = "normal";
		eventDescriptors = {
			"UI_EVENT_PRESS" = remove;
		}
		childWidgets = {
			num = 1;
			item[0] = {
				name = "layout";
				type = "ui/prefabs/layout_tabs_settings";
				modelOverride = {
					className = "idUIWidgetModel_Layout";
					object = {
						navItems = {
							num = $tabCount;
$navText						}
						changeContentOnHover = false;
					}
				}
			}
		}
	}
}
"@

$screenPath = Join-Path $hellwalker 'decls\uiwidget\ui\screens\mainmenu_hellwalker_mods\mainmenu_hellwalker_mods.decl'
$opens  = ([regex]::Matches($screenDecl, '\{')).Count
$closes = ([regex]::Matches($screenDecl, '\}')).Count
if ($opens -ne $closes) {
    Write-Host "[modmenu] ERROR: pantalla 'Mods' desbalanceada ($opens/$closes); no se escribe."
    exit 1
}
$screenDir = Split-Path $screenPath -Parent
if (-not (Test-Path $screenDir)) { New-Item -ItemType Directory -Path $screenDir -Force | Out-Null }
Set-Content -Path $screenPath -Value $screenDecl -Encoding Ascii
Write-Host "[modmenu] pantalla 'Mods': $tabCount tab(s) (primero Hellwalker). Accesibilidad restaurada a vanilla."
