#pragma once

namespace survival_shield {

// Installs the gameplay hooks (AddScore kill scoring and shield-dash damage
// boost) through the v7 disk thunks. Observe-only by default; the damage
// write is gated by gameplaystage.txt (0 observe, 1 boost).
bool InstallGameplayBridge();

// Live score held by the gameplay bridge (kill events feed it directly, not
// the SurvivalShieldCore instance). Returns false when the bridge is absent
// and no live score exists.
bool GetLiveScore(unsigned long long* scorePoints, int* targetScore, bool* supercharged);

} // namespace survival_shield
