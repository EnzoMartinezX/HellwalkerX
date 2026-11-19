#include "ModLibrary.hpp"
#include "JsonUtil.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <cwctype>

namespace survival_shield {
namespace {

std::string LocalTimestamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02u",
                  time.wYear, time.wMonth, time.wDay,
                  time.wHour, time.wMinute, time.wSecond);
    return buffer;
}

std::string Trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

std::string StripQuotes(std::string value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

bool HasDisabledSuffix(const std::wstring& folderName) {
    for (const wchar_t* suffix : {L".off", L".disabled"}) {
        const std::size_t length = std::wcslen(suffix);
        if (folderName.size() >= length) {
            std::wstring tail = folderName.substr(folderName.size() - length);
            std::transform(tail.begin(), tail.end(), tail.begin(),
                           [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            if (tail == suffix) {
                return true;
            }
        }
    }
    return false;
}

std::string StripDisabledSuffix(const std::string& folder) {
    std::string name = folder;
    for (const std::string_view suffix : {".off", ".disabled"}) {
        if (name.size() > suffix.size()) {
            std::string tail = name.substr(name.size() - suffix.size());
            std::transform(tail.begin(), tail.end(), tail.begin(), [](char c) {
                return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            });
            if (tail == suffix) {
                name.resize(name.size() - suffix.size());
                break;
            }
        }
    }
    return name;
}

// Inventario de decls por tipo: cuenta los archivos bajo decls/ agrupados
// por su primer nivel de carpeta (uiwidget, setting, string...). Es la
// materia prima del futuro traductor. El total es entry.declFiles.
std::map<std::string, int> InventoryDeclTypes(const std::filesystem::path& modFolder) {
    std::map<std::string, int> types;
    std::error_code error;
    const auto declsRoot = modFolder / L"decls";
    for (auto it = std::filesystem::recursive_directory_iterator(
             declsRoot, std::filesystem::directory_options::skip_permission_denied, error);
         it != std::filesystem::recursive_directory_iterator();
         it.increment(error)) {
        if (error) {
            break;
        }
        if (!it->is_regular_file(error)) {
            continue;
        }
        std::error_code relativeError;
        const auto relative =
            std::filesystem::relative(it->path(), declsRoot, relativeError);
        if (relativeError) {
            continue;
        }
        const std::string type = relative.begin()->string();
        ++types[type];
    }
    return types;
}

std::vector<std::string> SplitArrayStrings(std::string_view arraySlice) {
    std::vector<std::string> out;
    std::size_t position = 0;
    SkipWhitespace(arraySlice, position);
    if (position >= arraySlice.size() || arraySlice[position++] != '[') {
        return out;
    }

    while (position < arraySlice.size()) {
        SkipWhitespace(arraySlice, position);
        if (position >= arraySlice.size() || arraySlice[position] == ']') {
            break;
        }
        const std::size_t start = position;
        if (!SkipJsonValue(arraySlice, position)) {
            break;
        }
        out.push_back(DecodeJsonString(arraySlice.substr(start, position - start)));
        SkipWhitespace(arraySlice, position);
        if (position < arraySlice.size() && arraySlice[position] == ',') {
            ++position;
        }
    }
    return out;
}

std::vector<std::string_view> SplitArrayObjects(std::string_view arraySlice) {
    std::vector<std::string_view> objects;
    std::size_t position = 0;
    SkipWhitespace(arraySlice, position);
    if (position >= arraySlice.size() || arraySlice[position++] != '[') {
        return objects;
    }

    while (position < arraySlice.size()) {
        SkipWhitespace(arraySlice, position);
        if (position >= arraySlice.size()) {
            break;
        }
        if (arraySlice[position] == ']') {
            break;
        }
        if (arraySlice[position] != '{') {
            break;
        }
        const std::size_t start = position;
        if (!SkipJsonValue(arraySlice, position)) {
            break;
        }
        objects.push_back(arraySlice.substr(start, position - start));
        SkipWhitespace(arraySlice, position);
        if (position < arraySlice.size() && arraySlice[position] == ',') {
            ++position;
        }
    }
    return objects;
}

std::string ReadStringField(std::string_view object, std::string_view key) {
    const auto value = FindJsonValue(object, key);
    return value ? DecodeJsonString(*value) : std::string{};
}

int ReadIntField(std::string_view object, std::string_view key) {
    const auto value = FindJsonValue(object, key);
    if (!value) {
        return 0;
    }
    int parsed = 0;
    const auto result = std::from_chars(value->data(), value->data() + value->size(), parsed);
    return (result.ec == std::errc{}) ? parsed : 0;
}

} // namespace

ModLibraryEntry ModLibrary::ParseManifest(const std::string& text,
                                          const std::string& folderName) {
    ModLibraryEntry entry;
    entry.folder = folderName;

    std::map<std::string, std::string> fields;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed.rfind("//", 0) == 0 || trimmed[0] == '#') {
            continue;
        }
        const auto equals = trimmed.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = Trim(std::string_view(trimmed).substr(0, equals));
        std::string value = StripQuotes(Trim(std::string_view(trimmed).substr(equals + 1)));
        value = Trim(value);
        if (!key.empty() && fields.find(key) == fields.end()) {
            fields.emplace(key, value);
        }
    }

    const auto assign = [&fields](const char* key, std::string& target) {
        if (auto found = fields.find(key); found != fields.end()) {
            target = found->second;
        }
    };
    assign("name", entry.name);
    assign("author", entry.author);
    assign("version", entry.version);
    assign("description", entry.description);
    assign("game", entry.game);
    if (entry.name.empty()) {
        entry.name = folderName;
    }

    // Diagnóstico no fatal: los problemas quedan en entry.error y el
    // escaneo sigue. Un manifest sin campos útiles es el único caso grave.
    if (fields.empty()) {
        entry.error = "manifest sin campos modinfo legibles";
    }
    const auto checkIntField = [&fields, &entry](const char* key, int& target) {
        if (auto found = fields.find(key); found != fields.end()) {
            const auto result = std::from_chars(found->second.data(),
                                                found->second.data() + found->second.size(),
                                                target);
            if (result.ec != std::errc{} || result.ptr != found->second.data() + found->second.size()) {
                target = 0;
                entry.error = entry.error.empty()
                                  ? std::string(key) + " no es numérico"
                                  : entry.error + "; " + key + " no es numérico";
            }
        }
    };
    checkIntField("loadPriority", entry.loadPriority);
    checkIntField("requiredVersion", entry.requiredVersion);
    return entry;
}

