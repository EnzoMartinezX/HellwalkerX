// Entry point for the offscreen HUD preview tool. It reuses HudOverlay.cpp's
// drawing code (compiled with SURVIVALSHIELD_HUD_PREVIEW) to write PNGs of
// the counter forms, so the design can be reviewed without the game.
#include "HudOverlay.hpp"

#include <shellapi.h>
#include <memory>

int main(int argc, char**) {
    int wargc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv == nullptr) {
        return 2;
    }
    const int rc = survival_shield::RunHudPreview(wargc, wargv);
    LocalFree(wargv);
    return rc;
}
