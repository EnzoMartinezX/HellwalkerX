#include "Log.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <mutex>
#include <string>
#include <system_error>

namespace survival_shield {
namespace {

constexpr std::size_t kMaxMessageChars = 400;

std::mutex g_logMutex;
std::filesystem::path g_logDirectory;

void AppendLine(const std::wstring& message) {
    std::error_code error;
    std::filesystem::create_directories(g_logDirectory, error);
    if (error) {
        return;
    }

    const auto path = g_logDirectory / L"survivalshield.log";
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t line[512]{};
    const int length = swprintf_s(line, sizeof(line) / sizeof(line[0]),
                                  L"%04u-%02u-%02u %02u:%02u:%02u %.*s\r\n",
                                  now.wYear, now.wMonth, now.wDay,
                                  now.wHour, now.wMinute, now.wSecond,
                                  static_cast<unsigned>(message.size()), message.data());
    if (length > 0) {
        int byteCount = WideCharToMultiByte(CP_UTF8, 0, line, length, nullptr, 0, nullptr, nullptr);
        if (byteCount > 0) {
            std::string utf8(static_cast<std::size_t>(byteCount), '\0');
            WideCharToMultiByte(CP_UTF8, 0, line, length, utf8.data(), byteCount, nullptr, nullptr);
            DWORD written = 0;
            WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        }
    }
    CloseHandle(file);
}

} // namespace

void InitializeLog(const std::filesystem::path& directory) {
    std::lock_guard lock(g_logMutex);
    g_logDirectory = directory;
}

void LogLine(std::wstring_view message) {
    try {
        std::lock_guard lock(g_logMutex);
        if (g_logDirectory.empty() || message.empty()) {
            return;
        }
        AppendLine(std::wstring(message.substr(0, kMaxMessageChars)));
    } catch (...) {
    }
}

} // namespace survival_shield
