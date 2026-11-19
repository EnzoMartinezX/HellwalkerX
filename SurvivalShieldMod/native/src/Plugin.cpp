#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "SettingsStore.hpp"
#include "SurvivalShieldCore.hpp"
#include "ModLibrary.hpp"
#include "ModsMenu.hpp"
#include "BuildGuard.hpp"
#include "BridgeConfig.hpp"
#include "Log.hpp"
#ifdef SURVIVALSHIELD_DIAGNOSTIC_HOOKS
#include "GameHooks.hpp"
#endif
#ifdef SURVIVALSHIELD_UI_BRIDGE
#include "UiBridge.hpp"
#include "GameplayBridge.hpp"
#endif

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <cstring>
#include <filesystem>
#include <algorithm>

namespace {

constexpr unsigned int kHudLockedBlueArgb = 0xFF68B5FF;
constexpr unsigned int kHudReadyLedRedArgb = 0xFFFF3038;

std::mutex g_initializeMutex;
std::unique_ptr<survival_shield::SurvivalShieldCore> g_core;
bool g_initialized = false;
bool g_settingsDirty = false;
survival_shield::ModLibrary g_modLibrary;
std::string g_modLibraryJson;
std::filesystem::path g_modsDir;

bool PersistSettingsLocked(const survival_shield::ModConfig& config) {
    survival_shield::SettingsStore store;
    if (store.Save(config)) {
        g_settingsDirty = false;
        return true;
    }

    g_settingsDirty = true;
    survival_shield::LogLine(L"Hellwalker: settings are active in memory but still need to be saved.");
    return false;
}

// Fase 1 "lector" de la biblioteca de mods: cataloga las carpetas con
// darkagesmod.txt bajo <juego>\mods\ en modlibrary.json. Requiere el mutex
// de inicialización tomado.
std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(needed > 0 ? needed : 0), L'\0');
    if (needed > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                            wide.data(), needed);
    }
    return wide;
}

void ScanModLibraryLocked() {
    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) {
        return;
    }
    g_modsDir = std::filesystem::path(exePath).parent_path() / L"mods";

    const auto registryPath =
        survival_shield::SettingsStore::StorageDirectory() / L"modlibrary.json";
    if (!g_modLibrary.Scan(g_modsDir, registryPath)) {
        survival_shield::LogLine(L"[modlib] could not persist modlibrary.json.");
        return;
    }

    const auto& entries = g_modLibrary.Entries();
    int active = 0;
    for (const auto& entry : entries) {
        if (entry.enabled) {
            ++active;
        }
        wchar_t summary[288]{};
        swprintf_s(summary,
                   L"[modlib] %s v%s (prioridad %d, %d decls, %d fichas, %s%s)",
                   Utf8ToWide(entry.name).c_str(),
                   Utf8ToWide(entry.version).c_str(),
                   entry.loadPriority, entry.declFiles,
                   static_cast<unsigned>(entry.decls.size()),
                   entry.enabled ? L"activo" : L"desactivado",
                   entry.present ? L"" : L", dado de baja");
        survival_shield::LogLine(summary);
        if (!entry.error.empty()) {
            wchar_t warning[288]{};
            swprintf_s(warning, L"[modlib]   AVISO %s: %s",
                       Utf8ToWide(entry.folder).c_str(),
                       Utf8ToWide(entry.error).c_str());
            survival_shield::LogLine(warning);
        }
    }

    for (const auto& conflict : g_modLibrary.Conflicts()) {
        std::wstring joined;
        for (std::size_t i = 0; i < conflict.mods.size(); ++i) {
            if (i > 0) {
                joined += L" x ";
            }
            joined += Utf8ToWide(conflict.mods[i]);
        }
        wchar_t mods[192]{};
        swprintf_s(mods, L"[modlib] SOLAPE %s: %s",
                   Utf8ToWide(conflict.decl).c_str(), joined.c_str());
        survival_shield::LogLine(mods);
        if (!conflict.props.empty()) {
            std::wstring props;
            for (std::size_t i = 0; i < conflict.props.size() && i < 5; ++i) {
                if (i > 0) {
                    props += L", ";
                }
                props += Utf8ToWide(conflict.props[i]);
            }
            if (conflict.props.size() > 5) {
                props += L", ...";
            }
            wchar_t propLine[320]{};
            swprintf_s(propLine, L"[modlib]   en pugna: %s", props.c_str());
            survival_shield::LogLine(propLine);
        }
    }

    wchar_t header[128]{};
    swprintf_s(header, L"[modlib] %u mod(s) en la biblioteca, %u activo(s), %u solape(s).",
               static_cast<unsigned>(entries.size()), static_cast<unsigned>(active),
               static_cast<unsigned>(g_modLibrary.Conflicts().size()));
    survival_shield::LogLine(header);
    g_modLibraryJson = g_modLibrary.ToJson();

    // Fase 3: filas del menú "Mods" (game/modslist_* generadas por el bat).
    survival_shield::ModsMenuInitializeDesired(survival_shield::ModsMenuStatePath());
    std::map<std::string, std::pair<std::string, bool>> rows;
    for (const auto& entry : entries) {
        if (!entry.present) {
            continue; // los dados de baja se listan, pero sin fila accionable
        }
        rows[survival_shield::MenuIdForFolder(entry.folder)] = {entry.folder, entry.enabled};
    }
    survival_shield::ModsMenuSetRows(std::move(rows));
}

} // namespace

