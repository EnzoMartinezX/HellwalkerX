#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <TlHelp32.h>

#include "HudOverlay.hpp"

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

// External injector. The DOOM: The Dark Ages folder rejects any added or
// replaced DLL (verified: even a byte-identical signed copy of a system DLL
// aborts startup with exit code 53), so in-folder proxy loading is not viable.
// This loader keeps the game folder untouched and injects SurvivalShield.dll
// into the running game process with LoadLibraryW via CreateRemoteThread.
//
// IMPORTANT (anti-tamper, verified live on 2026-10-27): the game runs a
// cleanup pass shortly after startup (~8 s) that UNLOADS injected DLLs and
// clears the .mydata hook slots. Injecting at process birth gets the DLL
// evicted. The loader therefore waits past that window before the first
// injection, handles SteamAPI_RestartAppIfNecessary PID handoffs during the
// grace wait, and runs a watchdog that re-injects whenever the game is alive
// but the DLL is no longer loaded.
//
// Usage:
//   SurvivalShieldLoader.exe            wait for the game, inject, run HUD
//   SurvivalShieldLoader.exe <dll>      explicit DLL path (default: beside exe)
//   SurvivalShieldLoader.exe --no-hud   inject only, skip the HUD overlay
//   SurvivalShieldLoader.exe --hud-only run the HUD overlay alone (no wait,
//                                       no injection; for visual checks or
//                                       manual injection)