bool ModLibrary::Scan(const std::filesystem::path& modsDir,
                      const std::filesystem::path& registryPath) {
    const std::string now = LocalTimestamp();

    // 1) Registro previo: la biblioteca conserva los mods dados de baja.
    std::map<std::string, ModLibraryEntry> merged;
    std::map<std::string, DeclConflict> previousConflicts;
    {
        std::ifstream file(registryPath, std::ios::binary);
        if (file) {
            const std::string json((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
            const auto modsArray = FindJsonValue(json, "mods");
            if (modsArray) {
                for (const std::string_view object : SplitArrayObjects(*modsArray)) {
                    ModLibraryEntry entry;
                    entry.folder = ReadStringField(object, "folder");
                    if (entry.folder.empty()) {
                        continue;
                    }
                    entry.name = ReadStringField(object, "name");
                    entry.author = ReadStringField(object, "author");
                    entry.version = ReadStringField(object, "version");
                    entry.description = ReadStringField(object, "description");
                    entry.game = ReadStringField(object, "game");
                    entry.loadPriority = ReadIntField(object, "loadPriority");
                    entry.requiredVersion = ReadIntField(object, "requiredVersion");
                    entry.declFiles = ReadIntField(object, "declFiles");
                    if (const auto types = FindJsonValue(object, "declTypes")) {
                        std::size_t typePosition = 0;
                        SkipWhitespace(*types, typePosition);
                        if (typePosition < types->size() && (*types)[typePosition] == '{') {
                            ++typePosition;
                            SkipWhitespace(*types, typePosition);
                            while (typePosition < types->size() && (*types)[typePosition] != '}') {
                                if ((*types)[typePosition] != '"') {
                                    break;
                                }
                                const std::size_t keyStart = ++typePosition;
                                if (!SkipJsonString(*types, --typePosition)) {
                                    break;
                                }
                                const std::string typeKey(
                                    types->substr(keyStart, typePosition - 1U - keyStart));
                                SkipWhitespace(*types, typePosition);
                                if (typePosition >= types->size() ||
                                    (*types)[typePosition++] != ':') {
                                    break;
                                }
                                SkipWhitespace(*types, typePosition);
                                const std::size_t valueStart = typePosition;
                                if (!SkipJsonValue(*types, typePosition)) {
                                    break;
                                }
                                int typeCount = 0;
                                std::from_chars(types->data() + valueStart,
                                                types->data() + typePosition, typeCount);
                                if (!typeKey.empty()) {
                                    entry.declTypes[typeKey] = typeCount;
                                }
                                SkipWhitespace(*types, typePosition);
                                if (typePosition < types->size() &&
                                    (*types)[typePosition] == ',') {
                                    ++typePosition;
                                    SkipWhitespace(*types, typePosition);
                                }
                            }
                        }
                    }
                    entry.enabled = ReadIntField(object, "enabled") != 0;
                    entry.present = ReadIntField(object, "present") != 0;
                    entry.error = ReadStringField(object, "error");
                    if (const auto declsArray = FindJsonValue(object, "decls")) {
                        for (const std::string_view declObject :
                             SplitArrayObjects(*declsArray)) {
                            DeclInfo info;
                            info.type = ReadStringField(declObject, "type");
                            info.name = ReadStringField(declObject, "name");
                            info.className = ReadStringField(declObject, "class");
                            info.inherit = ReadStringField(declObject, "inherit");
                            if (const auto props = FindJsonValue(declObject, "props")) {
                                info.props = SplitArrayStrings(*props);
                            }
                            if (!info.name.empty()) {
                                entry.decls.push_back(std::move(info));
                            }
                        }
                    }
                    entry.firstSeen = ReadStringField(object, "firstSeen");
                    entry.lastSeen = ReadStringField(object, "lastSeen");
                    if (entry.name.empty()) {
                        entry.name = entry.folder;
                    }
                    const std::string key = StripDisabledSuffix(entry.folder);
                    auto prevIt = merged.find(key);
                    if (prevIt == merged.end()) {
                        merged.emplace(key, std::move(entry));
                    } else if (entry.present && !prevIt->second.present) {
                        prevIt->second = std::move(entry);
                    }
                }
            }
            if (const auto conflictsArray = FindJsonValue(json, "conflicts")) {
                for (const std::string_view conflictObject :
                     SplitArrayObjects(*conflictsArray)) {
                    DeclConflict conflict;
                    conflict.decl = ReadStringField(conflictObject, "decl");
                    if (conflict.decl.empty()) {
                        continue;
                    }
                    if (const auto modsArray = FindJsonValue(conflictObject, "mods")) {
                        conflict.mods = SplitArrayStrings(*modsArray);
                    }
                    if (const auto propsArray = FindJsonValue(conflictObject, "props")) {
                        conflict.props = SplitArrayStrings(*propsArray);
                    }
                    previousConflicts[conflict.decl] = std::move(conflict);
                }
            }
        }
    }

    // 2) Escaneo del directorio de mods del juego.
    int scanned = 0;
    std::set<std::string> seenFolders;
    std::error_code error;
    for (auto it = std::filesystem::directory_iterator(
             modsDir, std::filesystem::directory_options::skip_permission_denied, error);
         it != std::filesystem::directory_iterator();
         it.increment(error)) {
        if (error) {
            break;
        }
        if (!it->is_directory(error)) {
            continue;
        }
        const auto folder = it->path();
        const std::filesystem::path manifest = folder / L"darkagesmod.txt";
        if (!std::filesystem::exists(manifest, error)) {
            continue;
        }

        std::ifstream manifestFile(manifest, std::ios::binary);
        if (!manifestFile) {
            continue;
        }
        const std::string text((std::istreambuf_iterator<char>(manifestFile)),
                               std::istreambuf_iterator<char>());

        const std::string folderName = folder.filename().string();
        const std::string canonical = StripDisabledSuffix(folderName);
        seenFolders.insert(canonical);
        ModLibraryEntry entry = ParseManifest(text, folderName);
        entry.declTypes = InventoryDeclTypes(folder);
        int declTotal = 0;
        for (const auto& [type, count] : entry.declTypes) {
            declTotal += count;
        }
        entry.declFiles = declTotal;

        // Fase 2 "traductor": fichas de los decls del mod.
        const auto declsRoot = folder / L"decls";
        for (auto declIt = std::filesystem::recursive_directory_iterator(
                 declsRoot, std::filesystem::directory_options::skip_permission_denied, error);
             declIt != std::filesystem::recursive_directory_iterator();
             declIt.increment(error)) {
            if (error || entry.decls.size() >= kMaxDeclsPerMod) {
                break;
            }
            if (!declIt->is_regular_file(error)) {
                continue;
            }
            const auto declPath = declIt->path();
            if (declPath.extension().wstring() != L".decl") {
                continue;
            }
            DeclInfo info;
            if (DeclReader::ParseFile(declsRoot, declPath, &info)) {
                entry.decls.push_back(std::move(info));
            }
        }

        entry.enabled = !HasDisabledSuffix(folder.filename().wstring());
        entry.present = true;
        entry.lastSeen = now;
        ++scanned;

        auto existing = merged.find(canonical);
        if (existing != merged.end()) {
            entry.firstSeen = existing->second.firstSeen.empty() ? now
                                                                : existing->second.firstSeen;
            existing->second = std::move(entry);
        } else {
            entry.firstSeen = now;
            merged.emplace(canonical, std::move(entry));
        }
    }

    for (auto& [folder, entry] : merged) {
        if (seenFolders.find(folder) == seenFolders.end()) {
            // Registrado antes pero no visto en este escaneo: dado de baja.
            entry.present = false;
        }
    }

    entries_.clear();
    entries_.reserve(merged.size());
    for (auto& [folder, entry] : merged) {
        entries_.push_back(std::move(entry));
    }
    std::sort(entries_.begin(), entries_.end(), [](const ModLibraryEntry& a,
                                                   const ModLibraryEntry& b) {
        if (a.loadPriority != b.loadPriority) {
            return a.loadPriority > b.loadPriority;
        }
        return a.folder < b.folder;
    });

    // Solapes: decls editados por más de un mod activo y presente. El
    // loadPriority de Atlan decide el ganador; la biblioteca lo reporta.
    // Por cada decl en pugna se calcula además la intersección de rutas de
    // propiedades: son las que realmente compiten.
    struct DeclOwnership {
        std::vector<std::string> mods;                 // orden de prioridad
        std::map<std::string, std::set<std::string>> propsByMod;
    };
    std::map<std::string, DeclOwnership> declOwners;
    for (const auto& entry : entries_) {
        if (!entry.enabled || !entry.present) {
            continue;
        }
        for (const auto& decl : entry.decls) {
            DeclOwnership& ownership = declOwners[decl.type + "/" + decl.name];
            ownership.mods.push_back(entry.folder);
            auto& props = ownership.propsByMod[entry.folder];
            props.insert(decl.props.begin(), decl.props.end());
        }
    }
    // Los solapes ya registrados para mods que ya no están se conservan.
    std::map<std::string, DeclConflict> mergedConflicts;
    for (auto& [decl, ownership] : declOwners) {
        if (ownership.mods.size() < 2) {
            continue;
        }
        DeclConflict conflict;
        conflict.decl = decl;
        conflict.mods = std::move(ownership.mods);
        // Intersección de props entre todos los mods en pugna.
        auto first = ownership.propsByMod.find(conflict.mods.front());
        if (first != ownership.propsByMod.end()) {
            for (const auto& prop : first->second) {
                bool shared = true;
                for (std::size_t i = 1; i < conflict.mods.size() && shared; ++i) {
                    const auto other = ownership.propsByMod.find(conflict.mods[i]);
                    shared = other != ownership.propsByMod.end() &&
                             other->second.count(prop) != 0;
                }
                if (shared) {
                    conflict.props.push_back(prop);
                }
            }
        }
        mergedConflicts[decl] = std::move(conflict);
    }
    // Los solapes ya registrados sobreviven, pero podados: sólo cuentan los
    // mods que siguen presentes y activos; si queda uno solo, no hay solape.
    for (auto& [decl, previous] : previousConflicts) {
        if (mergedConflicts.find(decl) != mergedConflicts.end()) {
            continue;
        }
        std::vector<std::string> alive;
        for (const auto& modFolder : previous.mods) {
            const auto owner = merged.find(StripDisabledSuffix(modFolder));
            if (owner != merged.end() && owner->second.present && owner->second.enabled) {
                alive.push_back(modFolder);
            }
        }
        if (alive.size() >= 2) {
            DeclConflict conflict;
            conflict.decl = decl;
            conflict.mods = std::move(alive);
            conflict.props = previous.props; // se conserva el último análisis
            mergedConflicts[decl] = std::move(conflict);
        }
    }
    conflicts_.clear();
    conflicts_.reserve(mergedConflicts.size());
    for (auto& [decl, conflict] : mergedConflicts) {
        conflicts_.push_back(std::move(conflict));
    }

    // 3) Persistencia atómica del registro.
    if (registryPath.empty()) {
        return false;
    }
    std::filesystem::create_directories(registryPath.parent_path(), error);
    if (error) {
        return false;
    }

    std::string json = ToJson();
    const auto tempPath = registryPath.wstring() + L".tmp";
    {
        std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
        if (!file) {
            return false;
        }
        file << json;
        file.flush();
        if (!file) {
            return false;
        }
    }
    return MoveFileExW(tempPath.c_str(), registryPath.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

std::string ModLibrary::ToJson() const {
    std::string json = "{\n  \"schema\": 1,\n  \"mods\": [\n";
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const ModLibraryEntry& entry = entries_[i];
        json += "    {";
        json += "\"folder\": " + JsonEscape(entry.folder);
        json += ", \"name\": " + JsonEscape(entry.name);
        json += ", \"author\": " + JsonEscape(entry.author);
        json += ", \"version\": " + JsonEscape(entry.version);
        json += ", \"description\": " + JsonEscape(entry.description);
        json += ", \"game\": " + JsonEscape(entry.game);
        json += ", \"loadPriority\": " + std::to_string(entry.loadPriority);
        json += ", \"requiredVersion\": " + std::to_string(entry.requiredVersion);
        json += ", \"declFiles\": " + std::to_string(entry.declFiles);
        json += ", \"declTypes\": {";
        {
            bool firstType = true;
            for (const auto& [type, count] : entry.declTypes) {
                if (!firstType) {
                    json += ", ";
                }
                firstType = false;
                json += JsonEscape(type) + ": " + std::to_string(count);
            }
        }
        json += "}";
        json += ", \"decls\": [";
        for (std::size_t d = 0; d < entry.decls.size(); ++d) {
            const DeclInfo& decl = entry.decls[d];
            json += (d > 0) ? ", {" : "{";
            json += "\"type\": " + JsonEscape(decl.type);
            json += ", \"name\": " + JsonEscape(decl.name);
            json += ", \"class\": " + JsonEscape(decl.className);
            json += ", \"inherit\": " + JsonEscape(decl.inherit);
            json += ", \"props\": [";
            for (std::size_t p = 0; p < decl.props.size(); ++p) {
                json += (p > 0) ? ", " : "";
                json += JsonEscape(decl.props[p]);
            }
            json += "]}";
        }
        json += "]";
        json += std::string(", \"enabled\": ") + (entry.enabled ? "true" : "false");
        json += std::string(", \"present\": ") + (entry.present ? "true" : "false");
        json += ", \"error\": " + JsonEscape(entry.error);
        json += ", \"firstSeen\": " + JsonEscape(entry.firstSeen);
        json += ", \"lastSeen\": " + JsonEscape(entry.lastSeen);
        json += "}";
        json += (i + 1 < entries_.size()) ? ",\n" : "\n";
    }
    json += "  ]";
    json += ",\n  \"conflicts\": [\n";
    for (std::size_t i = 0; i < conflicts_.size(); ++i) {
        const DeclConflict& conflict = conflicts_[i];
        json += "    {";
        json += "\"decl\": " + JsonEscape(conflict.decl);
        json += ", \"mods\": [";
        for (std::size_t m = 0; m < conflict.mods.size(); ++m) {
            json += (m > 0) ? ", " : "";
            json += JsonEscape(conflict.mods[m]);
        }
        json += "], \"props\": [";
        for (std::size_t p = 0; p < conflict.props.size(); ++p) {
            json += (p > 0) ? ", " : "";
            json += JsonEscape(conflict.props[p]);
        }
        json += "]}";
        json += (i + 1 < conflicts_.size()) ? ",\n" : "\n";
    }
    json += "  ]\n}\n";
    return json;
}

} // namespace survival_shield