extern "C" __declspec(dllexport) BOOL WINAPI SurvivalShield_Initialize(HMODULE) noexcept;

// When injected with LoadLibraryW there is no proxy to call
// SurvivalShield_Initialize, so self-initialize on process attach. All real
// work (file hashing, MinHook) stays off DllMain's loader-lock path.
extern "C" BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) noexcept {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        QueueUserWorkItem(
            [](PVOID) noexcept -> DWORD {
                SurvivalShield_Initialize(nullptr);
                return 0;
            },
            nullptr, WT_EXECUTEDEFAULT);
    }
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL WINAPI SurvivalShield_Initialize(HMODULE) noexcept {
    std::lock_guard lock(g_initializeMutex);
    if (g_initialized) {
        return TRUE;
    }

    try {
        survival_shield::SettingsStore store;
        survival_shield::InitializeLog(store.FilePath().parent_path());

        const auto build = survival_shield::CheckCurrentExecutable();
        if (!build.supported) {
            survival_shield::LogLine(
                (L"Unsupported executable SHA-256: " + build.sha256 + L". Hooks remain disabled.").c_str());
            return FALSE;
        }

        const auto config = store.LoadOrDefault();
        g_core = std::make_unique<survival_shield::SurvivalShieldCore>(config);
        g_settingsDirty = store.FilePath().empty() || !store.Save(config);
        if (g_settingsDirty) {
            survival_shield::LogLine(L"Could not persist settings.json; using defaults/current values.");
        }

        survival_shield::LogLine(L"Native core loaded.");
        ScanModLibraryLocked();
#ifdef SURVIVALSHIELD_NO_HOOKS
        survival_shield::LogLine(L"BISECT BUILD: all hooks disabled by compile flag.");
#elif defined(SURVIVALSHIELD_DIAGNOSTIC_HOOKS)
        if (!survival_shield::InstallLoggingHooks()) {
            survival_shield::LogLine(L"Diagnostic AddScore observer did not install.");
        }
#elif defined(SURVIVALSHIELD_UI_BRIDGE)
        if (!survival_shield::InstallUiBridge()) {
            survival_shield::LogLine(L"UI bridge did not install; settings menu stays vanilla.");
        }
        if (!survival_shield::InstallGameplayBridge()) {
            survival_shield::LogLine(L"Gameplay bridge did not install.");
        }
#else
        survival_shield::LogLine(
            L"Gameplay/UI hooks remain disabled until their runtime adapters are verified.");
#endif
        g_initialized = true;
        return TRUE;
    } catch (...) {
        return FALSE;
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI SurvivalShield_GetOptions(
    BOOL* enabled, int* targetScore) noexcept {
    if (enabled == nullptr || targetScore == nullptr) {
        return FALSE;
    }

    try {
        std::lock_guard lock(g_initializeMutex);
        if (!g_initialized || g_core == nullptr) {
            return FALSE;
        }

        const auto config = g_core->GetConfig();
        *enabled = config.enabled ? TRUE : FALSE;
        *targetScore = config.targetScore;
        return TRUE;
    } catch (...) {
        return FALSE;
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI SurvivalShield_GetPoweredDashDamage(
    int* damage) noexcept {
    if (damage == nullptr) {
        return FALSE;
    }
    try {
        std::lock_guard lock(g_initializeMutex);
        if (!g_initialized || g_core == nullptr) {
            return FALSE;
        }
        *damage = g_core->GetConfig().poweredDashDamage;
        return TRUE;
    } catch (...) {
        return FALSE;
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI SurvivalShield_SetPoweredDashDamage(
    int damage) noexcept {
    try {
        std::lock_guard lock(g_initializeMutex);
        if (!g_initialized || g_core == nullptr) {
            return FALSE;
        }
        auto config = g_core->GetConfig();
        config.poweredDashDamage = damage;
        g_core->SetConfig(config);
        config = g_core->GetConfig();
        return PersistSettingsLocked(config) ? TRUE : FALSE;
    } catch (...) {
        return FALSE;
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI SurvivalShield_GetSettings(
    BOOL* enabled,
    int* targetScore,
    int* poweredDashDamage,
    int* pctFodder,
    int* pctHeavy,
    int* pctSuperheavy) noexcept {
    if (enabled == nullptr || targetScore == nullptr || poweredDashDamage == nullptr ||
        pctFodder == nullptr || pctHeavy == nullptr || pctSuperheavy == nullptr) {
        return FALSE;
    }

    survival_shield::BridgeValues values{};
    if (!survival_shield::BridgeGetValues(&values)) {
        return FALSE;
    }
    *enabled = values.enabled ? TRUE : FALSE;
    *targetScore = values.targetScore;
    *poweredDashDamage = values.poweredDashDamage;
    *pctFodder = values.pctFodder;
    *pctHeavy = values.pctHeavy;
    *pctSuperheavy = values.pctSuperheavy;
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL WINAPI SurvivalShield_SetSettings(
    BOOL enabled,
    int targetScore,
    int poweredDashDamage,
    int pctFodder,
    int pctHeavy,
    int pctSuperheavy) noexcept {
    survival_shield::BridgeValues values{};
    if (!survival_shield::BridgeGetValues(&values)) {
        return FALSE; // preserve the stored skipIntro, not part of this ABI
    }
    values.enabled = enabled != FALSE;
    values.targetScore = targetScore;
    values.pctFodder = pctFodder;
    values.pctHeavy = pctHeavy;
    values.pctSuperheavy = pctSuperheavy;
    values.poweredDashDamage = poweredDashDamage;
    return survival_shield::BridgeSetValues(values) ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) BOOL WINAPI SurvivalShield_GetScoreState(
    unsigned long long* scorePoints, int* targetScore, BOOL* supercharged) noexcept {
    if (scorePoints == nullptr || targetScore == nullptr || supercharged == nullptr) {
        return FALSE;
    }
#ifdef SURVIVALSHIELD_UI_BRIDGE
    // The gameplay bridge owns the live accumulator; prefer it over the
    // isolated core, which only reflects RecordDemonKill calls.
    {
        unsigned long long points = 0;
        int target = 0;
        bool charged = false;
        if (survival_shield::GetLiveScore(&points, &target, &charged)) {
            *scorePoints = points;
            *targetScore = target;
            *supercharged = charged ? TRUE : FALSE;
            return TRUE;
        }
    }
#endif
    try {
        std::lock_guard lock(g_initializeMutex);
        if (!g_initialized || g_core == nullptr) {
            return FALSE;
        }
        const auto score = g_core->GetScoreSnapshot();
        *scorePoints = static_cast<unsigned long long>(score.scorePoints);
        *targetScore = score.targetScore;
        *supercharged = score.supercharged ? TRUE : FALSE;
        return TRUE;
    } catch (...) {
        return FALSE;
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI SurvivalShield_GetHudState(
    unsigned long long* scorePoints,
    int* targetScore,
    BOOL* supercharged,
    unsigned int* iconColorArgb) noexcept {
    if (scorePoints == nullptr || targetScore == nullptr || supercharged == nullptr ||
        iconColorArgb == nullptr) {
        return FALSE;
    }
#ifdef SURVIVALSHIELD_UI_BRIDGE
    {
        unsigned long long points = 0;
        int target = 0;
        bool charged = false;
        if (survival_shield::GetLiveScore(&points, &target, &charged)) {
            *scorePoints = points;
            *targetScore = target;
            *supercharged = charged ? TRUE : FALSE;
            *iconColorArgb = charged ? kHudReadyLedRedArgb : kHudLockedBlueArgb;
            return TRUE;
        }
    }
#endif
    try {
        std::lock_guard lock(g_initializeMutex);
        if (!g_initialized || g_core == nullptr) {
            return FALSE;
        }

        const auto score = g_core->GetScoreSnapshot();
        *scorePoints = static_cast<unsigned long long>(score.scorePoints);
        *targetScore = score.targetScore;
        *supercharged = score.supercharged ? TRUE : FALSE;
        *iconColorArgb = score.supercharged ? kHudReadyLedRedArgb : kHudLockedBlueArgb;
        return TRUE;
    } catch (...) {
        return FALSE;
    }
}

bool survival_shield::BridgeGetValues(BridgeValues* out) {
    if (out == nullptr) {
        return false;
    }
    try {
        std::lock_guard lock(g_initializeMutex);
        if (!g_initialized || g_core == nullptr) {
            return false;
        }
        const auto config = g_core->GetConfig();
        *out = BridgeValues{config.enabled, config.targetScore,
                            config.pctFodder, config.pctHeavy, config.pctSuperheavy,
                            config.poweredDashDamage, config.parryPct, config.skipIntro};
        return true;
    } catch (...) {
        return false;
    }
}

bool survival_shield::BridgeSetValues(const BridgeValues& values) {
    try {
        std::lock_guard lock(g_initializeMutex);
        if (!g_initialized || g_core == nullptr) {
            return false;
        }

        survival_shield::ModConfig config = g_core->GetConfig();
        config.enabled = values.enabled;
        const int boundedTarget = std::clamp(values.targetScore, 100, 10000);
        config.targetScore = ((boundedTarget + 50) / 100) * 100;
        config.pctFodder = std::clamp(values.pctFodder, 0, 100);
        config.pctHeavy = std::clamp(values.pctHeavy, 0, 100);
        config.pctSuperheavy = std::clamp(values.pctSuperheavy, 0, 100);
        const int boundedDamage = std::clamp(values.poweredDashDamage, 100, 5000);
        config.poweredDashDamage = ((boundedDamage + 25) / 50) * 50;
        const int boundedParry = std::clamp(values.parryPct, 0, 50);
        config.parryPct = ((boundedParry + 5) / 10) * 10;
        config.skipIntro = values.skipIntro;

        // The settings page re-syncs continuously; skip the file write when
        // nothing actually changed.
        const auto current = g_core->GetConfig();
        if (current.enabled == config.enabled && current.targetScore == config.targetScore &&
            current.pctFodder == config.pctFodder && current.pctHeavy == config.pctHeavy &&
            current.pctSuperheavy == config.pctSuperheavy &&
            current.poweredDashDamage == config.poweredDashDamage &&
            current.parryPct == config.parryPct &&
            current.skipIntro == config.skipIntro && !g_settingsDirty) {
            return true;
        }

        if (current.enabled != config.enabled || current.targetScore != config.targetScore ||
            current.pctFodder != config.pctFodder || current.pctHeavy != config.pctHeavy ||
            current.pctSuperheavy != config.pctSuperheavy ||
            current.poweredDashDamage != config.poweredDashDamage ||
            current.parryPct != config.parryPct ||
            current.skipIntro != config.skipIntro) {
            g_core->SetConfig(config);
        }
        const auto applied = g_core->GetConfig();
        const bool saved = PersistSettingsLocked(applied);
        if (saved) {
            wchar_t line[192]{};
            swprintf_s(line, L"Hellwalker: enabled=%u target=%d pctFodder=%u%% pctHeavy=%u%% pctSuperheavy=%u%% dashDamage=%d parryPct=%d%% skipIntro=%u",
                       applied.enabled ? 1U : 0U, applied.targetScore,
                       static_cast<unsigned>(applied.pctFodder),
                       static_cast<unsigned>(applied.pctHeavy),
                       static_cast<unsigned>(applied.pctSuperheavy),
                       applied.poweredDashDamage, applied.parryPct,
                       applied.skipIntro ? 1U : 0U);
            survival_shield::LogLine(line);
        }
        return saved;
    } catch (...) {
        return false;
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI SurvivalShield_SetOptions(
    BOOL enabled, int targetScore) noexcept {
    try {
        std::lock_guard lock(g_initializeMutex);
        if (!g_initialized || g_core == nullptr) {
            return FALSE;
        }

        auto config = g_core->GetConfig();
        config.enabled = enabled != FALSE;
        config.targetScore = targetScore;
        g_core->SetConfig(config);
        config = g_core->GetConfig();
        return PersistSettingsLocked(config) ? TRUE : FALSE;
    } catch (...) {
        return FALSE;
    }
}

// ---- Biblioteca de mods (fase 1: lector) ----
// UTF-8 JSON con el estado de la biblioteca completa. Llamar con
// utf8Buffer == nullptr para consultar el tamaño necesario (incluye el NUL).

extern "C" __declspec(dllexport) BOOL WINAPI SurvivalShield_GetModLibrary(
    char* utf8Buffer, unsigned int* size) noexcept {
    if (size == nullptr) {
        return FALSE;
    }
    try {
        std::lock_guard lock(g_initializeMutex);
        if (!g_initialized) {
            return FALSE;
        }
        const unsigned int needed = static_cast<unsigned int>(g_modLibraryJson.size()) + 1U;
        if (utf8Buffer == nullptr) {
            *size = needed;
            return TRUE;
        }
        if (*size < needed) {
            *size = needed;
            return FALSE;
        }
        std::memcpy(utf8Buffer, g_modLibraryJson.c_str(), needed);
        return TRUE;
    } catch (...) {
        return FALSE;
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI SurvivalShield_RescanModLibrary() noexcept {
    try {
        std::lock_guard lock(g_initializeMutex);
        if (!g_initialized) {
            return FALSE;
        }
        ScanModLibraryLocked();
        return TRUE;
    } catch (...) {
        return FALSE;
    }
}
