#include "GameHooks.hpp"

#include "Log.hpp"

#ifdef SURVIVALSHIELD_DIAGNOSTIC_HOOKS

#include <Windows.h>
#include <MinHook.h>
#include <intrin.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <string_view>

namespace survival_shield {
namespace {

constexpr std::uintptr_t kAddScoreRva = 0x0197CAD0;
constexpr std::uintptr_t kBroadcastNowRva = 0x00492160;
constexpr std::uintptr_t kPacketCtorRva = 0x013FC100;
constexpr std::uintptr_t kDeathScoreReturnRva = 0x00ED712B;
constexpr unsigned kMaximumLoggedCalls = 512;
constexpr unsigned kMaximumItemDumps = 64;
constexpr unsigned kMaximumLoggedBroadcasts = 512;

using AddScoreFunction = void(__fastcall*)(void*, void*, float, bool, bool);
AddScoreFunction g_originalAddScore = nullptr;
std::atomic<unsigned> g_loggedAddScoreCalls{0};

using BroadcastNowFunction = void(__fastcall*)(void*, std::uint64_t, std::uint64_t, void*);
BroadcastNowFunction g_originalBroadcastNow = nullptr;
std::atomic<unsigned> g_loggedBroadcasts{0};

const wchar_t* HudEventName(std::uint64_t eventId) noexcept {
    switch (eventId) {
    case 0x2E: return L"HUD_EVENT_PLAYER_DEMON_KILL";
    case 0x155: return L"HUD_EVENT_HORDE_GAIN_SCORE";
    case 0x156: return L"HUD_EVENT_HORDE_SET_SCORE";
    case 0x15F: return L"HUD_EVENT_HORDE_BLITZ_SCORE";
    case 0x16C: return L"HUD_EVENT_HORDE_TOTAL_UPDATE";
    default: return nullptr;
    }
}

SIZE_T HudEventPayloadSize(std::uint64_t eventId) noexcept {
    switch (eventId) {
    case 0x2E: return sizeof(std::uint32_t); // demonKillPoints
    case 0x155: return 0x30;                // Horde scoring update payload
    case 0x156: return 0x40;                // score-manager set-score event
    case 0x15F: return 0x40;                // blitz score event
    case 0x16C: return sizeof(std::uint32_t);
    default: return 0;
    }
}

void LogHudEventPayload(std::uint64_t eventId, void* payload, unsigned callIndex) noexcept {
    const SIZE_T payloadSize = HudEventPayloadSize(eventId);
    if (payload == nullptr || payloadSize == 0) {
        return;
    }

    std::array<unsigned char, 0x40> bytes{};
    SIZE_T bytesRead = 0;
    if (ReadProcessMemory(GetCurrentProcess(), payload, bytes.data(), payloadSize, &bytesRead) == FALSE ||
        bytesRead != payloadSize) {
        wchar_t line[180]{};
        swprintf_s(line, L"Diagnostic HUD event #%u payload read failed (%llu/%llu bytes, error %lu).",
                   callIndex + 1U, static_cast<unsigned long long>(bytesRead),
                   static_cast<unsigned long long>(payloadSize), GetLastError());
        LogLine(line);
        return;
    }

    static constexpr wchar_t kHex[] = L"0123456789ABCDEF";
    wchar_t line[260]{};
    const int prefixLength = swprintf_s(line, L"Diagnostic HUD event #%u payload=", callIndex + 1U);
    if (prefixLength <= 0 || static_cast<std::size_t>(prefixLength) + payloadSize * 2U >=
                                 (sizeof(line) / sizeof(line[0]))) {
        return;
    }

    std::size_t position = static_cast<std::size_t>(prefixLength);
    for (SIZE_T index = 0; index < payloadSize; ++index) {
        const unsigned char byte = bytes[index];
        line[position++] = kHex[(byte >> 4U) & 0x0FU];
        line[position++] = kHex[byte & 0x0FU];
    }
    LogLine(std::wstring_view(line, position));
}

// ---- HUD/packet probe (SURVIVALSHIELD_DIAGNOSTIC_HUD_PACKET) ----
// Independent of the slot-based bridge: MinHook hooks the same two entries
// the slots observe, logs bounded samples of EVERY BroadcastNow call and
// every damage-packet construction, and forwards unchanged. Used to tell
// "the game never calls these entries in this mode" apart from "the slot
// chain is broken".
std::atomic<unsigned> g_probeBroadcasts{0};
std::atomic<unsigned> g_probePackets{0};
using ProbePacketCtorFn = void(__fastcall*)(void*, void*, void*, void*);
ProbePacketCtorFn g_originalPacketCtor = nullptr;

void __fastcall ProbeBroadcastNow(void* manager, std::uint64_t systemId,
                                  std::uint64_t eventId, void* payload) noexcept {
    const unsigned callIndex = g_probeBroadcasts.fetch_add(1, std::memory_order_relaxed);
    if (callIndex < 256U) {
        unsigned char bytes[8] = {};
        if (payload != nullptr) {
            ReadProcessMemory(GetCurrentProcess(), payload, bytes, sizeof(bytes), nullptr);
        }
        unsigned v0 = 0;
        std::memcpy(&v0, bytes, 4);
        wchar_t line[192]{};
        swprintf_s(line, L"Diagnostic PROBE HUD #%u system=%llu id=0x%llX p0=%u",
                   callIndex + 1U, static_cast<unsigned long long>(systemId),
                   static_cast<unsigned long long>(eventId), v0);
        LogLine(line);
    }
    if (g_originalBroadcastNow != nullptr) {
        g_originalBroadcastNow(manager, systemId, eventId, payload);
    }
}

void __fastcall ProbePacketCtor(void* packet, void* handle2, void* handle3,
                                void* handle4) noexcept {
    const unsigned callIndex = g_probePackets.fetch_add(1, std::memory_order_relaxed);
    if (callIndex < 64U) {
        unsigned long long declId = 0;
        void* decl = nullptr;
        float dmg = 0.0F;
        unsigned long long h2 = 0, h3 = 0, h4 = 0;
        if (packet != nullptr) {
            ReadProcessMemory(GetCurrentProcess(),
                              static_cast<const char*>(packet) + 0x60, &declId,
                              sizeof(declId), nullptr);
            ReadProcessMemory(GetCurrentProcess(),
                              static_cast<const char*>(packet) + 0x68, &decl,
                              sizeof(decl), nullptr);
            ReadProcessMemory(GetCurrentProcess(),
                              static_cast<const char*>(packet) + 0x358, &dmg,
                              sizeof(dmg), nullptr);
        }
        if (handle2 != nullptr) {
            ReadProcessMemory(GetCurrentProcess(), handle2, &h2, sizeof(h2), nullptr);
        }
        if (handle3 != nullptr) {
            ReadProcessMemory(GetCurrentProcess(), handle3, &h3, sizeof(h3), nullptr);
        }
        if (handle4 != nullptr) {
            ReadProcessMemory(GetCurrentProcess(), handle4, &h4, sizeof(h4), nullptr);
        }
        wchar_t line[320]{};
        swprintf_s(line,
                   L"Diagnostic PROBE PKT #%u declId=%llu decl=%p dmg=%.0f h2=%llu h3=%llu h4=%llu",
                   callIndex + 1U, static_cast<unsigned long long>(declId), decl,
                   static_cast<double>(dmg),
                   static_cast<unsigned long long>(h2),
                   static_cast<unsigned long long>(h3),
                   static_cast<unsigned long long>(h4));
        LogLine(line);
    }
    if (g_originalPacketCtor != nullptr) {
        g_originalPacketCtor(packet, handle2, handle3, handle4);
    }
}

void __fastcall ObserveBroadcastNow(void* manager,
                                   std::uint64_t systemId,
                                   std::uint64_t eventId,
                                   void* payload) noexcept {
    const wchar_t* eventName = HudEventName(eventId);
    if (eventName != nullptr) {
        const unsigned callIndex = g_loggedBroadcasts.fetch_add(1, std::memory_order_relaxed);
        if (callIndex < kMaximumLoggedBroadcasts) {
            wchar_t line[240]{};
            swprintf_s(line, L"Diagnostic HUD event #%u system=%llu id=0x%llX name=%s payload=%p",
                       callIndex + 1U, static_cast<unsigned long long>(systemId),
                       static_cast<unsigned long long>(eventId), eventName, payload);
            LogLine(line);
            LogHudEventPayload(eventId, payload, callIndex);
        }
    }

    if (g_originalBroadcastNow != nullptr) {
        g_originalBroadcastNow(manager, systemId, eventId, payload);
    }
}

void LogScoringItemBytes(void* scoringItem, unsigned callIndex) noexcept {
    if (scoringItem == nullptr) {
        LogLine(L"Diagnostic AddScore scoring item is null; memory dump skipped.");
        return;
    }

    // AddScore reads fields through +0xE8 in this supported executable. Capture
    // the object region that contains its type/id/value fields for offline ABI
    // mapping; never dereference the game pointer directly in the hook.
    constexpr SIZE_T kDumpSize = 0x80;
    std::array<unsigned char, kDumpSize> bytes{};
    SIZE_T bytesRead = 0;
    const auto* source = static_cast<const unsigned char*>(scoringItem) + 0x80;
    if (ReadProcessMemory(GetCurrentProcess(), source, bytes.data(), bytes.size(), &bytesRead) == FALSE ||
        bytesRead != bytes.size()) {
        wchar_t line[160]{};
        swprintf_s(line, L"Diagnostic AddScore item memory read failed at +0x80 (%llu/%llu bytes, error %lu).",
                   static_cast<unsigned long long>(bytesRead),
                   static_cast<unsigned long long>(bytes.size()), GetLastError());
        LogLine(line);
        return;
    }

    static constexpr wchar_t kHex[] = L"0123456789ABCDEF";
    wchar_t line[360]{};
    const int prefixLength = swprintf_s(line, L"Diagnostic AddScore item #%u bytes +0x80..+0xFF=",
                                        callIndex + 1U);
    if (prefixLength <= 0 || static_cast<std::size_t>(prefixLength) + bytes.size() * 2U >=
                                 (sizeof(line) / sizeof(line[0]))) {
        return;
    }

    std::size_t position = static_cast<std::size_t>(prefixLength);
    for (const unsigned char byte : bytes) {
        line[position++] = kHex[(byte >> 4U) & 0x0FU];
        line[position++] = kHex[byte & 0x0FU];
    }
    LogLine(std::wstring_view(line, position));
}

void __fastcall ObserveAddScore(void* scoreManager,
                                void* scoringItem,
                                float multiplier,
                                bool flag1,
                                bool flag2) noexcept {
    const auto returnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const std::uintptr_t returnRva = moduleBase != 0 && returnAddress >= moduleBase
                                         ? returnAddress - moduleBase
                                         : 0;
    const bool knownDeathCallsite = returnRva == kDeathScoreReturnRva;
#if defined(SURVIVALSHIELD_DIAGNOSTIC_ALL_CALLSCORE)
    const bool shouldLog = moduleBase != 0;
#else
    const bool shouldLog = knownDeathCallsite;
#endif
    if (shouldLog) {
        const unsigned callIndex = g_loggedAddScoreCalls.fetch_add(1, std::memory_order_relaxed);
        if (callIndex < kMaximumLoggedCalls) {
            wchar_t line[240]{};
            swprintf_s(line,
                       L"Diagnostic AddScore call #%u callerRva=0x%llX expectedDeathSite=%u manager=%p item=%p multiplier=%.3f flags=%u,%u",
                       callIndex + 1U, static_cast<unsigned long long>(returnRva),
                       knownDeathCallsite ? 1U : 0U, scoreManager, scoringItem, multiplier,
                       flag1 ? 1U : 0U, flag2 ? 1U : 0U);
            LogLine(line);
        }
        if (callIndex < kMaximumItemDumps) {
            LogScoringItemBytes(scoringItem, callIndex);
        }
    }

    if (g_originalAddScore != nullptr) {
        g_originalAddScore(scoreManager, scoringItem, multiplier, flag1, flag2);
    }
}

} // namespace

bool InstallLoggingHooks() {
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (moduleBase == 0) {
        LogLine(L"Diagnostic AddScore observer: game module base unavailable.");
        return false;
    }

    const MH_STATUS initializeStatus = MH_Initialize();
    if (initializeStatus != MH_OK && initializeStatus != MH_ERROR_ALREADY_INITIALIZED) {
        LogLine(L"Diagnostic AddScore observer: MinHook initialization failed.");
        return false;
    }

#if defined(SURVIVALSHIELD_DIAGNOSTIC_HUD_PACKET)
    void* broadcastTarget = reinterpret_cast<void*>(moduleBase + kBroadcastNowRva);
    const MH_STATUS broadcastCreateStatus = MH_CreateHook(
        broadcastTarget, reinterpret_cast<void*>(&ProbeBroadcastNow),
        reinterpret_cast<void**>(&g_originalBroadcastNow));
    if (broadcastCreateStatus != MH_OK && broadcastCreateStatus != MH_ERROR_ALREADY_CREATED) {
        LogLine(L"Probe: BroadcastNow hook creation failed.");
        return false;
    }
    const MH_STATUS broadcastEnableStatus = MH_EnableHook(broadcastTarget);
    if (broadcastEnableStatus != MH_OK && broadcastEnableStatus != MH_ERROR_ENABLED) {
        LogLine(L"Probe: BroadcastNow hook enable failed.");
        return false;
    }
    LogLine(L"Probe: BroadcastNow MinHook installed at game RVA 0x00492160 (logs every call, bounded).");

    void* packetTarget = reinterpret_cast<void*>(moduleBase + kPacketCtorRva);
    const MH_STATUS packetCreateStatus = MH_CreateHook(
        packetTarget, reinterpret_cast<void*>(&ProbePacketCtor),
        reinterpret_cast<void**>(&g_originalPacketCtor));
    if (packetCreateStatus != MH_OK && packetCreateStatus != MH_ERROR_ALREADY_CREATED) {
        LogLine(L"Probe: packet-ctor hook creation failed.");
        return false;
    }
    const MH_STATUS packetEnableStatus = MH_EnableHook(packetTarget);
    if (packetEnableStatus != MH_OK && packetEnableStatus != MH_ERROR_ENABLED) {
        LogLine(L"Probe: packet-ctor hook enable failed.");
        return false;
    }
    LogLine(L"Probe: FUN_1413FC100 MinHook installed at game RVA 0x013FC100 (logs every damage packet, bounded).");
    return true;
#elif defined(SURVIVALSHIELD_DIAGNOSTIC_HUD_EVENTS)
    void* broadcastTarget = reinterpret_cast<void*>(moduleBase + kBroadcastNowRva);
    const MH_STATUS broadcastCreateStatus = MH_CreateHook(
        broadcastTarget, reinterpret_cast<void*>(&ObserveBroadcastNow),
        reinterpret_cast<void**>(&g_originalBroadcastNow));
    if (broadcastCreateStatus != MH_OK && broadcastCreateStatus != MH_ERROR_ALREADY_CREATED) {
        LogLine(L"Diagnostic HUD event observer: hook creation failed.");
        return false;
    }

    const MH_STATUS broadcastEnableStatus = MH_EnableHook(broadcastTarget);
    if (broadcastEnableStatus != MH_OK && broadcastEnableStatus != MH_ERROR_ENABLED) {
        LogLine(L"Diagnostic HUD event observer: hook enable failed.");
        return false;
    }
    LogLine(L"Diagnostic HUD event observer installed at game RVA 0x00492160; captures PlayerDemonKill and Horde score events, then forwards unchanged.");
    return true;
#else

    void* target = reinterpret_cast<void*>(moduleBase + kAddScoreRva);
    const MH_STATUS createStatus = MH_CreateHook(
        target, reinterpret_cast<void*>(&ObserveAddScore),
        reinterpret_cast<void**>(&g_originalAddScore));
    if (createStatus != MH_OK && createStatus != MH_ERROR_ALREADY_CREATED) {
        LogLine(L"Diagnostic AddScore observer: hook creation failed.");
        return false;
    }

    const MH_STATUS enableStatus = MH_EnableHook(target);
    if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED) {
        LogLine(L"Diagnostic AddScore observer: hook enable failed.");
        return false;
    }

#if defined(SURVIVALSHIELD_DIAGNOSTIC_ALL_CALLSCORE)
    LogLine(L"Diagnostic AddScore observer installed at game RVA 0x0197CAD0; capture=all-callers, log-and-forward only.");
#else
    LogLine(L"Diagnostic AddScore observer installed at game RVA 0x0197CAD0; capture=known-death-caller, log-and-forward only.");
#endif
    return true;
#endif
}

} // namespace survival_shield

#else

namespace survival_shield {

bool InstallLoggingHooks() {
    // Fail closed until gameplay/UI adapters have a confirmed ABI and can be
    // validated against the supported game build.
    LogLine(L"Gameplay/UI hooks are not implemented; no hooks were installed.");
    return false;
}

} // namespace survival_shield

#endif
