#pragma once

#include <string>

namespace survival_shield {

struct BuildCheckResult {
    bool supported = false;
    std::wstring sha256;
};

BuildCheckResult CheckCurrentExecutable();

} // namespace survival_shield
