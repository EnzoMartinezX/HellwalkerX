#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace survival_shield {

struct ModConfig {
    bool enabled = true;
    int targetScore = 300;
    // Final damage scale for a fully charged mod dash (2000 = 1.0x default scale).
    int poweredDashDamage = 2000;
    // Percentage of each demon category's base score (fodder 100, heavy 300,
    // superheavy 1000 points) that counts toward the threshold.
    int pctFodder = 10;
    int pctHeavy = 10;
    int pctSuperheavy = 10;
    // Percentage of the target dash score added per successful shield parry
    // (0..50 in steps of 10; 0 disables parry credit).
    int parryPct = 10;
    // Skips the startup logo/legal screens by (de)activating the
    // SkipIntro resource mod on the next launch.
    bool skipIntro = true;
};

struct EntityKey {
    std::uint32_t entityId = 0;
    std::uint32_t spawnGeneration = 0;

    friend bool operator==(const EntityKey&, const EntityKey&) = default;
};

struct EntityKeyHash {
    std::size_t operator()(const EntityKey& key) const noexcept;
};

struct Vec3 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

enum class DemonCategory : std::uint8_t {
    Other,
    Fodder,
    Heavy,
    Superheavy
};

struct DemonKillEvent {
    EntityKey victim;
    // Set only when the engine attributes this kill to the local player.
    bool playerCredited = false;
    bool isDemon = false;
    DemonCategory category = DemonCategory::Other;
};

struct ScoreSnapshot {
    std::uint64_t scorePoints = 0;
    int targetScore = 100;
    bool supercharged = false;
};

struct EnemyCandidate {
    EntityKey entity;
    Vec3 center;
};

class ICombatWorld {
public:
    virtual ~ICombatWorld() = default;

    virtual bool IsEnemy(EntityKey entity) const = 0;
    virtual bool IsAlive(EntityKey entity) const = 0;
    virtual Vec3 Center(EntityKey entity) const = 0;
    virtual std::vector<EnemyCandidate> QueryEnemies(const Vec3& origin, float radius) const = 0;
    virtual bool HasLineOfSight(const Vec3& start, const Vec3& end) const = 0;
    virtual void ApplyOrRefreshBurn(EntityKey target, EntityKey player, std::uint64_t chainId) = 0;
};

class SurvivalShieldCore {
public:
    explicit SurvivalShieldCore(ModConfig config = {});

    void SetConfig(ModConfig config);
    ModConfig GetConfig() const;

    // Adds the category's base score, scaled by its configured percentage.
    // Score is stored in centipoints so percentages retain fractional points.
    bool RecordDemonKill(const DemonKillEvent& event);
    void ResetSession();
    std::uint64_t ScoreCentipoints() const;
    std::uint64_t ScorePoints() const;
    bool IsSupercharged() const;
    ScoreSnapshot GetScoreSnapshot() const;

    // Call after the adapter resolves the impact damage so survival is checked
    // against the actual result. The adapter owns all game calls.
    bool OnShieldDashImpact(ICombatWorld& world,
                            EntityKey primary,
                            EntityKey player,
                            const Vec3& impactPoint,
                            const Vec3& impactNormalTowardPlayer,
                            std::uint64_t chainId,
                            float radius = 6.0F,
                            float halfAngleRadians = 0.78539816339F);

    static int NormalizeTargetScore(int value) noexcept;
    static int NormalizePoweredDashDamage(int value) noexcept;
    static int NormalizeParryPct(int value) noexcept;
    static double ParryPointsFor(int targetScore, int parryPct) noexcept;
    static float SuperchargedBashDamageFor(int configuredDamage,
                                           float maxHealth,
                                           DemonCategory category) noexcept;

private:

    mutable std::mutex mutex_;
    ModConfig config_;
    std::uint64_t scoreCentipoints_ = 0;
    std::unordered_set<EntityKey, EntityKeyHash> scoredVictims_;
    std::unordered_set<std::uint64_t> activeChains_;
};

} // namespace survival_shield
