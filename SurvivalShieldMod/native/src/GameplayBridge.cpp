#include "GameplayBridge.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <intrin.h>

#include "Log.hpp"
#include "BridgeConfig.hpp"
#include "BurnEngine.hpp"
#include "HudShared.hpp"
#include "SurvivalShieldCore.hpp"
#include "UiBridge.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace survival_shield {
namespace {

// Installed by native/tools/patch_u5.py (Update 5, 2026-11-12).
constexpr std::uint32_t kEntryAddScoreRva = 0x019C4B90;
constexpr std::uint32_t kTrampAddScoreRva = 0x004C7470;
constexpr std::uint32_t kTrampDamageRva = 0x00001CEF;
constexpr std::uint32_t kSlotAddScoreRva = 0x0B613020;
constexpr std::uint32_t kSlotDamageRva = 0x0B613028;
constexpr std::uint32_t kTrampParryRva = 0x0051ECF0;
constexpr std::uint32_t kSlotParryRva = 0x0B613030;
constexpr std::uint32_t kTrampKilledRva = 0x0071FC70;
constexpr std::uint32_t kSlotKilledRva = 0x0B613038;
constexpr std::uint32_t kTrampHudRva = 0x007CFF80;
constexpr std::uint32_t kSlotHudRva = 0x0B613040;
constexpr std::uint32_t kTrampPacketRva = 0x02CD7487;
constexpr std::uint32_t kSlotPacketRva = 0x0B613048;

// Cast helper (CastToIdPlayer) and idAI / idAI::Killed / VFX offsets (Update 5).
constexpr std::uint32_t kCastToIdPlayerRva = 0x02965770; // Update 5 CastToIdPlayer (idPlayer::Type @ 0x0B261970)
constexpr std::size_t kAiMonsterDataPtrOffset = 0x2A40;
constexpr std::size_t kAiMonsterTypeOffset = 0x2A8;
constexpr std::size_t kDeathInfoAttackerPtrOffset = 0x30C8;
constexpr std::size_t kAiIsDeadVtableOffset = 0xED0; // idAI::IsDead() (verified @ 0x00EEFCA9 / 0x00F28410)

// Native LogicFX status-effect burn + idEntity::StartFX helpers (Update 5).
constexpr std::uint32_t kLogicFxCtxCtorRva = 0x01750710;
constexpr std::uint32_t kLogicFxActivateRva = 0x029ABFF0;
constexpr std::uint32_t kLogicFxDeactivateRva = 0x029AC0C0;
constexpr std::uint32_t kHeapManagerPtrRva = 0x07AB4208;
constexpr std::int32_t kLogicFxTypeBurn = 1;
constexpr std::int32_t kLogicFxBurnTierFull = 2;
constexpr std::uint32_t kFxConditionParmsCtorRva = 0x02C65A70;
constexpr std::size_t kEntityGetOriginVtableOffset = 0x288;
constexpr std::size_t kEntityGetAxisVtableOffset = 0x820;
constexpr std::size_t kEntityStartFxVtableOffset = 0xA70;
constexpr std::int32_t kFxConditionManOnFire = 719;     // FX_MAN_ON_FIRE @ 0x3A6FC40
constexpr std::int32_t kFxConditionManOnFireStop = 722; // FX_MAN_ON_FIRE_STOP @ 0x3A6FC70

// aiMonsterType_t bitmasks (verified in Update 5 rdata enum table @ 0x03A844F0).
constexpr std::uint64_t kMonsterMaskFodder = 0x00000000000000FFULL;
constexpr std::uint64_t kMonsterMaskHeavy = 0x00000000001FFF00ULL;
constexpr std::uint64_t kMonsterMaskSuperheavy =
    0x000000003FE00000ULL | 0x0000002000000000ULL |
    0x0000008000000000ULL | 0x0FE0000000000000ULL;

// Read-only mode detection: idMapInstanceLocal* global + mapState / gameType_t fields.
constexpr std::uint32_t kMapInstancePtrRva = 0x0A564640;
constexpr std::size_t kMapStateOffset = 0x14E8;
constexpr std::size_t kMapGameTypeOffset = 0x14EC;

// Update 5 damage layout: idDeclDamage* lives at +0x350 (damageParms + 0x2F0),
// damage amount float at +0x358.
constexpr std::size_t kDamageParmsDeclOffset = 0x2F0;
constexpr std::size_t kDamagePacketDeclOffset = 0x350;

constexpr int kGameTypeNone = 0;
constexpr int kGameTypeCampaign = 1;
constexpr int kGameTypeBattleArena = 2;
constexpr int kGameTypeBattleArenaTutorial = 3;
constexpr int kGameTypeHorde = 4; // Slaughter Mode
constexpr int kGameTypeRipatorium = 5;
constexpr int kGameTypeDlc1 = 6;
constexpr int kGameTypeRipatoriumDlc = 7;
constexpr int kGameTypeDlc2 = 8;

// Vanilla base scores per category (decls/scoringitem_demon/*.decl).
constexpr double kBaseFodder = 100.0;
constexpr double kBaseHeavy = 300.0;
constexpr double kBaseSuperheavy = 1000.0;

// gameplaystage.txt: 0 = observe only (default, logs everything, writes
// nothing), 1 = supercharged bash applies the configured damage.
std::atomic<int> g_gameplayStage{0};

std::mutex g_scoreMutex;
double g_scorePoints = 0.0;
bool g_superchargedLatch = false;
std::atomic<unsigned> g_loggedKills{0};
std::atomic<unsigned> g_loggedDamages{0};
std::atomic<bool> g_freezeLogged{false};
// Kill/parry flash counter for the HUD overlay: bumped once per credited event.
std::atomic<std::uint64_t> g_hudKillSeq{0};

BurnEngine g_burn;

std::uintptr_t g_trampolineAddScore = 0;
std::uintptr_t g_trampolineDamage = 0;
std::uintptr_t g_trampolineParry = 0;
std::atomic<unsigned> g_rawParry{0};
std::atomic<unsigned> g_rawAiKill{0};
std::uintptr_t g_trampolineHud = 0;
std::uintptr_t g_trampolineKilled = 0;
std::uintptr_t g_trampolinePacket = 0;
std::atomic<int> g_lastLoggedGameType{-1};
std::atomic<int> g_lastLoggedMapState{-1};
std::atomic<bool> g_storyModeActive{false};

struct ActiveBurnVfx {
    void* entity;
    float expectedMaxHealth;
    ULONGLONG expiresAtMs;
    bool logicActive;
    bool fxActive;
};

constexpr std::size_t kMaxActiveBurnVfx = 16U;
constexpr ULONGLONG kBurnVfxDurationMs = 4000ULL;

std::mutex g_burnVfxMutex;
std::vector<ActiveBurnVfx> g_activeBurnVfx;
std::atomic<std::size_t> g_activeBurnVfxCount{0};

bool SafeRead(const void* address, void* buffer, std::size_t bytes) noexcept {
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, buffer, bytes, &bytesRead) != FALSE &&
           bytesRead == bytes;
}

