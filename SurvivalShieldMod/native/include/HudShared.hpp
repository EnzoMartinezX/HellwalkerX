#pragma once

#include <cstddef>
#include <cstdint>

namespace survival_shield {

// Name of the file mapping shared by the injected DLL (writer) and the
// loader's HUD overlay (reader). Local\ namespace: same session, no
// cross-user exposure.
constexpr wchar_t kHudSharedName[] = L"Local\\HellwalkerHudState";
constexpr std::uint32_t kHudSharedMagic = 0x48573131; // "HW11"
constexpr std::uint32_t kHudSharedVersion = 2;

// Single-writer snapshot. The DLL republishes every ~150 ms with a fresh
// heartbeat; the overlay treats a stale heartbeat (> 2.5 s) as "game closed
// or DLL not injected" and hides itself.
//
// v2 adds killSeq: the DLL increments it once per credited demon kill (even
// while the score is frozen). The overlay flashes the counter on each change
// and fades it out, so it never sits on screen during loads or cutscenes.
// v1 publishers (40-byte section, no killSeq) are still accepted; the overlay
// then falls back to score-delta detection, which cannot fire while frozen.
struct HudSharedState {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t enabled;      // 0/1 mod toggle
    std::uint32_t supercharged; // 0/1 charge ready
    std::uint64_t scorePoints;  // integer points toward the threshold
    std::int32_t targetScore;
    float scoreFraction;        // clamped [0..1] progress toward the target
    std::uint64_t heartbeatMs;  // GetTickCount64() at publish time
    std::uint64_t killSeq;      // v2: monotonically increasing kill counter
};

static_assert(sizeof(HudSharedState) == 48, "unexpected padding in HudSharedState");
// Size of the v1 layout, for readers that must accept an old publisher.
constexpr std::size_t kHudSharedV1Size = 40;

} // namespace survival_shield
