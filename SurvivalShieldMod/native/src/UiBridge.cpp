#include "UiBridge.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "Log.hpp"
#include "BridgeConfig.hpp"
#include "ModsMenu.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace survival_shield {
namespace {

// Image-relative addresses installed on disk by native/tools/patch_u5.py.
// The patched entries jump to a static dispatcher whose target slot lives in
// .mydata; with the slots at zero the game runs unmodified. This DLL only
// writes the slots — no code bytes change at runtime (the game sabotages
// runtime-patched code and exits silently when debug registers are armed).
constexpr std::uint32_t kTrampGetRva = 0x00488B10;  // displaced insn + jmp entry+5
constexpr std::uint32_t kTrampSetRva = 0x004AA660;
constexpr std::uint32_t kTrampRadioSyncRva = 0x0080FCA0;
constexpr std::uint32_t kTrampRadioApplyRva = 0x00810380;
constexpr std::uint32_t kSwfVarToIntRva = 0x0054A350;
constexpr std::uint32_t kSlotGetRva = 0x0B613010;   // 8 bytes in .mydata
constexpr std::uint32_t kSlotSetRva = 0x0B613018;
constexpr std::uint32_t kSlotRadioSyncRva = 0x0B613050;
constexpr std::uint32_t kSlotRadioApplyRva = 0x0B613058;

constexpr std::size_t kUiSlotCount = 4;
std::uintptr_t g_uiSlotAddrs[kUiSlotCount] = {};
std::uintptr_t g_uiSlotValues[kUiSlotCount] = {};
std::atomic<bool> g_uiSlotReassertLogged{false};

using GetSettingsDataValueFn = bool(__fastcall*)(void* model, void* value);
using SetSettingsDataValueFn = bool(__fastcall*)(void* model, void* value);
using RadioButtonSyncFn = void(__fastcall*)(void* model, void* widgetRefPtr);
using RadioButtonApplyFn = bool(__fastcall*)(void* widget, void* eventArgs,
                                             void* arg3, void* arg4);
using SwfVarToIntFn = std::int32_t(__fastcall*)(void* swfVar);

std::uintptr_t g_trampolineGet = 0;
std::uintptr_t g_trampolineSet = 0;
std::uintptr_t g_trampolineRadioSync = 0;
std::uintptr_t g_trampolineRadioApply = 0;

enum class Row {
    None = 0,
    Enabled,
    TargetScore,
    PctFodder,
    PctHeavy,
    PctSuperheavy,
    PoweredDashDamage,
    ParryPct,
    SkipIntro,
};

constexpr std::size_t kRowCount = 8;
const wchar_t* const kRowDeclNames[kRowCount] = {
    L"game/hellwalker_enabled",
    L"game/hellwalker_target_score",
    L"game/hellwalker_score_pct_fodder",
    L"game/hellwalker_score_pct_heavy",
    L"game/hellwalker_score_pct_superheavy",
    L"game/hellwalker_powered_dash_damage",
    L"game/hellwalker_parry_points",
    L"game/hellwalker_skip_intro",
};

std::atomic<int> g_stage{0};

bool SafeRead(const void* address, void* buffer, std::size_t bytes) noexcept {
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, buffer, bytes, &bytesRead) != FALSE &&
           bytesRead == bytes;
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
    char text[96]{};
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

Row ClassifyModel(void* model) {
    const void* settingDecl = nullptr;
    if (!SafeRead(static_cast<const char*>(model) + 0x428, &settingDecl, sizeof(settingDecl)) ||
        settingDecl == nullptr) {
        return Row::None;
    }
    const std::wstring name = TryReadDeclName(settingDecl);
    for (std::size_t i = 0; i < kRowCount; ++i) {
        if (name == kRowDeclNames[i]) {
            return static_cast<Row>(i + 1);
        }
    }
    return Row::None;
}

// Fase 3 "menú de mods": filas generadas game/modslist_<menuId>. Devuelve el
// menuId (sin prefijo) si el modelo es una fila del listado.
std::wstring TryClassifyModsListRow(void* model) {
    const void* settingDecl = nullptr;
    if (!SafeRead(static_cast<const char*>(model) + 0x428, &settingDecl, sizeof(settingDecl)) ||
        settingDecl == nullptr) {
        return {};
    }
    const std::wstring name = TryReadDeclName(settingDecl);
    constexpr std::wstring_view kPrefix = L"game/modslist_";
    if (name.size() <= kPrefix.size() ||
        name.compare(0, kPrefix.size(), kPrefix) != 0) {
        return {};
    }
    return name.substr(kPrefix.size());
}

// The widget value object stores its data pointer at +0x30 (verified live:
// GetSettingsDataValue copies the property value into *(value + 0x30), and
// SetSettingsDataValue passes *(value + 0x30) to the property writer).
void* WidgetValueData(void* value) noexcept {
    if (value == nullptr) {
        return nullptr;
    }
    void* data = nullptr;
    if (!SafeRead(static_cast<const char*>(value) + 0x30, &data, sizeof(data))) {
        return nullptr;
    }
    return data;
}

int PctFromFloat(float value) {
    return std::clamp(static_cast<int>(value * 100.0F + 0.5F), 0, 100);
}

void MapRawToConfig(Row row, const unsigned char* raw, BridgeValues& v) {
    if (row == Row::Enabled) {
        v.enabled = raw[0] != 0;
        return;
    }
    if (row == Row::SkipIntro) {
        v.skipIntro = raw[0] != 0;
        return;
    }
    float f = 0.0F;
    std::memcpy(&f, raw, sizeof(f));
    switch (row) {
        case Row::TargetScore: {
            const int rawInt = static_cast<int>(f + 0.5F);
            v.targetScore = std::clamp(((std::clamp(rawInt, 100, 10000) + 50) / 100) * 100, 100, 10000);
            break;
        }
        case Row::PoweredDashDamage: {
            const int rawInt = static_cast<int>(f + 0.5F);
            v.poweredDashDamage = std::clamp(((std::clamp(rawInt, 100, 5000) + 25) / 50) * 50, 100, 5000);
            break;
        }
        case Row::ParryPct: {
            const int rawPct = static_cast<int>(f * 100.0F + 0.5F);
            v.parryPct = std::clamp(((std::clamp(rawPct, 0, 50) + 5) / 10) * 10, 0, 50);
            break;
        }
        case Row::PctFodder: v.pctFodder = PctFromFloat(f); break;
        case Row::PctHeavy: v.pctHeavy = PctFromFloat(f); break;
        case Row::PctSuperheavy: v.pctSuperheavy = PctFromFloat(f); break;
        default: break;
    }
}

bool RawInRange(Row row, const unsigned char* raw) {
    if (row == Row::Enabled || row == Row::SkipIntro) {
        return true;
    }
    float f = 0.0F;
    std::memcpy(&f, raw, sizeof(f));
    switch (row) {
        case Row::TargetScore: return f >= 99.5F && f <= 10000.5F;
        case Row::PoweredDashDamage: return f >= 99.5F && f <= 5000.5F;
        case Row::ParryPct: return f >= -0.001F && f <= 0.501F;
        case Row::PctFodder:
        case Row::PctHeavy:
        case Row::PctSuperheavy: return f >= -0.001F && f <= 1.001F;
        default: return false;
    }
}

using PropGetTypeFn = const void*(__fastcall*)(void* self);
using PropCopyFromFn = bool(__fastcall*)(void* self, const void* srcType,
                                         const void* srcData, bool notify);

// Invokes idUIWidgetModelProperty<T>::Copy_Impl (vtbl[5], notify=true) so the
// Flash slider thumb / widget state synchronizes with the stored value instead
// of stale vanilla anchor values.
bool CallPropertyCopyFrom(void* value, const void* srcData) noexcept {
    if (value == nullptr || srcData == nullptr) {
        return false;
    }
    void* const* vtbl = nullptr;
    if (!SafeRead(value, &vtbl, sizeof(vtbl)) || vtbl == nullptr) {
        return false;
    }
    PropGetTypeFn getType = nullptr;
    PropCopyFromFn copyFrom = nullptr;
    if (!SafeRead(vtbl + 2, &getType, sizeof(getType)) ||
        !SafeRead(vtbl + 5, &copyFrom, sizeof(copyFrom)) ||
        getType == nullptr || copyFrom == nullptr) {
        return false;
    }
    const void* typeInfo = getType(value);
    if (typeInfo == nullptr) {
        return false;
    }
    return copyFrom(value, typeInfo, srcData, true);
}

const void* TryGetPropTypeInfo(void* prop) noexcept {
    if (prop == nullptr) {
        return nullptr;
    }
    void* const* vtbl = nullptr;
    if (!SafeRead(prop, &vtbl, sizeof(vtbl)) || vtbl == nullptr) {
        return nullptr;
    }
    PropGetTypeFn getType = nullptr;
    if (!SafeRead(vtbl + 2, &getType, sizeof(getType)) || getType == nullptr) {
        return nullptr;
    }
    return getType(prop);
}

struct FakeBoolProperty {
    void* const* vtbl;
    std::int64_t value;
    const void* typeInfo;
};

const void* __fastcall FakeProp_GetData(void* self) noexcept {
    return &static_cast<FakeBoolProperty*>(self)->value;
}

const void* __fastcall FakeProp_GetType(void* self) noexcept {
    return static_cast<FakeBoolProperty*>(self)->typeInfo;
}

bool __fastcall FakeProp_CopyFrom(void*, const void*, const void*, bool) noexcept {
    return true;
}

void* const kFakeBoolPropVtbl[6] = {
    nullptr,
    reinterpret_cast<void*>(&FakeProp_GetData),
    reinterpret_cast<void*>(&FakeProp_GetType),
    nullptr,
    nullptr,
    reinterpret_cast<void*>(&FakeProp_CopyFrom),
};

class ScopedModelPropSwap {
public:
    ScopedModelPropSwap(void* model, FakeBoolProperty* fake) noexcept
        : slot_(model != nullptr && fake != nullptr
                    ? reinterpret_cast<void**>(static_cast<char*>(model) + 0x430)
                    : nullptr),
          orig_(nullptr) {
        if (slot_ != nullptr) {
            if (!SafeRead(slot_, &orig_, sizeof(orig_)) || orig_ == nullptr) {
                slot_ = nullptr;
            } else {
                *slot_ = fake;
            }
        }
    }

