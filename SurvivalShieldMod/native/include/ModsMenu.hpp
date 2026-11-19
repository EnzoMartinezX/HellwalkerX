#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace survival_shield {

// Fase 3: puente entre la biblioteca de mods y el menú del juego.
// El generador (bin\generate_mods_menu.ps1) crea filas de settings con id
// "game/modslist_<menuId>" a partir de modlibrary.json; la DLL reconoce ese
// prefijo en el UiBridge, muestra/edita el estado y persiste el estado
// deseado; el .bat lo aplica renombrando carpetas (.off) antes de arrancar.

// Sanea el nombre de carpeta a id de fila. REGLA COMPARTIDA con
// generate_mods_menu.ps1: se recorta un sufijo .off/.disabled (X y X.off son
// el mismo mod y comparten fila), minúsculas, todo lo que no es [a-z0-9_] se
// vuelve '_', secuencias colapsadas, bordes recortados, vacío -> "mod".
std::string MenuIdForFolder(const std::string& folder);

// Registro de filas del menú (menuId -> carpeta + estado del escaneo).
// Thread-safe: lo alimenta el escaneo y lo consume el detour del UiBridge.
void ModsMenuSetRows(std::map<std::string, std::pair<std::string, bool>> rows);
bool ModsMenuLookupRow(const std::wstring& menuId, std::string* folder, bool* enabledNow);

// Estado deseado por el usuario (overrides del estado del escaneo). Se
// persiste en <dir>\mods_menu_state.json para que el launcher lo aplique.
void ModsMenuInitializeDesired(const std::filesystem::path& statePath);
bool ModsMenuGetDesired(const std::string& folder, bool fallback, bool* out);
bool ModsMenuSetDesired(const std::string& folder, bool desired,
                        const std::filesystem::path& statePath);
std::map<std::string, bool> ModsMenuLoadDesired(const std::filesystem::path& statePath);
bool ModsMenuSaveDesired(const std::map<std::string, bool>& desired,
                         const std::filesystem::path& statePath);

// %LOCALAPPDATA%\DOOMTheDarkAges\SurvivalShield\mods_menu_state.json
// (vacío si LOCALAPPDATA no está disponible).
std::filesystem::path ModsMenuStatePath();

} // namespace survival_shield
