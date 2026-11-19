#pragma once

#include <filesystem>
#include <string_view>

namespace survival_shield {

// Points the process-wide logger at <directory>\survivalshield.log. An empty
// directory disables logging. Call once during startup before any LogLine.
void InitializeLog(const std::filesystem::path& directory);

// Appends a timestamped UTF-8 line. No-op when logging is not initialized.
void LogLine(std::wstring_view message);

} // namespace survival_shield