    ~ScopedModelPropSwap() {
        if (slot_ != nullptr) {
            *slot_ = orig_;
        }
    }

    ScopedModelPropSwap(const ScopedModelPropSwap&) = delete;
    ScopedModelPropSwap& operator=(const ScopedModelPropSwap&) = delete;

private:
    void** slot_;
    void* orig_;
};

void* TryGetModelFromWidget(const void* widget) noexcept {
    if (widget == nullptr) {
        return nullptr;
    }
    const void* wrapper = nullptr;
    if (!SafeRead(static_cast<const char*>(widget) + 0x70, &wrapper, sizeof(wrapper)) ||
        wrapper == nullptr) {
        return nullptr;
    }
    const void* inner = nullptr;
    if (!SafeRead(static_cast<const char*>(wrapper) + 0x08, &inner, sizeof(inner)) ||
        inner == nullptr) {
        return nullptr;
    }
    const void* ref = nullptr;
    if (!SafeRead(static_cast<const char*>(inner) + 0x190, &ref, sizeof(ref)) ||
        ref == nullptr) {
        return nullptr;
    }
    void* model = nullptr;
    if (!SafeRead(static_cast<const char*>(ref) + 0x08, &model, sizeof(model))) {
        return nullptr;
    }
    return model;
}

bool SlotDetour_GetSettingsDataValue(void* model, void* value) {
    const int stage = g_stage.load(std::memory_order_relaxed);
    if (model != nullptr && value != nullptr) {
        if (stage >= 3) {
            const std::wstring menuId = TryClassifyModsListRow(model);
            if (!menuId.empty()) {
                std::string folder;
                bool enabledNow = false;
                if (ModsMenuLookupRow(menuId, &folder, &enabledNow)) {
                    bool desired = enabledNow;
                    ModsMenuGetDesired(folder, enabledNow, &desired);
                    const unsigned char byteVal = desired ? 1U : 0U;
                    if (CallPropertyCopyFrom(value, &byteVal)) {
                        return true;
                    }
                }
            }
        }

        if (stage >= 2) {
            const Row row = ClassifyModel(model);
            if (row != Row::None) {
                BridgeValues v{};
                if (BridgeGetValues(&v)) {
                    bool ok = false;
                    switch (row) {
                        case Row::Enabled: {
                            const unsigned char b = v.enabled ? 1U : 0U;
                            ok = CallPropertyCopyFrom(value, &b);
                            break;
                        }
                        case Row::SkipIntro: {
                            const unsigned char b = v.skipIntro ? 1U : 0U;
                            ok = CallPropertyCopyFrom(value, &b);
                            break;
                        }
                        case Row::TargetScore: {
                            const float f = static_cast<float>(v.targetScore);
                            ok = CallPropertyCopyFrom(value, &f);
                            break;
                        }
                        case Row::PctFodder: {
                            const float f = static_cast<float>(v.pctFodder) / 100.0F;
                            ok = CallPropertyCopyFrom(value, &f);
                            break;
                        }
                        case Row::PctHeavy: {
                            const float f = static_cast<float>(v.pctHeavy) / 100.0F;
                            ok = CallPropertyCopyFrom(value, &f);
                            break;
                        }
                        case Row::PctSuperheavy: {
                            const float f = static_cast<float>(v.pctSuperheavy) / 100.0F;
                            ok = CallPropertyCopyFrom(value, &f);
                            break;
                        }
                        case Row::PoweredDashDamage: {
                            const float f = static_cast<float>(v.poweredDashDamage);
                            ok = CallPropertyCopyFrom(value, &f);
                            break;
                        }
                        case Row::ParryPct: {
                            const float f = static_cast<float>(v.parryPct) / 100.0F;
                            ok = CallPropertyCopyFrom(value, &f);
                            break;
                        }
                        default: break;
                    }
                    if (ok) {
                        return true;
                    }
                }
            }
        }
    }

    return reinterpret_cast<GetSettingsDataValueFn>(g_trampolineGet)(model, value);
}

bool TryResolveRadioDesired(void* model, int stage, bool* outDesired) noexcept {
    if (model == nullptr || outDesired == nullptr) {
        return false;
    }
    if (stage >= 3) {
        const std::wstring menuId = TryClassifyModsListRow(model);
        if (!menuId.empty()) {
            std::string folder;
            bool enabledNow = false;
            if (ModsMenuLookupRow(menuId, &folder, &enabledNow)) {
                bool desired = enabledNow;
                ModsMenuGetDesired(folder, enabledNow, &desired);
                *outDesired = desired;
                return true;
            }
        }
    }
    if (stage >= 2) {
        const Row row = ClassifyModel(model);
        if (row == Row::Enabled || row == Row::SkipIntro) {
            BridgeValues v{};
            if (BridgeGetValues(&v)) {
                *outDesired = (row == Row::Enabled) ? v.enabled : v.skipIntro;
                return true;
            }
        }
    }
    return false;
}

void __fastcall SlotDetour_RadioButtonSync(void* model, void* widgetRefPtr) {
    bool desired = false;
    const int stage = g_stage.load(std::memory_order_relaxed);
    if (TryResolveRadioDesired(model, stage, &desired)) {
        void* origProp = nullptr;
        if (SafeRead(static_cast<const char*>(model) + 0x430, &origProp, sizeof(origProp)) &&
            origProp != nullptr) {
            const void* typeInfo = TryGetPropTypeInfo(origProp);
            if (typeInfo != nullptr) {
                FakeBoolProperty fake{kFakeBoolPropVtbl, desired ? 1LL : 0LL, typeInfo};
                ScopedModelPropSwap swap(model, &fake);
                reinterpret_cast<RadioButtonSyncFn>(g_trampolineRadioSync)(model, widgetRefPtr);
                return;
            }
        }
    }
    reinterpret_cast<RadioButtonSyncFn>(g_trampolineRadioSync)(model, widgetRefPtr);
}

bool CallRadioApplySwapped(void* widget, void* eventArgs, void* arg3, void* arg4,
                           void* model, bool desired) {
    void* origProp = nullptr;
    const void* typeInfo = nullptr;
    if (SafeRead(static_cast<const char*>(model) + 0x430, &origProp, sizeof(origProp)) &&
        origProp != nullptr) {
        typeInfo = TryGetPropTypeInfo(origProp);
    }
    FakeBoolProperty fake{kFakeBoolPropVtbl, desired ? 1LL : 0LL, typeInfo};
    ScopedModelPropSwap swap(model, &fake);
    return reinterpret_cast<RadioButtonApplyFn>(g_trampolineRadioApply)(
        widget, eventArgs, arg3, arg4);
}

void SyncRadioWidgetNow(void* model, void* widget, bool desired) noexcept {
    if (model == nullptr || widget == nullptr || g_trampolineRadioSync == 0) {
        return;
    }
    void* childRefBlock = nullptr;
    if (!SafeRead(static_cast<const char*>(widget) + 0x258, &childRefBlock,
                  sizeof(childRefBlock)) ||
        childRefBlock == nullptr) {
        return;
    }
    void* childWidget = nullptr;
    if (!SafeRead(static_cast<const char*>(childRefBlock) + 0x08, &childWidget,
                  sizeof(childWidget)) ||
        childWidget == nullptr || childWidget == widget) {
        return;
    }
    // Defense-in-depth: child radio widget must carry its own distinct model
    // (idUIWidgetModel_Radio_Buttons_Setting), never the parent setting model.
    void* childModel = TryGetModelFromWidget(childWidget);
    if (childModel == nullptr || childModel == model) {
        return;
    }
    void* origProp = nullptr;
    if (!SafeRead(static_cast<const char*>(model) + 0x430, &origProp, sizeof(origProp)) ||
        origProp == nullptr) {
        return;
    }
    const void* typeInfo = TryGetPropTypeInfo(origProp);
    if (typeInfo == nullptr) {
        return;
    }
    // Atomic weak-ref increment matching 0x01EB50AA..0x01EB50C2; RadioButtonSync
    // (0x01EB4BBD) always decrements weakCount by 1 and nulls localRef on return.
    auto* weakCountPtr = reinterpret_cast<volatile LONG*>(
        static_cast<char*>(childRefBlock) + 4);
    LONG oldWeak = 0;
    if (!SafeRead(static_cast<const char*>(childRefBlock) + 4, &oldWeak, sizeof(oldWeak))) {
        return;
    }
    while (oldWeak > 0) {
        const LONG prev = InterlockedCompareExchange(weakCountPtr, oldWeak + 1, oldWeak);
        if (prev == oldWeak) {
            break;
        }
        oldWeak = prev;
    }
    if (oldWeak <= 0) {
        return;
    }
    FakeBoolProperty fake{kFakeBoolPropVtbl, desired ? 1LL : 0LL, typeInfo};
    ScopedModelPropSwap swap(model, &fake);
    void* localRef = childRefBlock;
    reinterpret_cast<RadioButtonSyncFn>(g_trampolineRadioSync)(model, &localRef);
}

bool __fastcall SlotDetour_RadioButtonApply(void* widget, void* eventArgs,
                                            void* arg3, void* arg4) {
    const int stage = g_stage.load(std::memory_order_relaxed);
    std::int32_t argc = 0;
    void* arg0 = nullptr;
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (moduleBase == 0 || widget == nullptr || eventArgs == nullptr ||
        !SafeRead(static_cast<const char*>(eventArgs) + 0x10, &argc, sizeof(argc)) ||
        argc <= 0 ||
        !SafeRead(static_cast<const char*>(eventArgs) + 0x08, &arg0, sizeof(arg0)) ||
        arg0 == nullptr) {
        return reinterpret_cast<RadioButtonApplyFn>(g_trampolineRadioApply)(
            widget, eventArgs, arg3, arg4);
    }

    void* model = TryGetModelFromWidget(widget);
    if (model == nullptr) {
        return reinterpret_cast<RadioButtonApplyFn>(g_trampolineRadioApply)(
            widget, eventArgs, arg3, arg4);
    }

    const std::int32_t selectedIdx =
        reinterpret_cast<SwfVarToIntFn>(moduleBase + kSwfVarToIntRva)(arg0);
    if (selectedIdx != 0 && selectedIdx != 1) {
        return reinterpret_cast<RadioButtonApplyFn>(g_trampolineRadioApply)(
            widget, eventArgs, arg3, arg4);
    }

    const bool desired = (selectedIdx != 0);
    if (stage >= 3) {
        const std::wstring menuId = TryClassifyModsListRow(model);
        if (!menuId.empty()) {
            std::string folder;
            bool enabledNow = false;
            if (ModsMenuLookupRow(menuId, &folder, &enabledNow)) {
                const auto statePath = ModsMenuStatePath();
                if (!statePath.empty() &&
                    ModsMenuSetDesired(folder, desired, statePath)) {
                    std::wstring wide;
                    wide.reserve(folder.size());
                    for (const char c : folder) {
                        wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
                    }
                    wchar_t line[288]{};
                    swprintf_s(line, L"[modmenu] %s -> %s (aplica en el proximo arranque)",
                               wide.c_str(), desired ? L"activado" : L"desactivado");
                    LogLine(line);
                }
                if (menuId == L"hellwalkerintroskip" ||
                    menuId == L"skipintro" ||
                    menuId == L"introskip") {
                    BridgeValues v{};
                    if (BridgeGetValues(&v)) {
                        v.skipIntro = desired;
                        BridgeSetValues(v);
                    }
                }
            }
            const bool ok = CallRadioApplySwapped(widget, eventArgs, arg3, arg4, model, desired);
            SyncRadioWidgetNow(model, widget, desired);
            return ok;
        }
    }
    if (stage >= 2) {
        const Row row = ClassifyModel(model);
        if (row == Row::Enabled || row == Row::SkipIntro) {
            BridgeValues v{};
            if (BridgeGetValues(&v)) {
                if (row == Row::Enabled) {
                    v.enabled = desired;
                } else {
                    v.skipIntro = desired;
                }
                BridgeSetValues(v);
                wchar_t line[192]{};
                swprintf_s(line, L"[Hellwalker] captured %s (on RadioButton)",
                           kRowDeclNames[static_cast<std::size_t>(row) - 1U]);
                LogLine(line);
            }
            const bool ok = CallRadioApplySwapped(widget, eventArgs, arg3, arg4, model, desired);
            SyncRadioWidgetNow(model, widget, desired);
            return ok;
        }
    }
    return reinterpret_cast<RadioButtonApplyFn>(g_trampolineRadioApply)(
        widget, eventArgs, arg3, arg4);
}

bool SlotDetour_SetSettingsDataValue(void* model, void* value) {
    // Fase 3: los toggles del menú de mods NO se reenvían al juego (evita el
    // efecto colateral sobre la propiedad vanilla anclada). Se registra el
    // estado deseado y lo aplica el launcher en el próximo arranque.
    if (model != nullptr) {
        const std::wstring menuId = TryClassifyModsListRow(model);
        if (!menuId.empty()) {
            const void* data = WidgetValueData(value);
            unsigned char raw[4] = {};
            std::string folder;
            bool enabledNow = false;
            if (data != nullptr && SafeRead(data, raw, sizeof(raw)) &&
                ModsMenuLookupRow(menuId, &folder, &enabledNow)) {
                const bool desired = raw[0] != 0;
                const auto statePath = ModsMenuStatePath();
                if (!statePath.empty() && ModsMenuSetDesired(folder, desired, statePath)) {
                    std::wstring wide;
                    wide.reserve(folder.size());
                    for (const char c : folder) {
                        wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
                    }
                    wchar_t line[288]{};
                    swprintf_s(line, L"[modmenu] %s -> %s (aplica en el proximo arranque)",
                               wide.c_str(), desired ? L"activado" : L"desactivado");
                    LogLine(line);
                } else {
                    LogLine(L"[modmenu] toggle ignorado: sin estado persistible.");
                }
            } else {
                LogLine(L"[modmenu] toggle de fila desconocida ignorado.");
            }
            return true; // tragado: la propiedad anclada queda intacta
        }
    }

    const int stage = g_stage.load(std::memory_order_relaxed);
    if (stage >= 2 && model != nullptr && value != nullptr) {
        const Row row = ClassifyModel(model);
        if (row != Row::None) {
            const void* data = WidgetValueData(value);
            unsigned char raw[4] = {};
            if (data != nullptr && SafeRead(data, raw, sizeof(raw)) && RawInRange(row, raw)) {
                BridgeValues v{};
                if (BridgeGetValues(&v)) {
                    MapRawToConfig(row, raw, v);
                    BridgeSetValues(v);
                    static std::atomic<std::uint64_t> lastCaptureLog{0};
                    const std::uint64_t now = GetTickCount64();
                    std::uint64_t prev = lastCaptureLog.load(std::memory_order_relaxed);
                    if (now - prev > 2000ULL &&
                        lastCaptureLog.compare_exchange_strong(prev, now, std::memory_order_relaxed)) {
                        wchar_t line[192]{};
                        swprintf_s(line, L"[Hellwalker] captured %s (on Set)",
                                   kRowDeclNames[static_cast<std::size_t>(row) - 1U]);
                        LogLine(line);
                    }
                }
            }
            return true; // tragado: nunca mutar propiedades vanilla compartidas
        }
    }

    return reinterpret_cast<SetSettingsDataValueFn>(g_trampolineSet)(model, value);
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

int ReadStageFromDisk() noexcept {
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return 3;
    }
    std::wstring file(path, n);
    file += L"\\DOOMTheDarkAges\\SurvivalShield\\uistage.txt";
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return 3;
    }
    char text[8]{};
    DWORD read = 0;
    ReadFile(h, text, sizeof(text) - 1U, &read, nullptr);
    CloseHandle(h);
    // 0 sin captura | 1-2 captura Hellwalker | 3 además override de display
    // del menú de mods (validación en vivo pendiente).
    return std::clamp(atoi(text), 0, 3);
}

} // namespace

