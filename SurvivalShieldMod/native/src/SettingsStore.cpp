#include "SettingsStore.hpp"
#include "JsonUtil.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace survival_shield {
namespace {

void LoadBool(std::string_view json, std::string_view key, bool& out) {
    const auto value = FindJsonValue(json, key);
    if (!value) {
        return;
    }
    if (*value == "true") {
        out = true;
    } else if (*value == "false") {
        out = false;
    }
}

void LoadInt(std::string_view json, std::string_view key, int& out) {
    const auto value = FindJsonValue(json, key);
    if (!value) {
        return;
    }

    int parsed = out;
    const auto result = std::from_chars(value->data(), value->data() + value->size(), parsed);
    if (result.ec == std::errc{} && result.ptr == value->data() + value->size()) {
        out = parsed;
    }
}

int NormalizeTargetScore(int value) noexcept {
    constexpr int minimum = 100;
    constexpr int maximum = 10000;
    constexpr int step = 100;
    const int bounded = std::clamp(value, minimum, maximum);
    return ((bounded + step / 2) / step) * step;
}

int NormalizePoweredDashDamage(int value) noexcept {
    constexpr int minimum = 100;
    constexpr int maximum = 5000;
    constexpr int step = 50;
    const int bounded = std::clamp(value, minimum, maximum);
    return ((bounded + step / 2) / step) * step;
}

int NormalizeParryPct(int value) noexcept {
    constexpr int minimum = 0;
    constexpr int maximum = 50;
    constexpr int step = 10;
    const int bounded = std::clamp(value, minimum, maximum);
    return ((bounded + step / 2) / step) * step;
}

std::filesystem::path GetLocalAppData() {
    const DWORD needed = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (needed == 0) {
        return {};
    }

    std::vector<wchar_t> buffer(needed);
    const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), needed);
    if (written == 0 || written >= needed) {
        return {};
    }
    return std::filesystem::path(std::wstring(buffer.data(), written));
}

} // namespace

std::filesystem::path SettingsStore::StorageDirectory() {
    const auto localAppData = GetLocalAppData();
    if (localAppData.empty()) {
        return {};
    }
    return localAppData / L"DOOMTheDarkAges" / L"SurvivalShield";
}

std::filesystem::path SettingsStore::FilePath() const {
    const auto directory = StorageDirectory();
    return directory.empty() ? std::filesystem::path{} : directory / L"settings.json";
}

ModConfig SettingsStore::LoadOrDefault() const {
    ModConfig config{};
    const auto path = FilePath();
    if (path.empty()) {
        return config;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return config;
    }

    const std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    LoadBool(json, "bEnableSurvivalShieldMod", config.enabled);
    LoadInt(json, "iTargetSurvivalScore", config.targetScore);
    LoadInt(json, "iHellwalkerPoweredDashDamage", config.poweredDashDamage);
    LoadInt(json, "iHellwalkerPctFodder", config.pctFodder);
    LoadInt(json, "iHellwalkerPctHeavy", config.pctHeavy);
    LoadInt(json, "iHellwalkerPctSuperheavy", config.pctSuperheavy);
    LoadInt(json, "iHellwalkerParryPct", config.parryPct);
    LoadBool(json, "bHellwalkerSkipIntro", config.skipIntro);
    config.targetScore = NormalizeTargetScore(config.targetScore);
    config.poweredDashDamage = NormalizePoweredDashDamage(config.poweredDashDamage);
    config.pctFodder = std::clamp(config.pctFodder, 0, 100);
    config.pctHeavy = std::clamp(config.pctHeavy, 0, 100);
    config.pctSuperheavy = std::clamp(config.pctSuperheavy, 0, 100);
    config.parryPct = NormalizeParryPct(config.parryPct);
    return config;
}

bool SettingsStore::Save(const ModConfig& config) const {
    const auto path = FilePath();
    if (path.empty()) {
        return false;
    }

    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }

    const auto tempPath = path.wstring() + L".tmp";
    {
        std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
        if (!file) {
            return false;
        }
        file << "{\n"
             << "  \"bEnableSurvivalShieldMod\": " << (config.enabled ? "true" : "false") << ",\n"
             << "  \"iTargetSurvivalScore\": " << NormalizeTargetScore(config.targetScore) << ",\n"
             << "  \"iHellwalkerPoweredDashDamage\": "
             << NormalizePoweredDashDamage(config.poweredDashDamage) << ",\n"
             << "  \"iHellwalkerPctFodder\": " << std::clamp(config.pctFodder, 0, 100) << ",\n"
             << "  \"iHellwalkerPctHeavy\": " << std::clamp(config.pctHeavy, 0, 100) << ",\n"
             << "  \"iHellwalkerPctSuperheavy\": " << std::clamp(config.pctSuperheavy, 0, 100) << ",\n"
             << "  \"iHellwalkerParryPct\": " << NormalizeParryPct(config.parryPct) << ",\n"
             << "  \"bHellwalkerSkipIntro\": " << (config.skipIntro ? "true" : "false") << "\n"
             << "}\n";
        file.flush();
        if (!file) {
            return false;
        }
    }

    return MoveFileExW(tempPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

} // namespace survival_shield
