#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace survival_shield {

// Gothic counter HUD overlay, per the user's reference mockup: dark stone
// plate with a red edge and glow at the top-right of the screen, red inverted
// cross behind it, ivory digits zero-padded to the target's digit form
// (3/4/5). Runs a message loop on the calling thread (the loader, after
// injection) until the game process exits or the window is destroyed. Reads
// the shared section published by the injected DLL (see HudShared.hpp); with
// no live heartbeat it dims itself. Position, scale and extras are read from
// hud.ini next to settings.json and hot reload every second. gameProcess may
// be null: the overlay then simply stays until its window is closed or the
// process is killed.
int RunHudOverlay(HANDLE gameProcess);

#ifdef SURVIVALSHIELD_HUD_PREVIEW
// Test hook: renders the plate forms and the automatic top-right anchor to
// PNG files with the same drawing code the live window uses, without opening
// a window. argv: <outDir> [fontDir] [scale]. Returns 0 on success.
int RunHudPreview(int argc, wchar_t** argv);
#endif

} // namespace survival_shield
