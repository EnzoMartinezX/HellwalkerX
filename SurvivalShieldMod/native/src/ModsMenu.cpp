#include "ModsMenu.hpp"
#include "JsonUtil.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cctype>
#include <fstream>
#include <iterator>
#include <mutex>
#include <system_error>

namespace survival_shield {
namespace {

std::mutex g_rowsMutex;
std::map<std::string, std::pair<std::string, bool>> g_rows; // menuId -> {folder, enabled}
std::mutex g_desiredMutex;
std::map<std::string, bool> g_desired;

std::string StripDisabledSuffix(const std::string& folder) {
    std::string name = folder;
    for (const std::string suffix : {".off", ".disabled"}) {
        if (name.size() > suffix.size()) {
            std::string tail = name.substr(name.size() - suffix.size());
            std::transform(tail.begin(), tail.end(), tail.begin(),
                           [](char c) {
                               return static_cast<char>(
                                   std::tolower(static_cast<unsigned char>(c)));
                           });
            if (tail == suffix) {
                name.resize(name.size() - suffix.size());
                break;
            }
        }
    }
    return name;
}

} // namespace

std::string MenuIdForFolder(const std::string& folder) {
    // El sufijo de carpeta deshabilitada se elimina ANTES de sanear: la fila
    // de un mod conserva su id al togglearlo (X y X.off son el mismo mod).
    const std::string name = StripDisabledSuffix(folder);

    std::string out;
    out.reserve(name.size());
    for (const char current : name) {
        const char lowered =
            static_cast<char>(std::tolower(static_cast<unsigned char>(current)));
        const bool ok = (lowered >= 'a' && lowered <= 'z') ||
                        (lowered >= '0' && lowered <= '9') || lowered == '_';
        out.push_back(ok ? lowered : '_');
    }

    std::string collapsed;
    collapsed.reserve(out.size());
    for (const char current : out) {
        if (current == '_' && (!collapsed.empty() && collapsed.back() == '_')) {
            continue;
        }
        collapsed.push_back(current);
    }
    while (!collapsed.empty() && collapsed.front() == '_') {
        collapsed.erase(collapsed.begin());
    }
    while (!collapsed.empty() && collapsed.back() == '_') {
        collapsed.pop_back();
    }
    return collapsed.empty() ? "mod" : collapsed;
}

void ModsMenuSetRows(std::map<std::string, std::pair<std::string, bool>> rows) {
    std::lock_guard lock(g_rowsMutex);
    g_rows = std::move(rows);
}

bool ModsMenuLookupRow(const std::wstring& menuId, std::string* folder, bool* enabledNow) {
    std::string narrow;
    narrow.reserve(menuId.size());
    for (const wchar_t current : menuId) {
        narrow.push_back(static_cast<char>(current)); // ids generados: ASCII
    }

    std::lock_guard lock(g_rowsMutex);
    const auto found = g_rows.find(narrow);
    if (found == g_rows.end()) {
        return false;
    }
    if (folder != nullptr) {
        *folder = found->second.first;
    }
    if (enabledNow != nullptr) {
        *enabledNow = found->second.second;
    }
    return true;
}

std::map<std::string, bool> ModsMenuLoadDesired(const std::filesystem::path& statePath) {
    std::map<std::string, bool> desired;
    std::ifstream file(statePath, std::ios::binary);
    if (!file) {
        return desired;
    }
    const std::string json((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());

    std::size_t position = 0;
    SkipWhitespace(json, position);
    if (position >= json.size() || json[position++] != '{') {
        return desired;
    }
    SkipWhitespace(json, position);
    while (position + 1 < json.size() && json[position] != '}') {
        const std::size_t keyStart = position;
        if (!SkipJsonString(json, position)) {
            break;
        }
        const std::string key = DecodeJsonString(json.substr(keyStart, position - keyStart));
        SkipWhitespace(json, position);
        if (position >= json.size() || json[position++] != ':') {
            break;
        }
        SkipWhitespace(json, position);
        const std::size_t valueStart = position;
        if (!SkipJsonValue(json, position)) {
            break;
        }
        const std::string value(json.substr(valueStart, position - valueStart));
        desired[key] = value == "true";
        SkipWhitespace(json, position);
        if (position < json.size() && json[position] == ',') {
            ++position;
            SkipWhitespace(json, position);
        }
    }
    return desired;
}

bool ModsMenuSaveDesired(const std::map<std::string, bool>& desired,
                         const std::filesystem::path& statePath) {
    std::error_code error;
    std::filesystem::create_directories(statePath.parent_path(), error);
    if (error) {
        return false;
    }

    std::string json = "{\n";
    bool first = true;
    for (const auto& [folder, enabled] : desired) {
        json += first ? "  " : ",\n  ";
        first = false;
        json += JsonEscape(folder) + ": " + (enabled ? "true" : "false");
    }
    json += "\n}\n";

    const auto tempPath = statePath.wstring() + L".tmp";
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
    return MoveFileExW(tempPath.c_str(), statePath.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

void ModsMenuInitializeDesired(const std::filesystem::path& statePath) {
    if (statePath.empty()) {
        return;
    }
    const auto loaded = ModsMenuLoadDesired(statePath);
    std::lock_guard lock(g_desiredMutex);
    for (const auto& [rawKey, desired] : loaded) {
        g_desired[StripDisabledSuffix(rawKey)] = desired;
    }
}

bool ModsMenuGetDesired(const std::string& folder, bool fallback, bool* out) {
    if (out == nullptr) {
        return false;
    }
    const std::string key = StripDisabledSuffix(folder);
    std::lock_guard lock(g_desiredMutex);
    const auto found = g_desired.find(key);
    if (found == g_desired.end()) {
        *out = fallback;
        return false;
    }
    *out = found->second;
    return true;
}

bool ModsMenuSetDesired(const std::string& folder, bool desired,
                        const std::filesystem::path& statePath) {
    const std::string key = StripDisabledSuffix(folder);
    std::map<std::string, bool> snapshot;
    {
        std::lock_guard lock(g_desiredMutex);
        g_desired[key] = desired;
        snapshot = g_desired;
    }
    return ModsMenuSaveDesired(snapshot, statePath);
}

std::filesystem::path ModsMenuStatePath() {
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path(std::wstring(path, n)) / L"DOOMTheDarkAges" /
           L"SurvivalShield" / L"mods_menu_state.json";
}

} // namespace survival_shield