int ReadMapInstanceInt32(std::size_t offset, int fallback) noexcept {
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (moduleBase == 0) {
        return fallback;
    }
    const void* mapInstance = nullptr;
    if (!SafeRead(reinterpret_cast<const void*>(moduleBase + kMapInstancePtrRva),
                  &mapInstance, sizeof(mapInstance)) ||
        mapInstance == nullptr) {
        return fallback;
    }
    std::int32_t value = fallback;
    if (!SafeRead(static_cast<const char*>(mapInstance) + offset, &value, sizeof(value))) {
        return fallback;
    }
    return value;
}

int ReadCurrentGameType() noexcept {
    return ReadMapInstanceInt32(kMapGameTypeOffset, kGameTypeNone);
}

int ReadCurrentMapState() noexcept {
    return ReadMapInstanceInt32(kMapStateOffset, 0);
}

const wchar_t* GameTypeName(int gt) noexcept {
    switch (gt) {
        case kGameTypeNone: return L"GAME_TYPE_NONE";
        case kGameTypeCampaign: return L"GAME_TYPE_CAMPAIGN";
        case kGameTypeBattleArena: return L"GAME_TYPE_BATTLEARENA";
        case kGameTypeBattleArenaTutorial: return L"GAME_TYPE_BATTLEARENA_TUTORIAL";
        case kGameTypeHorde: return L"GAME_TYPE_HORDE (Slaughter)";
        case kGameTypeRipatorium: return L"GAME_TYPE_RIPATORIUM";
        case kGameTypeDlc1: return L"GAME_TYPE_DLC1";
        case kGameTypeRipatoriumDlc: return L"GAME_TYPE_RIPATORIUM_DLC";
        case kGameTypeDlc2: return L"GAME_TYPE_DLC2";
        default: return L"GAME_TYPE_UNKNOWN";
    }
}

bool IsStoryModeGameType(int gt) noexcept {
    return gt == kGameTypeCampaign || gt == kGameTypeDlc1 || gt == kGameTypeDlc2;
}

bool RefreshGameModeState() noexcept {
    const int gt = ReadCurrentGameType();
    const int ms = ReadCurrentMapState();
    const bool allowed = IsStoryModeGameType(gt) && (ms == 2);
    g_storyModeActive.store(allowed, std::memory_order_relaxed);
    const int prevGt = g_lastLoggedGameType.exchange(gt, std::memory_order_relaxed);
    const int prevMs = g_lastLoggedMapState.exchange(ms, std::memory_order_relaxed);
    if (gt != prevGt || ms != prevMs) {
        if (!allowed) {
            g_burn.StopAll();
            {
                std::lock_guard lock(g_burnVfxMutex);
                g_activeBurnVfx.clear();
                g_activeBurnVfxCount.store(0U, std::memory_order_relaxed);
            }
        }
        wchar_t line[192]{};
        swprintf_s(line, L"[mode] gameType=%d (%s) mapState=%d -> hellwalker=%s",
                   gt, GameTypeName(gt), ms,
                   allowed ? L"ACTIVE_STORY" : L"PAUSED_NON_STORY");
        LogLine(line);
    }
    return allowed;
}

using CastToIdPlayerFn = void*(__fastcall*)(void* entity);

bool IsLocalSlayer(void* entity) noexcept {
    if (entity == nullptr) {
        return false;
    }
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (moduleBase == 0) {
        return false;
    }
    return reinterpret_cast<CastToIdPlayerFn>(moduleBase + kCastToIdPlayerRva)(entity) != nullptr;
}

std::uint64_t ReadAiMonsterType(const void* aiEntity) noexcept {
    if (aiEntity == nullptr) {
        return 0;
    }
    const void* dataPtr = nullptr;
    if (!SafeRead(static_cast<const char*>(aiEntity) + kAiMonsterDataPtrOffset,
                  &dataPtr, sizeof(dataPtr)) ||
        dataPtr == nullptr) {
        return 0;
    }
    std::uint64_t mt = 0;
    if (!SafeRead(static_cast<const char*>(dataPtr) + kAiMonsterTypeOffset, &mt, sizeof(mt))) {
        return 0;
    }
    return mt;
}

DemonCategory ClassifyMonsterType(std::uint64_t monsterType) noexcept {
    if ((monsterType & kMonsterMaskSuperheavy) != 0ULL) {
        return DemonCategory::Superheavy;
    }
    if ((monsterType & kMonsterMaskHeavy) != 0ULL) {
        return DemonCategory::Heavy;
    }
    if ((monsterType & kMonsterMaskFodder) != 0ULL) {
        return DemonCategory::Fodder;
    }
    return DemonCategory::Other;
}

bool IsExeCodeAddress(std::uintptr_t moduleBase, std::uintptr_t addr) noexcept {
    return moduleBase != 0 && addr >= moduleBase && addr < moduleBase + 0x10000000ULL;
}

