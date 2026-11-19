#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace survival_shield {

// Ficha de un decl registrado (fase 2 "traductor"): lo mínimo para saber
// qué toca un mod y detectar solapamientos con otros mods.
struct DeclInfo {
    std::string type;      // primer nivel bajo decls/: "uiwidget", "setting"...
    std::string name;      // ruta relativa sin extensión: "ui/screens/menu_bethesda_logo"
    std::string inherit;   // valor de inherit = "..." en la raíz, si existe
    std::string className; // valor de edit.class, si existe
    // Rutas de propiedades bajo edit{}, aplanadas con puntos
    // ("edit.model.object.duration"). Tope fijo por decl.
    std::vector<std::string> props;
};

class DeclReader {
public:
    static constexpr std::size_t kMaxProps = 32;

    // Parsea un archivo .decl. Llena type/name desde la ruta relativa a
    // declsRoot; inherit/className/props del contenido. Devuelve false si el
    // archivo no se pudo leer o no tiene estructura de bloque.
    static bool ParseFile(const std::filesystem::path& declsRoot,
                          const std::filesystem::path& file, DeclInfo* out);

    // Parsea texto ya leído; no toca type/name. Expuesto para tests.
    static bool ParseText(const std::string& text, DeclInfo* out);
};

} // namespace survival_shield
