#pragma once

#include "DeclReader.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace survival_shield {

// Fase 1 "lector" de la biblioteca de mods: registra los mods que los
// modders depositan en <juego>\mods\ leyendo su darkagesmod.txt, y los
// conserva en un registro persistente aunque la carpeta desaparezca. No
// carga ni ejecuta nada del mod: sólo lee y cataloga.
//
// Un mod con manifest roto NUNCA tira el escaneo: el problema queda
// registrado en entry.error y en el log, y el resto de la biblioteca sigue.
struct ModLibraryEntry {
    std::string folder;        // nombre de la carpeta dentro de mods/
    std::string name;          // modinfo.name (cae al nombre de carpeta si falta)
    std::string author;
    std::string version;
    std::string description;
    std::string game;
    int loadPriority = 0;
    int requiredVersion = 0;
    int declFiles = 0;         // archivos bajo decls/ de la carpeta del mod
    // Inventario de decls por tipo (primer nivel bajo decls/), base del
    // futuro traductor: {"uiwidget": 12, "setting": 7}.
    std::map<std::string, int> declTypes;
    // Fichas de los decls leídos (fase 2 "traductor"). Tope fijo por mod.
    std::vector<DeclInfo> decls;
    bool enabled = true;       // carpeta sin sufijo .off / .disabled
    bool present = true;       // visto en el último escaneo (false: dado de baja)
    std::string error;         // avisos de diagnóstico del manifest (vacío = OK)
    std::string firstSeen;     // fecha local ISO-8601 sin zona
    std::string lastSeen;
};

// Solape de decls: dos o más mods activos editan el mismo decl; el
// loadPriority de Atlan decide cuál gana, pero la biblioteca lo marca.
// props son las rutas de propiedades que los mods en pugna editan en común
// (intersección); vacío si tocan partes distintas del mismo decl.
struct DeclConflict {
    std::string decl;              // "uiwidget/ui/screens/menu_bethesda_logo"
    std::vector<std::string> mods; // carpetas, ordenadas por prioridad
    std::vector<std::string> props;
};

class ModLibrary {
public:
    // Un mod comunitario real trae 124 decls; 512 da margen. Las fichas son
    // livianas (paths acotados), el tope protege tiempo de escaneo.
    static constexpr std::size_t kMaxDeclsPerMod = 512;

    // Escanea modsDir (carpetas con darkagesmod.txt) y fusiona el resultado
    // con registryPath. Devuelve false si el registro no pudo guardarse.
    bool Scan(const std::filesystem::path& modsDir,
              const std::filesystem::path& registryPath);

    const std::vector<ModLibraryEntry>& Entries() const noexcept { return entries_; }

    // Solapes de la última pasada: decls editados por más de un mod activo.
    const std::vector<DeclConflict>& Conflicts() const noexcept { return conflicts_; }

    std::string ToJson() const;

    // Parse del bloque modinfo de un darkagesmod.txt. Expuesto estático para
    // los tests; no toca el filesystem.
    static ModLibraryEntry ParseManifest(const std::string& text,
                                         const std::string& folderName);

private:
    std::vector<ModLibraryEntry> entries_;
    std::vector<DeclConflict> conflicts_;
};

} // namespace survival_shield
