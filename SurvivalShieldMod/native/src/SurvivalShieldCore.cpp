#include "SurvivalShieldCore.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace survival_shield {
namespace {

constexpr float kLengthEpsilon = 1.0e-5F;

Vec3 Subtract(const Vec3& a, const Vec3& b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 Negate(const Vec3& value) noexcept {
    return {-value.x, -value.y, -value.z};
}

float Dot(const Vec3& a, const Vec3& b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

float LengthSquared(const Vec3& value) noexcept {
    return Dot(value, value);
}

bool Normalize(Vec3& value) noexcept {
    const float lengthSquared = LengthSquared(value);
    if (!std::isfinite(lengthSquared) || lengthSquared <= kLengthEpsilon * kLengthEpsilon) {
        return false;
    }

    const float inverseLength = 1.0F / std::sqrt(lengthSquared);
    value.x *= inverseLength;
    value.y *= inverseLength;
    value.z *= inverseLength;
    return true;
}

class ActiveChainGuard {
public:
    ActiveChainGuard(std::mutex& mutex,
                     std::unordered_set<std::uint64_t>& activeChains,
                     std::uint64_t chainId)
        : mutex_(mutex), activeChains_(activeChains), chainId_(chainId) {}

    ~ActiveChainGuard() {
        std::lock_guard lock(mutex_);
        activeChains_.erase(chainId_);
    }

    ActiveChainGuard(const ActiveChainGuard&) = delete;
    ActiveChainGuard& operator=(const ActiveChainGuard&) = delete;

private:
    std::mutex& mutex_;
    std::unordered_set<std::uint64_t>& activeChains_;
    std::uint64_t chainId_;
};

} // namespace

std::size_t EntityKeyHash::operator()(const EntityKey& key) const noexcept {
    const std::uint64_t packed = (static_cast<std::uint64_t>(key.entityId) << 32U) |
                                 static_cast<std::uint64_t>(key.spawnGeneration);
    return std::hash<std::uint64_t>{}(packed);
}

SurvivalShieldCore::SurvivalShieldCore(ModConfig config)
    : config_(config) {
    config_.targetScore = NormalizeTargetScore(config_.targetScore);
    config_.poweredDashDamage = NormalizePoweredDashDamage(config_.poweredDashDamage);
    config_.pctFodder = std::clamp(config_.pctFodder, 0, 100);
    config_.pctHeavy = std::clamp(config_.pctHeavy, 0, 100);
    config_.pctSuperheavy = std::clamp(config_.pctSuperheavy, 0, 100);
    config_.parryPct = NormalizeParryPct(config_.parryPct);
}

void SurvivalShieldCore::SetConfig(ModConfig config) {
    std::lock_guard lock(mutex_);
    config.targetScore = NormalizeTargetScore(config.targetScore);
    config.poweredDashDamage = NormalizePoweredDashDamage(config.poweredDashDamage);
    config.pctFodder = std::clamp(config.pctFodder, 0, 100);
    config.pctHeavy = std::clamp(config.pctHeavy, 0, 100);
    config.pctSuperheavy = std::clamp(config.pctSuperheavy, 0, 100);
    config.parryPct = NormalizeParryPct(config.parryPct);
    config_ = config;
}

ModConfig SurvivalShieldCore::GetConfig() const {
    std::lock_guard lock(mutex_);
    return config_;
}

bool SurvivalShieldCore::RecordDemonKill(const DemonKillEvent& event) {
    if (!event.playerCredited || !event.isDemon || event.category == DemonCategory::Other) {
        return false;
    }

    std::lock_guard lock(mutex_);
    if (!config_.enabled || scoredVictims_.contains(event.victim)) {
        return false;
    }

    int basePoints = 0;
    int percentage = 0;
    switch (event.category) {
        case DemonCategory::Fodder:
            basePoints = 100;
            percentage = config_.pctFodder;
            break;
        case DemonCategory::Heavy:
            basePoints = 300;
            percentage = config_.pctHeavy;
            break;
        case DemonCategory::Superheavy:
            basePoints = 1000;
            percentage = config_.pctSuperheavy;
            break;
        case DemonCategory::Other:
            return false;
    }

    scoredVictims_.insert(event.victim);
    const auto award = static_cast<std::uint64_t>(basePoints) *
                       static_cast<std::uint64_t>(percentage);
    if (scoreCentipoints_ > std::numeric_limits<std::uint64_t>::max() - award) {
        scoreCentipoints_ = std::numeric_limits<std::uint64_t>::max();
    } else {
        scoreCentipoints_ += award;
    }
    return true;
}

void SurvivalShieldCore::ResetSession() {
    std::lock_guard lock(mutex_);
    scoreCentipoints_ = 0;
    scoredVictims_.clear();
    activeChains_.clear();
}

std::uint64_t SurvivalShieldCore::ScoreCentipoints() const {
    std::lock_guard lock(mutex_);
    return scoreCentipoints_;
}

std::uint64_t SurvivalShieldCore::ScorePoints() const {
    std::lock_guard lock(mutex_);
    return scoreCentipoints_ / 100U;
}

bool SurvivalShieldCore::IsSupercharged() const {
    std::lock_guard lock(mutex_);
    const auto threshold = static_cast<std::uint64_t>(config_.targetScore) * 100U;
    return config_.enabled && scoreCentipoints_ >= threshold;
}

ScoreSnapshot SurvivalShieldCore::GetScoreSnapshot() const {
    std::lock_guard lock(mutex_);
    const auto threshold = static_cast<std::uint64_t>(config_.targetScore) * 100U;
    return ScoreSnapshot{
        scoreCentipoints_ / 100U,
        config_.targetScore,
        config_.enabled && scoreCentipoints_ >= threshold};
}

bool SurvivalShieldCore::OnShieldDashImpact(ICombatWorld& world,
                                             EntityKey primary,
                                             EntityKey player,
                                             const Vec3& impactPoint,
                                             const Vec3& impactNormalTowardPlayer,
                                             std::uint64_t chainId,
                                             float radius,
                                             float halfAngleRadians) {
    if (chainId == 0 || !IsSupercharged() || !world.IsEnemy(primary) || !world.IsAlive(primary)) {
        return false;
    }

    if (!std::isfinite(radius) || radius <= 0.0F ||
        !std::isfinite(halfAngleRadians) || halfAngleRadians <= 0.0F ||
        halfAngleRadians >= std::numbers::pi_v<float>) {
        return false;
    }

    {
        std::lock_guard lock(mutex_);
        if (!activeChains_.insert(chainId).second) {
            return false;
        }
    }
    ActiveChainGuard chainGuard(mutex_, activeChains_, chainId);

    // Vanilla impact damage has already run. Burn the primary only if it survived it.
    world.ApplyOrRefreshBurn(primary, player, chainId);

    Vec3 backwardAxis = Negate(impactNormalTowardPlayer);
    if (!Normalize(backwardAxis)) {
        return true;
    }

    const Vec3 primaryCenter = world.Center(primary);
    const float radiusSquared = radius * radius;
    const float minimumDot = std::cos(halfAngleRadians);

    for (const EnemyCandidate& candidate : world.QueryEnemies(primaryCenter, radius)) {
        if (candidate.entity == primary || !world.IsEnemy(candidate.entity) || !world.IsAlive(candidate.entity)) {
            continue;
        }

        const Vec3 offset = Subtract(candidate.center, primaryCenter);
        const float distanceSquared = LengthSquared(offset);
        if (!std::isfinite(distanceSquared) || distanceSquared <= kLengthEpsilon * kLengthEpsilon ||
            distanceSquared > radiusSquared) {
            continue;
        }

        const float inverseDistance = 1.0F / std::sqrt(distanceSquared);
        const float alignment = Dot(offset, backwardAxis) * inverseDistance;
        if (alignment < minimumDot || !world.HasLineOfSight(impactPoint, candidate.center)) {
            continue;
        }

        world.ApplyOrRefreshBurn(candidate.entity, player, chainId);
    }

    return true;
}

int SurvivalShieldCore::NormalizeTargetScore(int value) noexcept {
    constexpr int minimum = 100;
    constexpr int maximum = 10000;
    constexpr int step = 100;
    const int bounded = std::clamp(value, minimum, maximum);
    return ((bounded + step / 2) / step) * step;
}

int SurvivalShieldCore::NormalizePoweredDashDamage(int value) noexcept {
    constexpr int minimum = 100;
    constexpr int maximum = 5000;
    constexpr int step = 50;
    const int bounded = std::clamp(value, minimum, maximum);
    return ((bounded + step / 2) / step) * step;
}

int SurvivalShieldCore::NormalizeParryPct(int value) noexcept {
    constexpr int minimum = 0;
    constexpr int maximum = 50;
    constexpr int step = 10;
    const int bounded = std::clamp(value, minimum, maximum);
    return ((bounded + step / 2) / step) * step;
}

double SurvivalShieldCore::ParryPointsFor(int targetScore, int parryPct) noexcept {
    const int pct = NormalizeParryPct(parryPct);
    if (pct <= 0 || targetScore <= 0) {
        return 0.0;
    }
    return static_cast<double>(NormalizeTargetScore(targetScore)) *
           static_cast<double>(pct) / 100.0;
}

float SurvivalShieldCore::SuperchargedBashDamageFor(int configuredDamage,
                                                    float maxHealth,
                                                    DemonCategory category) noexcept {
    const int cfg = std::max(100, NormalizePoweredDashDamage(configuredDamage));
    const float floorDmg = static_cast<float>(cfg);
    if (!std::isfinite(maxHealth) || maxHealth <= 1.0F) {
        return floorDmg;
    }

    const float sliderFactor = std::clamp(floorDmg / 2000.0F, 0.25F, 2.50F);
    float pct = 0.0F;
    if (maxHealth > 45000.0F) {
        pct = std::clamp(0.12F * sliderFactor, 0.06F, 0.35F);
    } else if (category == DemonCategory::Fodder || maxHealth <= 2500.0F) {
        pct = std::clamp(1.00F * sliderFactor, 0.25F, 1.25F);
    } else if (category == DemonCategory::Heavy ||
               (category == DemonCategory::Other && maxHealth <= 20000.0F)) {
        pct = std::clamp(0.35F * sliderFactor, 0.15F, 0.75F);
    } else {
        pct = std::clamp(0.22F * sliderFactor, 0.10F, 0.55F);
    }
    return std::max(floorDmg, std::round(maxHealth * pct));
}

} // namespace survival_shield
