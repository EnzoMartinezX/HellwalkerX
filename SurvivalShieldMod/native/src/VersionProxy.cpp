#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winver.h>

#include <array>
#include <string>

// The proxy must export every VERSION.DLL name any module in the process can
// resolve at load time: the game EXE and idTechLauncher.exe import the A-set
// (the launcher also GetFileVersionInfoSizeW), and streamline/production/
// sl.dlss_g.dll imports the W/Ex-set. A missing name aborts the loader before
// the game starts, so all documented version.dll exports are forwarded.

namespace {

// The proxy is loaded by every process that imports VERSION.DLL from this
// folder (game, idTechLauncher, crash reporter helpers). Only the game itself
// should ever load the plugin; in other hosts the forwarding must be a
// side-effect-free passthrough or the launcher wedges at startup.
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

using GetFileVersionInfoSizeAProc = decltype(&::GetFileVersionInfoSizeA);
using GetFileVersionInfoSizeWProc = decltype(&::GetFileVersionInfoSizeW);
using GetFileVersionInfoSizeExAProc = decltype(&::GetFileVersionInfoSizeExA);
using GetFileVersionInfoSizeExWProc = decltype(&::GetFileVersionInfoSizeExW);
using GetFileVersionInfoAProc = decltype(&::GetFileVersionInfoA);
using GetFileVersionInfoWProc = decltype(&::GetFileVersionInfoW);
using GetFileVersionInfoExAProc = decltype(&::GetFileVersionInfoExA);
using GetFileVersionInfoExWProc = decltype(&::GetFileVersionInfoExW);
using VerQueryValueAProc = decltype(&::VerQueryValueA);
using VerQueryValueWProc = decltype(&::VerQueryValueW);

HMODULE g_proxyModule = nullptr;
HMODULE g_systemVersionModule = nullptr;
HMODULE g_pluginModule = nullptr;
INIT_ONCE g_systemVersionInitOnce = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK LoadSystemVersionDllOnce(PINIT_ONCE, PVOID, PVOID*) noexcept {
    g_systemVersionModule = LoadLibraryExW(L"version.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (g_systemVersionModule == nullptr) {
        std::array<wchar_t, MAX_PATH> systemDirectory{};
        const UINT length = GetSystemDirectoryW(systemDirectory.data(), static_cast<UINT>(systemDirectory.size()));
        if (length == 0 || length >= static_cast<UINT>(systemDirectory.size())) {
            return FALSE;
        }

        std::wstring systemPath(systemDirectory.data(), length);
        systemPath += L"\\version.dll";
        g_systemVersionModule = LoadLibraryExW(systemPath.c_str(), nullptr, 0);
    }
    return g_systemVersionModule != nullptr ? TRUE : FALSE;
}

bool LoadSystemVersionDll() noexcept {
    return InitOnceExecuteOnce(&g_systemVersionInitOnce, LoadSystemVersionDllOnce, nullptr, nullptr) != FALSE &&
           g_systemVersionModule != nullptr;
}

void* SystemVersionProc(const char* name) noexcept {
    if (!LoadSystemVersionDll()) {
        return nullptr;
    }
    return reinterpret_cast<void*>(GetProcAddress(g_systemVersionModule, name));
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

extern "C" DWORD WINAPI Proxy_GetFileVersionInfoSizeA(LPCSTR fileName, LPDWORD handle) {
    const auto proc = reinterpret_cast<GetFileVersionInfoSizeAProc>(
        SystemVersionProc("GetFileVersionInfoSizeA"));
    if (proc == nullptr) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return 0;
    }
    return proc(fileName, handle);
}

extern "C" DWORD WINAPI Proxy_GetFileVersionInfoSizeW(LPCWSTR fileName, LPDWORD handle) {
    const auto proc = reinterpret_cast<GetFileVersionInfoSizeWProc>(
        SystemVersionProc("GetFileVersionInfoSizeW"));
    if (proc == nullptr) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return 0;
    }
    return proc(fileName, handle);
}

extern "C" DWORD WINAPI Proxy_GetFileVersionInfoSizeExA(DWORD flags, LPCSTR fileName, LPDWORD handle) {
    const auto proc = reinterpret_cast<GetFileVersionInfoSizeExAProc>(
        SystemVersionProc("GetFileVersionInfoSizeExA"));
    if (proc == nullptr) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return 0;
    }
    return proc(flags, fileName, handle);
}

extern "C" DWORD WINAPI Proxy_GetFileVersionInfoSizeExW(DWORD flags, LPCWSTR fileName, LPDWORD handle) {
    const auto proc = reinterpret_cast<GetFileVersionInfoSizeExWProc>(
        SystemVersionProc("GetFileVersionInfoSizeExW"));
    if (proc == nullptr) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return 0;
    }
    return proc(flags, fileName, handle);
}

extern "C" BOOL WINAPI Proxy_GetFileVersionInfoA(
    LPCSTR fileName, DWORD handle, DWORD length, LPVOID data) {
    const auto proc = reinterpret_cast<GetFileVersionInfoAProc>(
        SystemVersionProc("GetFileVersionInfoA"));
    if (proc == nullptr) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return FALSE;
    }
    return proc(fileName, handle, length, data);
}

extern "C" BOOL WINAPI Proxy_GetFileVersionInfoW(
    LPCWSTR fileName, DWORD handle, DWORD length, LPVOID data) {
    const auto proc = reinterpret_cast<GetFileVersionInfoWProc>(
        SystemVersionProc("GetFileVersionInfoW"));
    if (proc == nullptr) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return FALSE;
    }
    return proc(fileName, handle, length, data);
}

extern "C" BOOL WINAPI Proxy_GetFileVersionInfoExA(
    DWORD flags, LPCSTR fileName, DWORD handle, DWORD length, LPVOID data) {
    const auto proc = reinterpret_cast<GetFileVersionInfoExAProc>(
        SystemVersionProc("GetFileVersionInfoExA"));
    if (proc == nullptr) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return FALSE;
    }
    return proc(flags, fileName, handle, length, data);
}

extern "C" BOOL WINAPI Proxy_GetFileVersionInfoExW(
    DWORD flags, LPCWSTR fileName, DWORD handle, DWORD length, LPVOID data) {
    const auto proc = reinterpret_cast<GetFileVersionInfoExWProc>(
        SystemVersionProc("GetFileVersionInfoExW"));
    if (proc == nullptr) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return FALSE;
    }
    return proc(flags, fileName, handle, length, data);
}

extern "C" BOOL WINAPI Proxy_VerQueryValueA(
    LPCVOID block, LPCSTR subBlock, LPVOID* buffer, PUINT length) {
    const auto proc = reinterpret_cast<VerQueryValueAProc>(
        SystemVersionProc("VerQueryValueA"));
    if (proc == nullptr) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return FALSE;
    }
    return proc(block, subBlock, buffer, length);
}

extern "C" BOOL WINAPI Proxy_VerQueryValueW(
    LPCVOID block, LPCWSTR subBlock, LPVOID* buffer, PUINT length) {
    const auto proc = reinterpret_cast<VerQueryValueWProc>(
        SystemVersionProc("VerQueryValueW"));
    if (proc == nullptr) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return FALSE;
    }
    return proc(block, subBlock, buffer, length);
}
