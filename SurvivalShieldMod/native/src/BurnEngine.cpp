#include "BurnEngine.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cmath>

namespace survival_shield {

bool BurnEngine::ReadHealth(const void* entity, float& out) noexcept {
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(GetCurrentProcess(),
                             static_cast<const char*>(entity) + kHealthFieldOffset,
                             &out, sizeof(out), &bytesRead) != FALSE &&
           bytesRead == sizeof(out);
}

bool BurnEngine::WriteHealth(const void* entity, float value) noexcept {
    SIZE_T written = 0;
    return WriteProcessMemory(GetCurrentProcess(),
                              static_cast<char*>(const_cast<void*>(entity)) + kHealthFieldOffset,
                              &value, sizeof(value), &written) != FALSE &&
           written == sizeof(value);
}

std::int64_t BurnEngine::NowMs() noexcept {
    return static_cast<std::int64_t>(GetTickCount64());
}

void BurnEngine::StartOrRefresh(const void* entity, float baseHealthHint) {
    if (entity == nullptr) {
        return;
    }

    // Clamp absurd hints (unmapped reads give garbage); a sane demon pool of
    // health fits well below this bound.
    if (!std::isfinite(baseHealthHint) || baseHealthHint <= 0.0F || baseHealthHint > 1.0e6F) {
        baseHealthHint = 0.0F;
    }

    std::lock_guard lock(mutex_);
    for (Burn& burn : burns_) {
        if (burn.entity == entity) {
            burn.baseMaxHealth = std::max(burn.baseMaxHealth, baseHealthHint);
            burn.ticksLeft = kMaxTicks; // refreshable: impact restarts the window
            burn.nextTickAtMs = NowMs() + kTickIntervalMs;
            return;
        }
    }

    if (burns_.size() >= kMaxConcurrent) {
        return;
    }
    burns_.push_back(Burn{entity, std::max(baseHealthHint, 1.0F),
                          NowMs() + kTickIntervalMs, kMaxTicks});
}

void BurnEngine::StopAll() {
    std::lock_guard lock(mutex_);
    burns_.clear();
}

std::size_t BurnEngine::ActiveCount() const {
    std::lock_guard lock(mutex_);
    return burns_.size();
}

int BurnEngine::Tick() {
    const std::int64_t now = NowMs();
    int applied = 0;

    // Held across the (short) process-memory writes so a StartOrRefresh from
    // a game thread cannot be clobbered by a stale list swap.
    std::lock_guard lock(mutex_);
    for (Burn& burn : burns_) {
        if (burn.ticksLeft <= 0 || now < burn.nextTickAtMs) {
            continue;
        }

        float health = 0.0F;
        if (!ReadHealth(burn.entity, health)) {
            burn.ticksLeft = 0; // entity memory gone (freed/recycled): stop
            continue;
        }
        // Health jumped far above anything this body ever had: the pointer
        // was recycled onto a fresh spawn. Stop instead of burning a stranger.
        if (health > burn.baseMaxHealth * 1.5F + 100.0F) {
            burn.ticksLeft = 0;
            continue;
        }
        if (health <= 1.0F) {
            burn.ticksLeft = 0; // floor reached; the fire never kills
            continue;
        }

        burn.baseMaxHealth = std::max(burn.baseMaxHealth, health);
        const float damage = static_cast<float>(burn.baseMaxHealth * kTickDamagePct);
        const float next = std::max(health - damage, 1.0F);
        if (WriteHealth(burn.entity, next)) {
            ++applied;
            --burn.ticksLeft;
        } else {
            burn.ticksLeft = 0;
        }
        burn.nextTickAtMs = now + kTickIntervalMs;
    }
    burns_.erase(std::remove_if(burns_.begin(), burns_.end(),
                                [](const Burn& burn) { return burn.ticksLeft <= 0; }),
                 burns_.end());
    return applied;
}

} // namespace survival_shield