bool InstallUiBridge() {
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (moduleBase == 0) {
        LogLine(L"[ui-bridge] game module base unavailable.");
        return false;
    }

    struct SlotSpec {
        std::uint32_t rva;
        std::uintptr_t detour;
        const wchar_t* label;
    };
    const SlotSpec specs[kUiSlotCount] = {
        {kSlotGetRva, reinterpret_cast<std::uintptr_t>(&SlotDetour_GetSettingsDataValue), L"Get"},
        {kSlotSetRva, reinterpret_cast<std::uintptr_t>(&SlotDetour_SetSettingsDataValue), L"Set"},
        {kSlotRadioSyncRva, reinterpret_cast<std::uintptr_t>(&SlotDetour_RadioButtonSync), L"RadioSync"},
        {kSlotRadioApplyRva, reinterpret_cast<std::uintptr_t>(&SlotDetour_RadioButtonApply), L"RadioApply"},
    };

    for (std::size_t i = 0; i < kUiSlotCount; ++i) {
        const std::uintptr_t slotAddr = moduleBase + specs[i].rva;
        std::uintptr_t current = 0;
        if (!SafeRead(reinterpret_cast<void*>(slotAddr), &current, sizeof(current))) {
            LogLine(L"[ui-bridge] u5 slots unreadable; is the EXE u5-patched?");
            return false;
        }
        if (current != 0 && current != specs[i].detour) {
            wchar_t line[128]{};
            swprintf_s(line, L"[ui-bridge] unexpected non-zero %s slot; refusing.", specs[i].label);
            LogLine(line);
            return false;
        }
    }

    g_trampolineGet = moduleBase + kTrampGetRva;
    g_trampolineSet = moduleBase + kTrampSetRva;
    g_trampolineRadioSync = moduleBase + kTrampRadioSyncRva;
    g_trampolineRadioApply = moduleBase + kTrampRadioApplyRva;

    for (std::size_t i = 0; i < kUiSlotCount; ++i) {
        const std::uintptr_t slotAddr = moduleBase + specs[i].rva;
        if (!WriteSlot(slotAddr, specs[i].detour)) {
            LogLine(L"[ui-bridge] slot write failed.");
            return false;
        }
        g_uiSlotAddrs[i] = slotAddr;
        g_uiSlotValues[i] = specs[i].detour;
    }

    g_stage.store(ReadStageFromDisk(), std::memory_order_relaxed);
    wchar_t stageLine[96]{};
    swprintf_s(stageLine, L"[ui-bridge] u5 disk thunks active (4 slots), Set-capture stage %d.",
               g_stage.load());
    LogLine(stageLine);
    return true;
}

void ReassertUiSlots() noexcept {
    for (std::size_t i = 0; i < kUiSlotCount; ++i) {
        if (g_uiSlotAddrs[i] == 0) {
            continue;
        }
        std::uintptr_t current = 0;
        if (!SafeRead(reinterpret_cast<void*>(g_uiSlotAddrs[i]), &current, sizeof(current))) {
            continue;
        }
        if (current != g_uiSlotValues[i]) {
            if (!g_uiSlotReassertLogged.exchange(true, std::memory_order_relaxed)) {
                wchar_t line[192]{};
                swprintf_s(line,
                           L"[ui-bridge] slot %llu was 0x%llX (expected 0x%llX); "
                           L"re-asserting every publish tick.",
                           static_cast<unsigned long long>(i),
                           static_cast<unsigned long long>(current),
                           static_cast<unsigned long long>(g_uiSlotValues[i]));
                LogLine(line);
            }
            WriteSlot(g_uiSlotAddrs[i], g_uiSlotValues[i]);
        }
    }
}

} // namespace survival_shield
