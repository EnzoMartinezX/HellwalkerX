#pragma once

namespace survival_shield {

// Installs the Hellwalker settings-menu bridge: diverts Get/SetSettingsDataValue
// for the Hellwalker rows, serves stored values into the widgets, and a
// menu-gated poller captures edits into our JSON store while restoring the
// inert anchor storage so the engine never applies or saves Hellwalker values
// as real settings. Returns false (installing nothing) on any hook failure.
bool InstallUiBridge();

// Re-asserts the UI bridge .mydata slots when cleared externally.
void ReassertUiSlots() noexcept;

} // namespace survival_shield
