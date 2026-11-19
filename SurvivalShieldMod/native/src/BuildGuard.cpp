#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>

#include "BuildGuard.hpp"

#include <array>
#include <vector>

namespace survival_shield {
namespace {

// Update 5 (2026-11-12) Atlan-patched + Hellwalker unified disk thunks
// (native/tools/patch_u5.py). Strict fail-closed guard: reject any other EXE.
constexpr wchar_t kSupportedU5ModSha256[] = L"D6D87BC0416B5B872E6E8383081C906BBEC9F88028E0485E360AE94BAA299523";

struct AlgorithmHandle {
    BCRYPT_ALG_HANDLE value = nullptr;
    ~AlgorithmHandle() {
        if (value != nullptr) {
            BCryptCloseAlgorithmProvider(value, 0);
        }
    }
};

struct HashHandle {
    BCRYPT_HASH_HANDLE value = nullptr;
    ~HashHandle() {
        if (value != nullptr) {
            BCryptDestroyHash(value);
        }
    }
};

struct FileHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~FileHandle() {
        if (value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
        }
    }
};

std::wstring ToHex(const std::vector<UCHAR>& bytes) {
    static constexpr wchar_t digits[] = L"0123456789ABCDEF";
    std::wstring output;
    output.reserve(bytes.size() * 2U);
    for (const UCHAR byte : bytes) {
        output.push_back(digits[(byte >> 4U) & 0x0fU]);
        output.push_back(digits[byte & 0x0fU]);
    }
    return output;
}

} // namespace

BuildCheckResult CheckCurrentExecutable() {
    std::array<wchar_t, 32768> imagePath{};
    const DWORD pathLength = GetModuleFileNameW(nullptr, imagePath.data(), static_cast<DWORD>(imagePath.size()));
    if (pathLength == 0 || pathLength >= static_cast<DWORD>(imagePath.size())) {
        return {};
    }

    FileHandle file{CreateFileW(imagePath.data(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) {
        return {};
    }

    AlgorithmHandle algorithm;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        return {};
    }

    DWORD objectLength = 0;
    DWORD hashLength = 0;
    DWORD resultLength = 0;
    if (!BCRYPT_SUCCESS(BCryptGetProperty(algorithm.value, BCRYPT_OBJECT_LENGTH,
                                           reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
                                           &resultLength, 0)) ||
        !BCRYPT_SUCCESS(BCryptGetProperty(algorithm.value, BCRYPT_HASH_LENGTH,
                                           reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength),
                                           &resultLength, 0)) || hashLength == 0) {
        return {};
    }

    std::vector<UCHAR> hashObject(objectLength);
    std::vector<UCHAR> hash(hashLength);
    HashHandle hashHandle;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm.value, &hashHandle.value,
                                         hashObject.data(), objectLength, nullptr, 0, 0))) {
        return {};
    }

    std::array<UCHAR, 1U << 16U> buffer{};
    for (;;) {
        DWORD bytesRead = 0;
        if (ReadFile(file.value, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) == FALSE) {
            return {};
        }
        if (bytesRead == 0) {
            break;
        }
        if (!BCRYPT_SUCCESS(BCryptHashData(hashHandle.value, buffer.data(), bytesRead, 0))) {
            return {};
        }
    }

    if (!BCRYPT_SUCCESS(BCryptFinishHash(hashHandle.value, hash.data(), hashLength, 0))) {
        return {};
    }

    BuildCheckResult result;
    result.sha256 = ToHex(hash);
    result.supported = (result.sha256 == kSupportedU5ModSha256);
    return result;
}

} // namespace survival_shield
