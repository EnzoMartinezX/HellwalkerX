#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <string>

// Injection vector: only DOOMTheDarkAges.exe statically imports XINPUT1_4.dll
// from this folder (idTechLauncher.exe imports XINPUTUAP.dll instead and no
// other bundled module imports it), so this proxy is loaded exclusively by the
// game process. A version.dll proxy cannot be used here: its presence in the
// folder wedges idTechLauncher.exe at startup.
//
// The export table mirrors the real System32 xinput1_4.dll: 8 named functions
// and 7 NONAME ordinals (100..104, 108, 109). The NONAME entries are forwarded
// through 4-integer-argument stubs, which on x64 pass any WINAPI function with
// up to four register arguments through unchanged.

namespace {

using XInputGetStateProc = DWORD(WINAPI*)(DWORD, void*);
using XInputSetStateProc = DWORD(WINAPI*)(DWORD, const void*);
using XInputGetCapabilitiesProc = DWORD(WINAPI*)(DWORD, DWORD, void*);
using XInputEnableProc = void(WINAPI*)(BOOL);
using XInputGetBatteryInformationProc = DWORD(WINAPI*)(DWORD, BYTE, void*);
using XInputGetKeystrokeProc = DWORD(WINAPI*)(DWORD, DWORD, void*);
using XInputGetAudioDeviceIdsProc = DWORD(WINAPI*)(DWORD, LPWSTR, UINT*, LPWSTR, UINT*);
using OrdinalProc = DWORD(WINAPI*)(DWORD_PTR, DWORD_PTR, DWORD_PTR, DWORD_PTR);

HMODULE g_proxyModule = nullptr;
HMODULE g_systemXInputModule = nullptr;
HMODULE g_pluginModule = nullptr;
INIT_ONCE g_systemXInputInitOnce = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK LoadSystemXInputOnce(PINIT_ONCE, PVOID, PVOID*) noexcept {
    g_systemXInputModule = LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return g_systemXInputModule != nullptr ? TRUE : FALSE;
}

OrdinalProc SystemXInputProc(LPCSTR name) noexcept {
    if (InitOnceExecuteOnce(&g_systemXInputInitOnce, LoadSystemXInputOnce, nullptr, nullptr) == FALSE ||
        g_systemXInputModule == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<OrdinalProc>(GetProcAddress(g_systemXInputModule, name));
}

OrdinalProc SystemXInputOrdinal(WORD ordinal) noexcept {
    if (InitOnceExecuteOnce(&g_systemXInputInitOnce, LoadSystemXInputOnce, nullptr, nullptr) == FALSE ||
        g_systemXInputModule == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<OrdinalProc>(GetProcAddress(g_systemXInputModule, MAKEINTRESOURCEA(ordinal)));
}

bool HostIsGameProcess() noexcept {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
    if (length == 0 || length >= static_cast<DWORD>(sizeof(path) / sizeof(path[0]))) {
        return false;
    }
    const wchar_t* name = path;
    for (const wchar_t* scan = path; *scan != L'\0'; ++scan) {
        if (*scan == L'\\' || *scan == L'/') {
            name = scan + 1;
        }
    }
    return lstrcmpiW(name, L"DOOMTheDarkAges.exe") == 0;
}

void LoadAndInitializePlugin() noexcept {
    if (g_pluginModule != nullptr || g_proxyModule == nullptr) {
        return;
    }

    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(g_proxyModule, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= static_cast<DWORD>(path.size())) {
        return;
    }

    std::wstring pluginPath(path.data(), length);
    const std::size_t separator = pluginPath.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        return;
    }
    pluginPath.resize(separator + 1U);
    pluginPath += L"SurvivalShield.dll";

    g_pluginModule = LoadLibraryExW(pluginPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (g_pluginModule == nullptr) {
        return;
    }

    using InitializeProc = BOOL(WINAPI*)(HMODULE);
    const auto initialize = reinterpret_cast<InitializeProc>(
        GetProcAddress(g_pluginModule, "SurvivalShield_Initialize"));
    if (initialize == nullptr || initialize(g_proxyModule) == FALSE) {
        FreeLibrary(g_pluginModule);
        g_pluginModule = nullptr;
    }
}

DWORD WINAPI InitializeWorker(void*) noexcept {
    if (HostIsGameProcess()) {
        LoadAndInitializePlugin();
    }
    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_proxyModule = instance;
        DisableThreadLibraryCalls(instance);
        // Queue initialization so the plugin is loaded after this DllMain returns.
        QueueUserWorkItem(InitializeWorker, nullptr, WT_EXECUTEDEFAULT);
    }
    return TRUE;
}

// The real xinput1_4.dll exports a DllMain by name at ordinal 1; nothing
// imports it, but the table is mirrored for fidelity.
extern "C" BOOL WINAPI Proxy_XInputDllMain(HINSTANCE, DWORD, LPVOID) noexcept {
    return TRUE;
}

extern "C" DWORD WINAPI XInputGetState(DWORD userIndex, void* state) {
    const auto proc = reinterpret_cast<XInputGetStateProc>(SystemXInputProc("XInputGetState"));
    if (proc == nullptr) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return ERROR_NOT_SUPPORTED;
    }
    return proc(userIndex, state);
}

extern "C" DWORD WINAPI XInputSetState(DWORD userIndex, const void* vibration) {
    const auto proc = reinterpret_cast<XInputSetStateProc>(SystemXInputProc("XInputSetState"));
    if (proc == nullptr) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return ERROR_NOT_SUPPORTED;
    }
    return proc(userIndex, vibration);
}

