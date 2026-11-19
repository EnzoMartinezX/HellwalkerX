#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace survival_shield {

// Runtime damage-over-time applied by writing the idAI's health float
// directly (embedded idAIHealth at aiEntity + 0x2FE0: maxHealth float at
// +0x1C -> aiEntity + 0x2FFC, currentHealth float at +0x3C -> aiEntity + 0x301C,
// verified from Update 5 idAIHealth::GetMaxHealth/GetHealth).
// Hellwalker fire: 3% of the resolved max health per 0.5 s tick,
// 8 ticks (4 s) for 24% total, refreshable on re-impact, never kills
// (health floors at 1 HP, matching the vanilla DOT's noDeath behaviour).
//
// The engine never owns a thread: StartOrRefresh() is called from game
// threads inside the damage detour and Tick() from the runtime publisher
// thread at >= 10 Hz; a small mutex keeps the burn list coherent.
class BurnEngine {
public:
    static constexpr double kTickDamagePct = 0.03;
    static constexpr int kMaxTicks = 8;
    static constexpr std::int64_t kTickIntervalMs = 500;
    static constexpr std::size_t kMaxConcurrent = 16;
    // Embedded idAIHealth (+0x2FE0) floats inside idAI (Update 5 build).
    static constexpr std::size_t kMaxHealthFieldOffset = 0x2FFC;
    static constexpr std::size_t kHealthFieldOffset = 0x301C;

    // baseHealthHint: best known approximation of the victim's MAX health
    // (pre-impact health when available). The engine keeps the largest
    // value ever observed per burn so later ticks stay calibrated.
    void StartOrRefresh(const void* entity, float baseHealthHint);

    void StopAll();
    std::size_t ActiveCount() const;

    // Executes due ticks; returns the number of damage ticks applied.
    int Tick();

private:
    struct Burn {
        const void* entity;
        float baseMaxHealth;
        std::int64_t nextTickAtMs;
        int ticksLeft;
    };

    static bool ReadHealth(const void* entity, float& out) noexcept;
    static bool WriteHealth(const void* entity, float value) noexcept;
    static std::int64_t NowMs() noexcept;

    mutable std::mutex mutex_;
    std::vector<Burn> burns_;
};

} // namespace survival_shield
