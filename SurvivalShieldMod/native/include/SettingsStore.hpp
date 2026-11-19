#pragma once

#include "SurvivalShieldCore.hpp"

#include <filesystem>

namespace survival_shield {

class SettingsStore {
public:
    // %LOCALAPPDATA%\DOOMTheDarkAges\SurvivalShield; vacío si LOCALAPPDATA
    // no está disponible. Compartido con la biblioteca de mods.
    static std::filesystem::path StorageDirectory();
    std::filesystem::path FilePath() const;
    ModConfig LoadOrDefault() const;
    bool Save(const ModConfig& config) const;
};

} // namespace survival_shield
