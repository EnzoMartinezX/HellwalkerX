#pragma once

namespace survival_shield {

// Values exchanged with the in-game settings UI through the Hellwalker bridge.
// The UI works in UI units (floats); these are the stored game-side integers.
struct BridgeValues {
    bool enabled = true;
    int targetScore = 300;       // 100..10000, step 100
    int pctFodder = 10;          // 0..100
    int pctHeavy = 10;           // 0..100
    int pctSuperheavy = 10;      // 0..100
    int poweredDashDamage = 2000; // 100..5000, step 50
    int parryPct = 10;           // 0..50, step 10; % of targetScore per parry; 0 disables
    // Renames the intro-skip mod folder before the next launch (handled by
    // PlaySurvivalShield.bat); the game itself cannot toggle resource mods.
    bool skipIntro = true;
};

// false when the native core is not initialized (hooks must keep passthrough).
bool BridgeGetValues(BridgeValues* out);
// Clamps, applies to the live core config and persists to settings.json.
bool BridgeSetValues(const BridgeValues& values);

} // namespace survival_shield