// POD-only SEH helper (no C++ destructors, not noexcept under /EHsc).
bool TryCallVtableBool(void* obj, std::size_t vtableOffset, bool* outValue) {
    if (obj == nullptr || outValue == nullptr) {
        return false;
    }
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::uintptr_t vtable = 0;
    if (!SafeRead(obj, &vtable, sizeof(vtable)) || vtable == 0) {
        return false;
    }
    std::uintptr_t fnAddr = 0;
    if (!SafeRead(reinterpret_cast<const void*>(vtable + vtableOffset), &fnAddr, sizeof(fnAddr)) ||
        !IsExeCodeAddress(moduleBase, fnAddr)) {
        return false;
    }
    using BoolMethodFn = bool(__fastcall*)(void*);
    __try {
        *outValue = reinterpret_cast<BoolMethodFn>(fnAddr)(obj);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsAiDead(void* aiEntity, float* outHealth) {
    if (outHealth != nullptr) {
        *outHealth = -1.0F;
    }
    if (aiEntity == nullptr) {
        return false;
    }
    float hp = -1.0F;
    if (SafeRead(static_cast<const char*>(aiEntity) + BurnEngine::kHealthFieldOffset,
                 &hp, sizeof(hp))) {
        if (outHealth != nullptr) {
            *outHealth = hp;
        }
        if (std::isfinite(hp) && hp <= 0.0F) {
            return true;
        }
    }
    bool dead = false;
    if (TryCallVtableBool(aiEntity, kAiIsDeadVtableOffset, &dead) && dead) {
        return true;
    }
    return false;
}

// POD-only SEH helper: invokes native Update 5 LogicFX burn status effect
// activation/deactivation directly on an idAI entity (must run on game thread).
bool TryTriggerLogicBurnVfx(void* aiEntity, bool activate, std::int32_t burnTier) {
    if (aiEntity == nullptr) {
        return false;
    }
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (moduleBase == 0) {
        return false;
    }
    std::uintptr_t vtable = 0;
    if (!SafeRead(aiEntity, &vtable, sizeof(vtable)) || !IsExeCodeAddress(moduleBase, vtable)) {
        return false;
    }
    const void* worldSlot = nullptr;
    if (!SafeRead(static_cast<const char*>(aiEntity) + 0xA8, &worldSlot, sizeof(worldSlot)) ||
        worldSlot == nullptr) {
        return false;
    }
    const void* worldPtr = nullptr;
    if (!SafeRead(worldSlot, &worldPtr, sizeof(worldPtr)) || worldPtr == nullptr) {
        return false;
    }

    using LogicCtxCtorFn = void*(__fastcall*)(void* ctx, void* aiEntity);
    using LogicEventFn = void(__fastcall*)(void* ctx, const std::int32_t* effectType,
                                           const std::int32_t* tier);
    using HeapFreeFn = void(__fastcall*)(void* heapMgr, void* ptr, std::uint32_t alignTag);

    alignas(16) std::uint8_t ctx[0xA0]{};
    const std::int32_t effectType = kLogicFxTypeBurn;
    const std::int32_t tierValue = burnTier;
    const std::uintptr_t eventRva = activate ? kLogicFxActivateRva : kLogicFxDeactivateRva;
    __try {
        void* built = reinterpret_cast<LogicCtxCtorFn>(moduleBase + kLogicFxCtxCtorRva)(
            ctx, aiEntity);
        if (built == nullptr) {
            return false;
        }
        reinterpret_cast<LogicEventFn>(moduleBase + eventRva)(built, &effectType, &tierValue);
        const std::uint8_t allocFlags = ctx[0x43];
        if (allocFlags == 0U || allocFlags == 3U || allocFlags == 4U) {
            void* heapBuf = *reinterpret_cast<void**>(ctx + 0x30);
            if (heapBuf != nullptr) {
                void* heapMgr = *reinterpret_cast<void**>(moduleBase + kHeapManagerPtrRva);
                if (heapMgr != nullptr) {
                    void** heapVtable = *reinterpret_cast<void***>(heapMgr);
                    reinterpret_cast<HeapFreeFn>(heapVtable[0x40 / sizeof(void*)])(
                        heapMgr, heapBuf, 0x10U);
                }
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// POD-only SEH helper: triggers FX_MAN_ON_FIRE / FX_MAN_ON_FIRE_STOP via idEntity::StartFX.
bool TryTriggerEntityFxCondition(void* aiEntity, std::int32_t fxCondition) {
    if (aiEntity == nullptr) {
        return false;
    }
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::uintptr_t vtable = 0;
    if (!SafeRead(aiEntity, &vtable, sizeof(vtable)) || vtable == 0) {
        return false;
    }
    std::uintptr_t getOriginAddr = 0;
    std::uintptr_t getAxisAddr = 0;
    std::uintptr_t startFxAddr = 0;
    if (!SafeRead(reinterpret_cast<const void*>(vtable + kEntityGetOriginVtableOffset),
                  &getOriginAddr, sizeof(getOriginAddr)) ||
        !SafeRead(reinterpret_cast<const void*>(vtable + kEntityGetAxisVtableOffset),
                  &getAxisAddr, sizeof(getAxisAddr)) ||
        !SafeRead(reinterpret_cast<const void*>(vtable + kEntityStartFxVtableOffset),
                  &startFxAddr, sizeof(startFxAddr)) ||
        !IsExeCodeAddress(moduleBase, getOriginAddr) ||
        !IsExeCodeAddress(moduleBase, getAxisAddr) ||
        !IsExeCodeAddress(moduleBase, startFxAddr)) {
        return false;
    }

    using FxParmsCtorFn = void*(__fastcall*)(void* fxParms);
    using GetOriginFn = const float*(__fastcall*)(void* entity, float* outVec3);
    using GetAxisFn = const float*(__fastcall*)(void* entity, float* outMat3);
    using StartFxFn = void(__fastcall*)(void* entity, const void* fxParms);

    alignas(16) std::uint8_t fxParms[0x80]{};
    float originBuf[3]{};
    float axisBuf[9]{};
    __try {
        reinterpret_cast<FxParmsCtorFn>(moduleBase + kFxConditionParmsCtorRva)(fxParms);
        *reinterpret_cast<std::int32_t*>(fxParms + 0x18) = fxCondition;
        const float* originPtr = reinterpret_cast<GetOriginFn>(getOriginAddr)(aiEntity, originBuf);
        if (originPtr != nullptr) {
            std::memcpy(fxParms + 0x20, originPtr, 12U);
        }
        const float* axisPtr = reinterpret_cast<GetAxisFn>(getAxisAddr)(aiEntity, axisBuf);
        if (axisPtr != nullptr) {
            std::memcpy(fxParms + 0x38, axisPtr, 36U);
        }
        reinterpret_cast<StartFxFn>(startFxAddr)(aiEntity, fxParms);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const wchar_t* StartOrRefreshBurnVfx(void* aiEntity, float maxHealth) {
    if (aiEntity == nullptr) {
        return L"none";
    }
    const bool logicOk = TryTriggerLogicBurnVfx(aiEntity, true, kLogicFxBurnTierFull);
    const bool fxOk = TryTriggerEntityFxCondition(aiEntity, kFxConditionManOnFire);
    if (!logicOk && !fxOk) {
        return L"none";
    }

    const ULONGLONG expiresAt = GetTickCount64() + kBurnVfxDurationMs;
    {
        std::lock_guard lock(g_burnVfxMutex);
        bool updated = false;
        for (ActiveBurnVfx& entry : g_activeBurnVfx) {
            if (entry.entity == aiEntity) {
                entry.expectedMaxHealth = std::max(entry.expectedMaxHealth, maxHealth);
                entry.expiresAtMs = expiresAt;
                entry.logicActive = entry.logicActive || logicOk;
                entry.fxActive = entry.fxActive || fxOk;
                updated = true;
                break;
            }
        }
        if (!updated && g_activeBurnVfx.size() < kMaxActiveBurnVfx) {
            g_activeBurnVfx.push_back(ActiveBurnVfx{
                aiEntity, maxHealth, expiresAt, logicOk, fxOk});
        }
        g_activeBurnVfxCount.store(g_activeBurnVfx.size(), std::memory_order_relaxed);
    }

    if (logicOk && fxOk) {
        return L"logic+fx";
    }
    return logicOk ? L"logic" : L"fx";
}

void StopBurnVfxEntryOnGameThread(const ActiveBurnVfx& entry) {
    if (entry.entity == nullptr || !g_storyModeActive.load(std::memory_order_relaxed)) {
        return;
    }
    float curMax = 0.0F;
    if (!SafeRead(static_cast<const char*>(entry.entity) + BurnEngine::kMaxHealthFieldOffset,
                  &curMax, sizeof(curMax)) ||
        !std::isfinite(curMax) || std::fabs(curMax - entry.expectedMaxHealth) > 1.0F) {
        return;
    }
    if (entry.logicActive) {
        TryTriggerLogicBurnVfx(entry.entity, false, kLogicFxBurnTierFull);
    }
    if (entry.fxActive) {
        TryTriggerEntityFxCondition(entry.entity, kFxConditionManOnFireStop);
    }
}

void StopBurnVfxForEntity(void* aiEntity) {
    if (aiEntity == nullptr || g_activeBurnVfxCount.load(std::memory_order_relaxed) == 0U) {
        return;
    }
    ActiveBurnVfx stopped{};
    bool found = false;
    {
        std::lock_guard lock(g_burnVfxMutex);
        for (auto it = g_activeBurnVfx.begin(); it != g_activeBurnVfx.end(); ++it) {
            if (it->entity == aiEntity) {
                stopped = *it;
                g_activeBurnVfx.erase(it);
                g_activeBurnVfxCount.store(g_activeBurnVfx.size(), std::memory_order_relaxed);
                found = true;
                break;
            }
        }
    }
    if (found) {
        StopBurnVfxEntryOnGameThread(stopped);
    }
}

void DrainExpiredBurnVfx() {
    if (g_activeBurnVfxCount.load(std::memory_order_relaxed) == 0U ||
        !g_storyModeActive.load(std::memory_order_relaxed)) {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    std::vector<ActiveBurnVfx> expired;
    {
        std::lock_guard lock(g_burnVfxMutex);
        auto it = g_activeBurnVfx.begin();
        while (it != g_activeBurnVfx.end()) {
            if (now >= it->expiresAtMs) {
                expired.push_back(*it);
                it = g_activeBurnVfx.erase(it);
            } else {
                ++it;
            }
        }
        if (!expired.empty()) {
            g_activeBurnVfxCount.store(g_activeBurnVfx.size(), std::memory_order_relaxed);
        }
    }
    for (const ActiveBurnVfx& entry : expired) {
        StopBurnVfxEntryOnGameThread(entry);
    }
}

std::wstring TryReadDeclName(const void* decl) {
    if (decl == nullptr) {
        return {};
    }
    const void* namePointer = nullptr;
    if (!SafeRead(static_cast<const char*>(decl) + 0x08, &namePointer, sizeof(namePointer)) ||
        namePointer == nullptr) {
        return {};
    }
    char text[128]{};
    if (!SafeRead(namePointer, text, sizeof(text) - 1U)) {
        return {};
    }
    std::wstring name;
    for (const char c : text) {
        if (c == '\0') {
            break;
        }
        const auto raw = static_cast<unsigned char>(c);
        if (raw < 0x20U || raw > 0x7EU) {
            return {};
        }
        name.push_back(static_cast<wchar_t>(c));
    }
    return name;
}

bool Contains(const std::wstring& text, const wchar_t* needle) {
    return text.find(needle) != std::wstring::npos;
}

// Score freezes once the Hellwalker charge is held (latch on and threshold
// met): further kills do not accumulate until the bash spends the charge.
// If the player raises the target mid-charge past the current score, the
// freeze releases and accumulation resumes toward the new goal.
bool IsFrozenUnderMutex(int targetScore) {
    double points = 0.0;
    bool frozen = false;
    {
        std::lock_guard lock(g_scoreMutex);
        points = g_scorePoints;
        frozen = g_superchargedLatch && points >= static_cast<double>(targetScore);
    }
    if (frozen && !g_freezeLogged.exchange(true, std::memory_order_relaxed)) {
        wchar_t line[128]{};
        swprintf_s(line, L"[score] FROZEN at %g/%d — bash to spend the charge",
                   points, targetScore);
        LogLine(line);
    }
    return frozen;
}

bool IsShieldBashDecl(const std::wstring& declName) {
    if (declName.empty() || Contains(declName, L"ai/") ||
        Contains(declName, L"splash") || Contains(declName, L"execute")) {
        return false;
    }
    return Contains(declName, L"shield/bash") ||
           Contains(declName, L"shield/charge") ||
           Contains(declName, L"shield/juggernaut_charge") ||
           Contains(declName, L"player/shield_bash");
}

float BaseShieldDamageForDecl(const std::wstring& declName) {
    if (Contains(declName, L"charge_direct_from_above")) {
        return 3000.0F;
    }
    if (Contains(declName, L"charge_direct")) {
        return 1000.0F;
    }
    if (Contains(declName, L"juggernaut_charge") || Contains(declName, L"bash_sprint")) {
        return 800.0F;
    }
    if (Contains(declName, L"bash_direct") || Contains(declName, L"shield_bash")) {
        return 100.0F;
    }
    return 1000.0F;
}

void ApplyScoreGainLocked(double gained, int targetScore, double* outTotal, bool* outBecame) {
    std::lock_guard lock(g_scoreMutex);
    g_scorePoints += gained;
    const bool became = g_scorePoints >= static_cast<double>(targetScore);
    if (became && !g_superchargedLatch) {
        g_superchargedLatch = true;
    }
    *outTotal = g_scorePoints;
    *outBecame = became;
}

using AddScoreFn = std::uint64_t(__fastcall*)(void*, void*, float, bool, bool);

std::uint64_t SlotDetour_AddScore(void* manager, void* scoringItem, float multiplier,
                                  bool flag1, bool flag2) {
    static std::atomic<unsigned> anyCalls{0};
    const unsigned anyIndex = anyCalls.fetch_add(1, std::memory_order_relaxed);
    if (anyIndex < 30U) {
        const auto ret = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const std::wstring name = TryReadDeclName(scoringItem);
        wchar_t line[288]{};
        swprintf_s(line, L"[score-raw] #%u callerRva=0x%llX item=%s",
                   anyIndex + 1U,
                   static_cast<unsigned long long>(base ? ret - base : 0),
                   name.empty() ? L"<unnamed>" : name.c_str());
        LogLine(line);
    }
    return reinterpret_cast<AddScoreFn>(g_trampolineAddScore)(manager, scoringItem,
                                                              multiplier, flag1, flag2);
}

// idAI::DeferredDamage(aiEntity*, damageInfo*) -> float dealt damage.
using DamageFn = float(__fastcall*)(void* entity, void* damageInfo);

float __fastcall SlotDetour_DeferredDamage(void* entity, void* damageInfo) {
    if (!g_storyModeActive.load(std::memory_order_relaxed)) {
        return reinterpret_cast<DamageFn>(g_trampolineDamage)(entity, damageInfo);
    }
    DrainExpiredBurnVfx();

    // Damage packet layout (Update 5): idDeclDamage* at +0x350 (damageParms + 0x2F0),
    // damage amount float at +0x358.
    void* damageDecl = nullptr;
    float dmg = 0.0F;
    if (damageInfo != nullptr) {
        SafeRead(static_cast<const char*>(damageInfo) + kDamagePacketDeclOffset,
                 &damageDecl, sizeof(damageDecl));
        SafeRead(static_cast<const char*>(damageInfo) + 0x358, &dmg, sizeof(dmg));
    }
    const std::wstring rawName = damageDecl != nullptr ? TryReadDeclName(damageDecl)
                                                       : std::wstring{};

    float preHealth = 0.0F;
    float maxHealth = 0.0F;
    if (entity != nullptr) {
        SafeRead(static_cast<const char*>(entity) + BurnEngine::kHealthFieldOffset,
                 &preHealth, sizeof(preHealth));
        SafeRead(static_cast<const char*>(entity) + BurnEngine::kMaxHealthFieldOffset,
                 &maxHealth, sizeof(maxHealth));
    }

    static std::atomic<unsigned> anyDamage{0};
    const unsigned anyIdx = anyDamage.fetch_add(1, std::memory_order_relaxed);
    if (anyIdx < 48U) {
        wchar_t line[320]{};
        swprintf_s(line, L"[dmg-raw] #%u dmg=%.1f hp=%.0f/%.0f decl=%s",
                   anyIdx + 1U,
                   static_cast<double>(dmg),
                   static_cast<double>(preHealth),
                   static_cast<double>(maxHealth),
                   rawName.empty() ? L"<none>" : rawName.c_str());
        LogLine(line);
    }

    bool burnAfterImpact = false;
    bool superchargedApplied = false;
    int configuredDamage = 0;
    float targetDamage = 0.0F;
    float refMaxHealth = 0.0F;
    float baseDmgApplied = 0.0F;
    float targetScaleApplied = 0.0F;
    if (damageInfo != nullptr && IsShieldBashDecl(rawName)) {
        const unsigned n = g_loggedDamages.fetch_add(1, std::memory_order_relaxed);
        if (n < 60U) {
            wchar_t line[192]{};
            swprintf_s(line, L"[dash] shield damage event %u decl=%s dmg=%.1f",
                       n + 1U, rawName.c_str(), static_cast<double>(dmg));
            LogLine(line);
        }

        bool claimedSupercharge = false;
        double prevScore = 0.0;
        BridgeValues v{};
        if (g_gameplayStage.load(std::memory_order_relaxed) >= 1 && BridgeGetValues(&v)) {
            std::lock_guard lock(g_scoreMutex);
            if (v.enabled && g_superchargedLatch &&
                g_scorePoints >= static_cast<double>(v.targetScore)) {
                prevScore = g_scorePoints;
                configuredDamage = v.poweredDashDamage;
                g_scorePoints = 0.0;
                g_superchargedLatch = false;
                claimedSupercharge = true;
            }
        }
        if (claimedSupercharge) {
            const std::uint64_t monsterType = ReadAiMonsterType(entity);
            const DemonCategory category = ClassifyMonsterType(monsterType);
            refMaxHealth = maxHealth > 1.0F ? maxHealth : preHealth;
            targetDamage = SurvivalShieldCore::SuperchargedBashDamageFor(
                configuredDamage, refMaxHealth, category);
            baseDmgApplied = BaseShieldDamageForDecl(rawName);
            const float incomingScale = dmg > 0.01F ? dmg : 1.0F;
            const float bonusScale = targetDamage / baseDmgApplied;
            const float desiredScale = incomingScale > 1.01F
                ? (incomingScale + bonusScale)
                : std::max(incomingScale, bonusScale);
            targetScaleApplied = std::clamp(desiredScale, incomingScale,
                                            std::max(incomingScale, 500.0F));
            SIZE_T written = 0;
            if (WriteProcessMemory(GetCurrentProcess(),
                                   static_cast<char*>(damageInfo) + 0x358,
                                   &targetScaleApplied, sizeof(targetScaleApplied), &written) != FALSE &&
                written == sizeof(targetScaleApplied)) {
                g_freezeLogged.store(false, std::memory_order_relaxed);
                burnAfterImpact = true;
                superchargedApplied = true;
            } else {
                std::lock_guard lock(g_scoreMutex);
                g_scorePoints = prevScore;
                g_superchargedLatch = true;
            }
        }
    }

    const float dealt = reinterpret_cast<DamageFn>(g_trampolineDamage)(entity, damageInfo);
    if (superchargedApplied) {
        wchar_t line[256]{};
        swprintf_s(line,
                   L"[dash] SUPERCHARGED bash: entity=0x%p scale %.2f -> %.2f (cfg=%d, hp=%.0f, target=%.0f, base=%.0f, dealt=%.0f, %s)",
                   entity, static_cast<double>(dmg), static_cast<double>(targetScaleApplied),
                   configuredDamage, static_cast<double>(refMaxHealth),
                   static_cast<double>(targetDamage), static_cast<double>(baseDmgApplied),
                   static_cast<double>(dealt), rawName.c_str());
        LogLine(line);
    }
    if (burnAfterImpact && entity != nullptr) {
        float postHealth = 0.0F;
        if (!SafeRead(static_cast<const char*>(entity) + BurnEngine::kHealthFieldOffset,
                      &postHealth, sizeof(postHealth))) {
            LogLine(L"[burn] target health unreadable; no burn.");
        } else if (postHealth > 1.0F) {
            const float refMax = maxHealth > 1.0F ? maxHealth
                                                  : (preHealth > 1.0F ? preHealth : postHealth);
            g_burn.StartOrRefresh(entity, refMax);
            const wchar_t* vfxTag = StartOrRefreshBurnVfx(entity, refMax);
            wchar_t line[192]{};
            swprintf_s(line, L"[burn] applied: entity=0x%p hp=%.0f/%.0f (3%%x8 ticks, vfx=%s)",
                       entity, static_cast<double>(postHealth), static_cast<double>(refMax),
                       vfxTag);
            LogLine(line);
        } else {
            LogLine(L"[burn] target died to the bash; no burn.");
        }
    }
    return dealt;
}

void TryCreditParry(std::uint64_t sourceEvent) {
    if (!g_storyModeActive.load(std::memory_order_relaxed) || sourceEvent == 0) {
        return;
    }
    BridgeValues v{};
    if (!BridgeGetValues(&v) || !v.enabled || v.parryPct <= 0) {
        return;
    }
    g_hudKillSeq.fetch_add(1, std::memory_order_relaxed);
    if (IsFrozenUnderMutex(v.targetScore)) {
        return;
    }
    const double gained =
        static_cast<double>(v.targetScore) * static_cast<double>(v.parryPct) / 100.0;
    if (gained <= 0.0) {
        return;
    }
    double totalNow = 0.0;
    bool became = false;
    ApplyScoreGainLocked(gained, v.targetScore, &totalNow, &became);
    static std::atomic<unsigned> parryLogged{0};
    const unsigned n = parryLogged.fetch_add(1, std::memory_order_relaxed);
    if (n < 60U || became) {
        wchar_t line[192]{};
        swprintf_s(line, L"[parry] +%.0f points (%d%% of %d, ev=0x%llX) total=%.0f/%d%s",
                   gained, v.parryPct, v.targetScore,
                   static_cast<unsigned long long>(sourceEvent),
                   totalNow, v.targetScore, became ? L" SUPERCHARGED" : L"");
        LogLine(line);
    }
}

// idPlayerMechanicShieldParry::ExecuteParry (0x01AD76C0):
// returns > 0.0f on successful parry, 0.0f on miss/early-exit.
using ShieldParryFn = float(__fastcall*)(void* mechanic, void* attacker, void* inflictor,
                                         void* damageDecl, void* parryInfo, bool flag6);
std::atomic<long long> g_lastParryCreditMs{-100000};

float __fastcall SlotDetour_ShieldParry(void* mechanic, void* attacker, void* inflictor,
                                        void* damageDecl, void* parryInfo, bool flag6) {
    const float result = reinterpret_cast<ShieldParryFn>(g_trampolineParry)(
        mechanic, attacker, inflictor, damageDecl, parryInfo, flag6);
    if (g_storyModeActive.load(std::memory_order_relaxed)) {
        DrainExpiredBurnVfx();
        const unsigned idx = g_rawParry.fetch_add(1, std::memory_order_relaxed);
        if (idx < 32U) {
            wchar_t line[160]{};
            swprintf_s(line, L"[parry-raw] #%u ret=%.3f attacker=0x%p",
                       idx + 1U, static_cast<double>(result), attacker);
            LogLine(line);
        }
        if (result > 0.0F) {
            const ULONGLONG now = GetTickCount64();
            long long prev = g_lastParryCreditMs.load(std::memory_order_relaxed);
            if (now - prev >= 350ULL &&
                g_lastParryCreditMs.compare_exchange_strong(
                    prev, static_cast<long long>(now), std::memory_order_relaxed)) {
                TryCreditParry(1);
            }
        }
    }
    return result;
}

bool RecentKillDedup(const void* aiEntity) {
    constexpr std::size_t kDedupSlots = 256U;
    constexpr ULONGLONG kDedupWindowMs = 120000ULL;
    struct RecentKill {
        const void* entity;
        ULONGLONG tickMs;
    };
    static std::mutex dedupMutex;
    static RecentKill recent[kDedupSlots] = {};
    static std::size_t writePos = 0;

    const ULONGLONG now = GetTickCount64();
    std::lock_guard lock(dedupMutex);
    for (const RecentKill& entry : recent) {
        if (entry.entity == aiEntity && now - entry.tickMs < kDedupWindowMs) {
            return true;
        }
    }
    recent[writePos] = RecentKill{aiEntity, now};
    writePos = (writePos + 1U) % kDedupSlots;
    return false;
}

void CreditDemonKill(double basePoints, int pct, const wchar_t* category) {
    BridgeValues v{};
    if (!BridgeGetValues(&v) || !v.enabled) {
        return;
    }
    g_hudKillSeq.fetch_add(1, std::memory_order_relaxed);
    if (IsFrozenUnderMutex(v.targetScore)) {
        return;
    }
    const double gained = basePoints * static_cast<double>(pct) / 100.0;
    double totalNow = 0.0;
    bool became = false;
    ApplyScoreGainLocked(gained, v.targetScore, &totalNow, &became);
    const unsigned n = g_loggedKills.fetch_add(1, std::memory_order_relaxed);
    if (n < 200U || became) {
        wchar_t line[192]{};
        swprintf_s(line, L"[score] +%.0f (%s %d%%) total=%.0f/%d%s",
                   gained, category, pct, totalNow, v.targetScore,
                   became ? L" SUPERCHARGED" : L"");
        LogLine(line);
    }
}

// idAI::ProcessDeferredDamageRecord(aiEntity*, damageRecord*) @ 0x00EEFC10 (sole caller 0x00F7DC48).
using AiKilledFn = void(__fastcall*)(void* aiEntity, void* deathInfo);

void __fastcall SlotDetour_AIKilled(void* aiEntity, void* deathInfo) {
    void* attackerPtr = nullptr;
    if (deathInfo != nullptr) {
        SafeRead(static_cast<const char*>(deathInfo) + kDeathInfoAttackerPtrOffset,
                 &attackerPtr, sizeof(attackerPtr));
    }
    const std::uint64_t monsterType = ReadAiMonsterType(aiEntity);

    reinterpret_cast<AiKilledFn>(g_trampolineKilled)(aiEntity, deathInfo);

    if (!g_storyModeActive.load(std::memory_order_relaxed) || aiEntity == nullptr) {
        return;
    }
    DrainExpiredBurnVfx();

    float postHealth = -1.0F;
    if (!IsAiDead(aiEntity, &postHealth)) {
        return;
    }
    StopBurnVfxForEntity(aiEntity);

    if (attackerPtr == nullptr && deathInfo != nullptr) {
        SafeRead(static_cast<const char*>(deathInfo) + kDeathInfoAttackerPtrOffset,
                 &attackerPtr, sizeof(attackerPtr));
    }

    const bool bySlayer = IsLocalSlayer(attackerPtr);
    const bool duplicate = RecentKillDedup(aiEntity);

    const wchar_t* category = nullptr;
    double basePoints = 0.0;
    BridgeValues v{};
    BridgeGetValues(&v);
    int pct = 100;
    switch (ClassifyMonsterType(monsterType)) {
        case DemonCategory::Superheavy:
            category = L"superheavy";
            basePoints = kBaseSuperheavy;
            pct = v.pctSuperheavy;
            break;
        case DemonCategory::Heavy:
            category = L"heavy";
            basePoints = kBaseHeavy;
            pct = v.pctHeavy;
            break;
        case DemonCategory::Fodder:
            category = L"fodder";
            basePoints = kBaseFodder;
            pct = v.pctFodder;
            break;
        case DemonCategory::Other:
        default:
            break;
    }

    const unsigned idx = g_rawAiKill.fetch_add(1, std::memory_order_relaxed);
    if (idx < 64U) {
        wchar_t line[256]{};
        swprintf_s(line,
                   L"[ai-kill] #%u bySlayer=%d dup=%d hp=%.0f mt=0x%llX tier=%s victim=0x%p attacker=0x%p",
                   idx + 1U, bySlayer ? 1 : 0, duplicate ? 1 : 0,
                   static_cast<double>(postHealth),
                   static_cast<unsigned long long>(monsterType),
                   category != nullptr ? category : L"none",
                   aiEntity, attackerPtr);
        LogLine(line);
    }

    if (bySlayer && category != nullptr && !duplicate) {
        CreditDemonKill(basePoints, pct, category);
    }
}

// BroadcastNow(manager*, u64 systemId, u64 eventId, void* payload) — lightweight
// observer only (no kill/parry scoring on HUD events).
using HudFn = void(__fastcall*)(void*, std::uint64_t, std::uint64_t, void*);

void __fastcall SlotDetour_BroadcastNow(void* manager, std::uint64_t systemId,
                                        std::uint64_t eventId, void* payload) {
    if (g_storyModeActive.load(std::memory_order_relaxed)) {
        DrainExpiredBurnVfx();
        static std::mutex seenMutex;
        static std::unordered_set<std::uint64_t> seenIds;
        bool first = false;
        {
            std::lock_guard lock(seenMutex);
            if (seenIds.size() < 64U) {
                first = seenIds.insert(eventId).second;
            }
        }
        if (first) {
            unsigned payload0 = 0;
            if (payload != nullptr) {
                SafeRead(payload, &payload0, sizeof(payload0));
            }
            wchar_t line[160]{};
            swprintf_s(line, L"[hud-id] id=0x%llX sys=%llu payload0=%u",
                       static_cast<unsigned long long>(eventId),
                       static_cast<unsigned long long>(systemId), payload0);
            LogLine(line);
        }
    }
    reinterpret_cast<HudFn>(g_trampolineHud)(manager, systemId, eventId, payload);
}

// idEntity::Damage (0x0142DEC0, Update 5): 7-arg float-returning damage
// pipeline entry point. Observing in Story Mode logs every damage decl & amount
// cleanly without corrupting stack args 5-7 or the XMM0 float return value.
using PacketCtorFn = float(__fastcall*)(void* victim, void* inflictor,
                                        void* attacker, void* damageParms,
                                        float damageAmount, void* dirVec3,
                                        void* hitInfo);

float __fastcall SlotDetour_DamagePacketCtor(void* victim, void* inflictor,
                                             void* attacker, void* damageParms,
                                             float damageAmount, void* dirVec3,
                                             void* hitInfo) {
    if (g_storyModeActive.load(std::memory_order_relaxed)) {
        DrainExpiredBurnVfx();
        static std::atomic<unsigned> packets{0};
        const unsigned idx = packets.fetch_add(1, std::memory_order_relaxed);
        if (idx < 64U) {
            void* decl = nullptr;
            if (damageParms != nullptr) {
                SafeRead(static_cast<const char*>(damageParms) + kDamageParmsDeclOffset,
                         &decl, sizeof(decl));
            }
            const std::wstring name = decl != nullptr ? TryReadDeclName(decl) : std::wstring{};
            wchar_t line[320]{};
            swprintf_s(line,
                       L"[pkt] #%u dmg=%.1f decl=%s victim=0x%p inflictor=0x%p attacker=0x%p",
                       idx + 1U,
                       static_cast<double>(damageAmount),
                       name.empty() ? L"<none>" : name.c_str(),
                       victim, inflictor, attacker);
            LogLine(line);
        }
    }
    return reinterpret_cast<PacketCtorFn>(g_trampolinePacket)(
        victim, inflictor, attacker, damageParms, damageAmount, dirVec3, hitInfo);
}

bool WriteSlot(std::uintptr_t address, std::uintptr_t value) noexcept {
    DWORD oldProtect = 0;
    if (VirtualProtect(reinterpret_cast<void*>(address), 8, PAGE_READWRITE, &oldProtect) == FALSE) {
        return false;
    }
    *reinterpret_cast<std::uintptr_t*>(address) = value;
    DWORD temp = 0;
    VirtualProtect(reinterpret_cast<void*>(address), 8, oldProtect, &temp);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), 8);
    return true;
}

// The disk thunks read these slots on every call. Live sessions showed the
// entries being called while the detours stayed silent (the self-test at
// init worked, then nothing): something clears the slots after startup. The
// publisher thread re-asserts them every tick so the chain survives, and
// logs once the first time a slot is found different from what we wrote.
constexpr std::size_t kReassertSlotCount = 6;
std::uintptr_t g_slotAddrs[kReassertSlotCount] = {};
std::uintptr_t g_slotValues[kReassertSlotCount] = {};
std::atomic<bool> g_slotReassertLogged{false};

void ReassertSlots() noexcept {
    for (std::size_t i = 0; i < kReassertSlotCount; ++i) {
        if (g_slotAddrs[i] == 0) {
            continue;
        }
        std::uintptr_t current = 0;
        if (!SafeRead(reinterpret_cast<void*>(g_slotAddrs[i]), &current, sizeof(current))) {
            continue;
        }
        if (current != g_slotValues[i]) {
            if (!g_slotReassertLogged.exchange(true, std::memory_order_relaxed)) {
                wchar_t line[192]{};
                swprintf_s(line,
                           L"[gameplay] slot %llu was 0x%llX (expected 0x%llX); "
                           L"re-asserting every publish tick.",
                           static_cast<unsigned long long>(i),
                           static_cast<unsigned long long>(current),
                           static_cast<unsigned long long>(g_slotValues[i]));
                LogLine(line);
            }
            WriteSlot(g_slotAddrs[i], g_slotValues[i]);
        }
    }
}

int ReadGameplayStageFromDisk() noexcept {
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return 1;
    }
    std::wstring file(path, n);
    file += L"\\DOOMTheDarkAges\\SurvivalShield\\gameplaystage.txt";
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return 1;
    }
    char text[8]{};
    DWORD read = 0;
    ReadFile(h, text, sizeof(text) - 1U, &read, nullptr);
    CloseHandle(h);
    return std::clamp(atoi(text), 0, 1);
}

// Publisher half of the HUD bridge: mirrors the score snapshot into the
// named shared section the loader overlay polls. Also ticks the burn engine
// at 10 Hz (its own ticks are 500 ms apart, so this is comfortably exact).
DWORD WINAPI RuntimePublisherThread(LPVOID) noexcept {
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        0, sizeof(HudSharedState), kHudSharedName);
    if (mapping == nullptr) {
        LogLine(L"[hud-pub] shared section unavailable; overlay stays dark.");
    }
    auto* shared = mapping != nullptr
        ? static_cast<HudSharedState*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                                     sizeof(HudSharedState)))
        : nullptr;
    if (shared != nullptr) {
        shared->magic = kHudSharedMagic;
        shared->version = kHudSharedVersion;
    } else {
        LogLine(L"[hud-pub] map view failed; overlay stays dark.");
    }

    bool burnAnnounced = false;
    for (;;) {
        Sleep(150);
        ReassertSlots();
        ReassertUiSlots();
        const bool storyActive = RefreshGameModeState();
        if (storyActive) {
            g_burn.Tick();
            const int ticksNow = static_cast<int>(g_burn.ActiveCount());
            if (ticksNow > 0 && !burnAnnounced) {
                burnAnnounced = true;
                wchar_t line[96]{};
                swprintf_s(line, L"[burn] active targets: %d", ticksNow);
                LogLine(line);
            } else if (ticksNow == 0) {
                burnAnnounced = false;
            }
        } else {
            burnAnnounced = false;
        }

        if (shared == nullptr) {
            continue;
        }
        BridgeValues v{};
        double points = 0.0;
        bool latched = false;
        if (BridgeGetValues(&v)) {
            std::lock_guard lock(g_scoreMutex);
            points = g_scorePoints;
            latched = g_superchargedLatch;
        } else {
            std::lock_guard lock(g_scoreMutex);
            points = g_scorePoints;
            latched = g_superchargedLatch;
            v.enabled = false;
            v.targetScore = 100;
        }
        const double target = static_cast<double>(v.targetScore > 0 ? v.targetScore : 1);
        shared->enabled = (v.enabled && storyActive) ? 1U : 0U;
        shared->supercharged =
            (v.enabled && storyActive && latched && points >= static_cast<double>(v.targetScore))
                ? 1U
                : 0U;
        shared->scorePoints = static_cast<std::uint64_t>(points);
        shared->targetScore = v.targetScore;
        shared->scoreFraction = static_cast<float>(std::clamp(points / target, 0.0, 1.0));
        shared->killSeq = g_hudKillSeq.load(std::memory_order_relaxed);
        shared->heartbeatMs = GetTickCount64();
    }
}

void StartRuntimePublisher() {
    HANDLE thread = CreateThread(nullptr, 0, RuntimePublisherThread, nullptr, 0, nullptr);
    if (thread != nullptr) {
        CloseHandle(thread); // process-lifetime worker; freeing the handle is safe
    } else {
        LogLine(L"[hud-pub] runtime thread failed to start.");
    }
}

} // namespace

bool GetLiveScore(unsigned long long* scorePoints, int* targetScore, bool* supercharged) {
    if (scorePoints == nullptr || targetScore == nullptr || supercharged == nullptr) {
        return false;
    }
    BridgeValues v{};
    if (!BridgeGetValues(&v)) {
        return false;
    }
    std::lock_guard lock(g_scoreMutex);
    *scorePoints = static_cast<unsigned long long>(g_scorePoints);
    *targetScore = v.targetScore;
    *supercharged = v.enabled && g_superchargedLatch &&
                    g_scorePoints >= static_cast<double>(v.targetScore);
    return true;
}

bool InstallGameplayBridge() {
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (moduleBase == 0) {
        LogLine(L"[gameplay] game module base unavailable.");
        return false;
    }
    RefreshGameModeState();

    const std::uintptr_t slotScore = moduleBase + kSlotAddScoreRva;
    const std::uintptr_t slotDamage = moduleBase + kSlotDamageRva;
    std::uintptr_t curScore = 0, curDamage = 0;
    if (!SafeRead(reinterpret_cast<void*>(slotScore), &curScore, sizeof(curScore)) ||
        !SafeRead(reinterpret_cast<void*>(slotDamage), &curDamage, sizeof(curDamage))) {
        LogLine(L"[gameplay] slots unreadable; is the EXE v7-patched?");
        return false;
    }
    if (curScore != 0 && curScore != reinterpret_cast<std::uintptr_t>(&SlotDetour_AddScore)) {
        LogLine(L"[gameplay] unexpected AddScore slot; refusing.");
        return false;
    }
    if (curDamage != 0 && curDamage != reinterpret_cast<std::uintptr_t>(&SlotDetour_DeferredDamage)) {
        LogLine(L"[gameplay] unexpected damage slot; refusing.");
        return false;
    }

    g_trampolineAddScore = moduleBase + kTrampAddScoreRva;
    g_trampolineDamage = moduleBase + kTrampDamageRva;
    g_gameplayStage.store(ReadGameplayStageFromDisk(), std::memory_order_relaxed);

    const std::uintptr_t slotParry = moduleBase + kSlotParryRva;
    std::uintptr_t curParry = 0;
    if (!SafeRead(reinterpret_cast<void*>(slotParry), &curParry, sizeof(curParry)) ||
        (curParry != 0 &&
         curParry != reinterpret_cast<std::uintptr_t>(&SlotDetour_ShieldParry))) {
        LogLine(L"[gameplay] parry slot unreadable; refusing.");
        return false;
    }
    g_trampolineParry = moduleBase + kTrampParryRva;

    const std::uintptr_t slotKilled = moduleBase + kSlotKilledRva;
    std::uintptr_t curKilled = 0;
    if (!SafeRead(reinterpret_cast<void*>(slotKilled), &curKilled, sizeof(curKilled)) ||
        (curKilled != 0 &&
         curKilled != reinterpret_cast<std::uintptr_t>(&SlotDetour_AIKilled))) {
        LogLine(L"[gameplay] killed slot unreadable; refusing.");
        return false;
    }
    g_trampolineKilled = moduleBase + kTrampKilledRva;

    const std::uintptr_t slotHud = moduleBase + kSlotHudRva;
    std::uintptr_t curHud = 0;
    if (!SafeRead(reinterpret_cast<void*>(slotHud), &curHud, sizeof(curHud)) ||
        (curHud != 0 && curHud != reinterpret_cast<std::uintptr_t>(&SlotDetour_BroadcastNow))) {
        LogLine(L"[gameplay] hud slot unreadable; refusing.");
        return false;
    }
    g_trampolineHud = moduleBase + kTrampHudRva;

    const std::uintptr_t slotPacket = moduleBase + kSlotPacketRva;
    std::uintptr_t curPacket = 0;
    if (!SafeRead(reinterpret_cast<void*>(slotPacket), &curPacket, sizeof(curPacket)) ||
        (curPacket != 0 &&
         curPacket != reinterpret_cast<std::uintptr_t>(&SlotDetour_DamagePacketCtor))) {
        LogLine(L"[gameplay] packet slot unreadable; refusing.");
        return false;
    }
    g_trampolinePacket = moduleBase + kTrampPacketRva;

    if (!WriteSlot(slotScore, reinterpret_cast<std::uintptr_t>(&SlotDetour_AddScore)) ||
        !WriteSlot(slotDamage, reinterpret_cast<std::uintptr_t>(&SlotDetour_DeferredDamage)) ||
        !WriteSlot(slotParry, reinterpret_cast<std::uintptr_t>(&SlotDetour_ShieldParry)) ||
        !WriteSlot(slotKilled, reinterpret_cast<std::uintptr_t>(&SlotDetour_AIKilled)) ||
        !WriteSlot(slotHud, reinterpret_cast<std::uintptr_t>(&SlotDetour_BroadcastNow)) ||
        !WriteSlot(slotPacket, reinterpret_cast<std::uintptr_t>(&SlotDetour_DamagePacketCtor))) {
        LogLine(L"[gameplay] slot write failed.");
        return false;
    }

    g_slotAddrs[0] = slotScore;
    g_slotValues[0] = reinterpret_cast<std::uintptr_t>(&SlotDetour_AddScore);
    g_slotAddrs[1] = slotDamage;
    g_slotValues[1] = reinterpret_cast<std::uintptr_t>(&SlotDetour_DeferredDamage);
    g_slotAddrs[2] = slotParry;
    g_slotValues[2] = reinterpret_cast<std::uintptr_t>(&SlotDetour_ShieldParry);
    g_slotAddrs[3] = slotKilled;
    g_slotValues[3] = reinterpret_cast<std::uintptr_t>(&SlotDetour_AIKilled);
    g_slotAddrs[4] = slotHud;
    g_slotValues[4] = reinterpret_cast<std::uintptr_t>(&SlotDetour_BroadcastNow);
    g_slotAddrs[5] = slotPacket;
    g_slotValues[5] = reinterpret_cast<std::uintptr_t>(&SlotDetour_DamagePacketCtor);

    // Self-test: invoke the AddScore entry directly with null arguments (the
    // original rejects a null scoring item early, so this is side-effect free).
    // If the slot chain works, our detour logs [score-raw] #1; if not, the
    // entry path itself is broken. Only run once per process.
    static std::atomic<bool> selfTested{false};
    if (!selfTested.exchange(true)) {
        LogLine(L"[gameplay] self-test: invoking AddScore chain with null args...");
        reinterpret_cast<AddScoreFn>(moduleBase + kEntryAddScoreRva)(nullptr, nullptr, 1.0F, false, false);
        LogLine(L"[gameplay] self-test call returned.");
    }

    wchar_t line[160]{};
    swprintf_s(line,
               L"[gameplay] active (AddScore + idAI::DeferredDamage + ShieldParry + idAI::Killed + HUD + idEntity::Damage), stage %d.",
               g_gameplayStage.load());
    LogLine(line);

    StartRuntimePublisher();
    return true;
}

} // namespace survival_shield