namespace {

constexpr wchar_t kGameProcessName[] = L"DOOMTheDarkAges.exe";
constexpr wchar_t kModuleName[] = L"SurvivalShield.dll";
constexpr DWORD kPollIntervalMs = 200;
constexpr DWORD kWaitTimeoutMs = 10U * 60U * 1000U;
// Anti-tamper cleanup runs ~8 s after launch; wait well past it.
constexpr DWORD kStartupGraceMs = 60U * 1000U;
constexpr DWORD kRelaunchPollWindowMs = 25U * 1000U;
constexpr DWORD kWatchdogIntervalMs = 5U * 1000U;
constexpr int kMaxReinjects = 6;

struct HandleDeleter {
    void operator()(HANDLE handle) const noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};

using UniqueHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleDeleter>;

std::mutex g_logMutex;

std::filesystem::path LoaderLogPath() {
    wchar_t localAppData[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path(std::wstring(localAppData, n)) /
           L"DOOMTheDarkAges" / L"SurvivalShield" / L"loader.log";
}

void LogLoader(const wchar_t* fmt, ...) {
    wchar_t msg[768]{};
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(msg, _TRUNCATE, fmt, args);
    va_end(args);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t line[864]{};
    swprintf_s(line, L"%04u-%02u-%02u %02u:%02u:%02u [loader] %s\r\n",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, msg);

    std::lock_guard lock(g_logMutex);
    wprintf(L"%s\n", msg);
    fflush(stdout);

    const auto logPath = LoaderLogPath();
    if (logPath.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(logPath.parent_path(), ec);
    UniqueHandle h{CreateFileW(logPath.c_str(), FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (h.get() == INVALID_HANDLE_VALUE || h.get() == nullptr) {
        return;
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
    if (needed > 1) {
        std::string utf8(static_cast<std::size_t>(needed - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8.data(), needed - 1, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(h.get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
}

DWORD FindGameProcessId() noexcept {
    UniqueHandle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (snapshot.get() == INVALID_HANDLE_VALUE) {
        return 0;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot.get(), &entry) == FALSE) {
        return 0;
    }
    do {
        if (lstrcmpiW(entry.szExeFile, kGameProcessName) == 0) {
            return entry.th32ProcessID;
        }
    } while (Process32NextW(snapshot.get(), &entry) == TRUE);
    return 0;
}

UniqueHandle OpenGameProcess(DWORD pid) noexcept {
    return UniqueHandle{OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE,
        FALSE, pid)};
}

bool IsModuleLoaded(DWORD pid) noexcept {
    UniqueHandle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)};
    if (snapshot.get() == INVALID_HANDLE_VALUE) {
        return false;
    }
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot.get(), &entry) == FALSE) {
        return false;
    }
    do {
        if (lstrcmpiW(entry.szModule, kModuleName) == 0) {
            return true;
        }
    } while (Module32NextW(snapshot.get(), &entry) == TRUE);
    return false;
}

struct WatchdogContext {
    HANDLE process = nullptr;
    std::wstring dllPath;
    DWORD pid = 0;
};

bool InjectDll(HANDLE process, DWORD pid, const std::wstring& dllPath);

DWORD WINAPI WatchdogThread(LPVOID parameter) noexcept {
    std::unique_ptr<WatchdogContext> context(static_cast<WatchdogContext*>(parameter));
    DWORD backoffMs = kWatchdogIntervalMs;
    int reinjects = 0;
    for (;;) {
        Sleep(kWatchdogIntervalMs);
        if (WaitForSingleObject(context->process, 0) == WAIT_OBJECT_0) {
            break; // game exited
        }
        if (IsModuleLoaded(context->pid)) {
            backoffMs = kWatchdogIntervalMs;
            continue;
        }
        if (reinjects >= kMaxReinjects) {
            continue;
        }
        Sleep(backoffMs);
        if (WaitForSingleObject(context->process, 0) == WAIT_OBJECT_0) {
            break;
        }
        LogLoader(L"Watchdog: SurvivalShield.dll missing from PID %lu; re-injecting (attempt %d)...",
                  static_cast<unsigned long>(context->pid), reinjects + 1);
        if (InjectDll(context->process, context->pid, context->dllPath)) {
            LogLoader(L"Watchdog: re-injection completed.");
            ++reinjects;
            backoffMs = kWatchdogIntervalMs;
        } else {
            LogLoader(L"Watchdog: re-injection failed (error %lu).",
                      static_cast<unsigned long>(GetLastError()));
            if (backoffMs < 120'000U) {
                backoffMs *= 2;
            }
        }
    }
    return 0;
}

bool InjectDll(HANDLE process, DWORD pid, const std::wstring& dllPath) {
    const std::size_t bytes = (dllPath.size() + 1U) * sizeof(wchar_t);
    void* remoteMemory = VirtualAllocEx(process, nullptr, bytes,
                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remoteMemory == nullptr) {
        LogLoader(L"VirtualAllocEx failed (error %lu).", static_cast<unsigned long>(GetLastError()));
        return false;
    }

    SIZE_T written = 0;
    const bool copied = WriteProcessMemory(process, remoteMemory, dllPath.c_str(),
                                           bytes, &written) != FALSE &&
                        written == bytes;
    if (!copied) {
        LogLoader(L"WriteProcessMemory failed (error %lu).", static_cast<unsigned long>(GetLastError()));
        VirtualFreeEx(process, remoteMemory, 0, MEM_RELEASE);
        return false;
    }

    const auto loadLibraryW = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW")));
    if (loadLibraryW == nullptr) {
        VirtualFreeEx(process, remoteMemory, 0, MEM_RELEASE);
        return false;
    }

    UniqueHandle thread{CreateRemoteThread(process, nullptr, 0, loadLibraryW,
                                           remoteMemory, 0, nullptr)};
    if (thread.get() == nullptr) {
        LogLoader(L"CreateRemoteThread failed (error %lu).", static_cast<unsigned long>(GetLastError()));
        VirtualFreeEx(process, remoteMemory, 0, MEM_RELEASE);
        return false;
    }

    const DWORD waitRes = WaitForSingleObject(thread.get(), 30'000);
    if (waitRes != WAIT_OBJECT_0) {
        LogLoader(L"WaitForSingleObject on LoadLibraryW thread returned %lu.",
                  static_cast<unsigned long>(waitRes));
        VirtualFreeEx(process, remoteMemory, 0, MEM_RELEASE);
        return false;
    }

    DWORD remoteExitCode = 0;
    GetExitCodeThread(thread.get(), &remoteExitCode);
    VirtualFreeEx(process, remoteMemory, 0, MEM_RELEASE);
    // LoadLibraryW returns a 64-bit HMODULE; Wait+IsModuleLoaded guards against
    // low-32-bit zero truncation if the DLL maps at a 4 GB boundary.
    const bool loaded = IsModuleLoaded(pid);
    LogLoader(L"LoadLibraryW remoteExit=0x%08lX loaded=%s on PID %lu.",
              static_cast<unsigned long>(remoteExitCode),
              loaded ? L"true" : L"false",
              static_cast<unsigned long>(pid));
    return remoteExitCode != 0 || loaded;
}

bool WaitForStartupGrace(UniqueHandle& process, DWORD& pid) {
    DWORD elapsed = 0;
    constexpr DWORD kStepMs = 1000;
    while (elapsed < kStartupGraceMs) {
        if (WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) {
            LogLoader(L"Game PID %lu exited during grace wait; polling %lu s for Steam relaunch...",
                      static_cast<unsigned long>(pid),
                      static_cast<unsigned long>(kRelaunchPollWindowMs / 1000U));
            DWORD newPid = 0;
            const DWORD relaunchStart = GetTickCount();
            while (GetTickCount() - relaunchStart < kRelaunchPollWindowMs) {
                Sleep(500);
                const DWORD candidate = FindGameProcessId();
                if (candidate != 0 && candidate != pid) {
                    newPid = candidate;
                    break;
                }
            }
            if (newPid == 0) {
                LogLoader(L"Game did not relaunch within %lu s; exiting loader.",
                          static_cast<unsigned long>(kRelaunchPollWindowMs / 1000U));
                return false;
            }
            UniqueHandle nextProcess = OpenGameProcess(newPid);
            if (nextProcess.get() == nullptr) {
                LogLoader(L"OpenProcess failed on relaunched PID %lu (error %lu).",
                          static_cast<unsigned long>(newPid),
                          static_cast<unsigned long>(GetLastError()));
                return false;
            }
            process = std::move(nextProcess);
            pid = newPid;
            elapsed = 0;
            LogLoader(L"Attached to relaunched PID %lu; resetting %lu s anti-tamper wait (please wait before changing Mods menu).",
                      static_cast<unsigned long>(pid),
                      static_cast<unsigned long>(kStartupGraceMs / 1000U));
            continue;
        }
        Sleep(kStepMs);
        elapsed += kStepMs;
    }
    return WaitForSingleObject(process.get(), 0) != WAIT_OBJECT_0;
}

std::filesystem::path DefaultDllPath() {
    wchar_t selfPath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, selfPath,
                                            static_cast<DWORD>(sizeof(selfPath) / sizeof(selfPath[0])));
    if (length == 0 || length >= static_cast<DWORD>(sizeof(selfPath) / sizeof(selfPath[0]))) {
        return {};
    }
    std::filesystem::path dll(std::filesystem::path(selfPath).parent_path() / L"SurvivalShield.dll");
    return std::filesystem::absolute(dll);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::filesystem::path dllPath;
    bool runOverlay = true;
    bool overlayOnly = false;
    for (int i = 1; i < argc; ++i) {
        if (lstrcmpiW(argv[i], L"--no-hud") == 0) {
            runOverlay = false;
        } else if (lstrcmpiW(argv[i], L"--hud-only") == 0) {
            overlayOnly = true;
        } else {
            dllPath = std::filesystem::absolute(std::filesystem::path(argv[i]));
        }
    }
    if (overlayOnly) {
        wprintf(L"Hellwalker HUD overlay (standalone). Close this console to stop it.\n");
        return survival_shield::RunHudOverlay(nullptr);
    }
    if (dllPath.empty()) {
        dllPath = DefaultDllPath();
    }

    if (dllPath.empty() || !std::filesystem::exists(dllPath)) {
        LogLoader(L"DLL not found: %s", dllPath.c_str());
        return 2;
    }
    LogLoader(L"SurvivalShield loader starting. DLL: %s", dllPath.c_str());

    const DWORD startTick = GetTickCount();
    DWORD pid = FindGameProcessId();
    if (pid == 0) {
        LogLoader(L"Waiting for %s...", kGameProcessName);
        while (pid == 0) {
            if (GetTickCount() - startTick > kWaitTimeoutMs) {
                LogLoader(L"Timed out waiting for the game process.");
                return 3;
            }
            Sleep(kPollIntervalMs);
            pid = FindGameProcessId();
        }
    }
    LogLoader(L"Found game process PID %lu; opening handle...", static_cast<unsigned long>(pid));

    UniqueHandle process = OpenGameProcess(pid);
    if (process.get() == nullptr) {
        LogLoader(L"OpenProcess failed on PID %lu (error %lu).",
                  static_cast<unsigned long>(pid),
                  static_cast<unsigned long>(GetLastError()));
        return 4;
    }

    LogLoader(L"Waiting %lu s for anti-tamper startup checks (wait before changing Mods menu)...",
              static_cast<unsigned long>(kStartupGraceMs / 1000U));
    if (!WaitForStartupGrace(process, pid)) {
        return 0;
    }

    if (!InjectDll(process.get(), pid, dllPath.wstring())) {
        LogLoader(L"Injection failed on PID %lu (error %lu).",
                  static_cast<unsigned long>(pid),
                  static_cast<unsigned long>(GetLastError()));
        return 5;
    }

    LogLoader(L"Injection READY on PID %lu! Safe to open Extras -> Mods. Check survivalshield.log.",
              static_cast<unsigned long>(pid));

    auto* watchdog = new WatchdogContext{process.get(), dllPath.wstring(), pid};
    UniqueHandle watchdogThread{
        CreateThread(nullptr, 0, WatchdogThread, watchdog, 0, nullptr)};
    if (watchdogThread.get() == nullptr) {
        delete watchdog;
    }

    if (!runOverlay) {
        return 0;
    }
    LogLoader(L"HUD overlay running. Close the game or this console to stop it.");
    const int overlayRc = survival_shield::RunHudOverlay(process.get());
    LogLoader(L"Loader exiting (overlay rc=%d).", overlayRc);
    return overlayRc;
}