extern "C" DWORD WINAPI XInputGetCapabilities(DWORD userIndex, DWORD flags, void* capabilities) {
    const auto proc = reinterpret_cast<XInputGetCapabilitiesProc>(SystemXInputProc("XInputGetCapabilities"));
    if (proc == nullptr) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return ERROR_NOT_SUPPORTED;
    }
    return proc(userIndex, flags, capabilities);
}

extern "C" void WINAPI XInputEnable(BOOL enable) {
    const auto proc = reinterpret_cast<XInputEnableProc>(SystemXInputProc("XInputEnable"));
    if (proc != nullptr) {
        proc(enable);
    }
}

extern "C" DWORD WINAPI XInputGetBatteryInformation(DWORD userIndex, BYTE devType, void* batteryInformation) {
    const auto proc = reinterpret_cast<XInputGetBatteryInformationProc>(
        SystemXInputProc("XInputGetBatteryInformation"));
    if (proc == nullptr) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return ERROR_NOT_SUPPORTED;
    }
    return proc(userIndex, devType, batteryInformation);
}

extern "C" DWORD WINAPI XInputGetKeystroke(DWORD userIndex, DWORD reserved, void* keystroke) {
    const auto proc = reinterpret_cast<XInputGetKeystrokeProc>(SystemXInputProc("XInputGetKeystroke"));
    if (proc == nullptr) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return ERROR_NOT_SUPPORTED;
    }
    return proc(userIndex, reserved, keystroke);
}

extern "C" DWORD WINAPI XInputGetAudioDeviceIds(
    DWORD userIndex, LPWSTR renderDeviceId, UINT* renderCount, LPWSTR captureDeviceId, UINT* captureCount) {
    const auto proc = reinterpret_cast<XInputGetAudioDeviceIdsProc>(
        SystemXInputProc("XInputGetAudioDeviceIds"));
    if (proc == nullptr) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return ERROR_NOT_SUPPORTED;
    }
    return proc(userIndex, renderDeviceId, renderCount, captureDeviceId, captureCount);
}

// NONAME ordinals. All known xinput internals take at most four integer
// register arguments, so a four-argument tail call forwards them regardless
// of their real signature.
extern "C" DWORD WINAPI XInputOrdinal100(DWORD_PTR a, DWORD_PTR b, DWORD_PTR c, DWORD_PTR d) {
    const auto proc = SystemXInputOrdinal(100);
    return proc != nullptr ? proc(a, b, c, d) : ERROR_NOT_SUPPORTED;
}

extern "C" DWORD WINAPI XInputOrdinal101(DWORD_PTR a, DWORD_PTR b, DWORD_PTR c, DWORD_PTR d) {
    const auto proc = SystemXInputOrdinal(101);
    return proc != nullptr ? proc(a, b, c, d) : ERROR_NOT_SUPPORTED;
}

extern "C" DWORD WINAPI XInputOrdinal102(DWORD_PTR a, DWORD_PTR b, DWORD_PTR c, DWORD_PTR d) {
    const auto proc = SystemXInputOrdinal(102);
    return proc != nullptr ? proc(a, b, c, d) : ERROR_NOT_SUPPORTED;
}

extern "C" DWORD WINAPI XInputOrdinal103(DWORD_PTR a, DWORD_PTR b, DWORD_PTR c, DWORD_PTR d) {
    const auto proc = SystemXInputOrdinal(103);
    return proc != nullptr ? proc(a, b, c, d) : ERROR_NOT_SUPPORTED;
}

extern "C" DWORD WINAPI XInputOrdinal104(DWORD_PTR a, DWORD_PTR b, DWORD_PTR c, DWORD_PTR d) {
    const auto proc = SystemXInputOrdinal(104);
    return proc != nullptr ? proc(a, b, c, d) : ERROR_NOT_SUPPORTED;
}

extern "C" DWORD WINAPI XInputOrdinal108(DWORD_PTR a, DWORD_PTR b, DWORD_PTR c, DWORD_PTR d) {
    const auto proc = SystemXInputOrdinal(108);
    return proc != nullptr ? proc(a, b, c, d) : ERROR_NOT_SUPPORTED;
}

extern "C" DWORD WINAPI XInputOrdinal109(DWORD_PTR a, DWORD_PTR b, DWORD_PTR c, DWORD_PTR d) {
    const auto proc = SystemXInputOrdinal(109);
    return proc != nullptr ? proc(a, b, c, d) : ERROR_NOT_SUPPORTED;
}
