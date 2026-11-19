#include "HudOverlay.hpp"
#include "HudShared.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
// WIN32_LEAN_AND_MEAN strips the COM base types gdiplus.h needs (IStream,
// IUnknown, PROPID); pull them in explicitly before the GDI+ headers.
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

// Gothic counter, drawn per the user's reference screenshot ("Contador
// gotico sobre ruinas infernales"): a dark stone plate with pointed ends and
// a red edge + glow, anchored top-right of the screen, with a red inverted
// cross rising from behind it. The score renders as ivory engraved digits,
// zero-padded to the digit count of the player's target (3/4/5 figures), so
// the plate stretches exactly like the three width forms mocked up in the
// reference. Once the charge is ready the digits run burgundy->LED red with a
// slow pulse and the glow intensifies.

namespace survival_shield {
namespace {

using Gdiplus::REAL;

constexpr wchar_t kOverlayClass[] = L"HellwalkerHudOverlay";
constexpr UINT kRepaintTimerId = 1;
constexpr UINT kRepaintMs = 33;      // ~30 fps
constexpr UINT kIniReloadMs = 1000;

// Kill-flash timing, per the user's spec: the counter only shows when a demon
// is killed, holds steady, then fades away — 4 seconds in total — so it never
// sits over map loads or the game's own animations.
constexpr ULONGLONG kKillHoldMs = 2000;
constexpr ULONGLONG kKillFadeMs = 2000;

// Reference geometry is measured on a 1080p screen; everything scales by
// (screen height / 1080) so the counter keeps the mockup's proportions on any
// resolution, times the user's hud.ini multiplier.
constexpr double kDesignHeight = 1080.0;

struct OverlayConfig {
    int x = -1;             // top-left of the window; -1 = auto (top-right, below gold/ruby/life)
    int y = -1;
    double scale = 1.0;     // 0.6 .. 2.0, multiplied by screenH/1080
    bool showProgress = true;
    bool alwaysShow = false; // test hook: skip the kill-flash gating
};

struct FrameCellSpec {
    int x0;
    int x1;
};

struct FrameSpec {
    int digits;
    const wchar_t* fileName;
    int srcW;
    int srcH;
    int cellY0;
    int cellY1;
    FrameCellSpec cells[5];
};

// Exact pixel coordinates measured from the 3 cleaned demonic frames
// (hud_frame_3.png = 600x190, hud_frame_4.png = 632x183, hud_frame_5.png = 700x199).
constexpr FrameSpec kFrameSpecs[3] = {
    {3, L"hud_frame_3.png", 600, 190, 58, 153, {{166, 249}, {259, 342}, {353, 435}, {0, 0}, {0, 0}}},
    {4, L"hud_frame_4.png", 632, 183, 55, 146, {{154, 227}, {236, 313}, {322, 399}, {409, 479}, {0, 0}}},
    {5, L"hud_frame_5.png", 700, 199, 59, 145, {{153, 228}, {236, 307}, {315, 388}, {396, 467}, {476, 548}}},
};

// Base scale factor mapping the 600..700px source artwork onto a 1080p screen
// (~240..280px wide at scale=1.0, compact enough to sit below the top-right HUD).
constexpr double kArtworkBaseScale = 0.40;

const FrameSpec& GetFrameSpec(int digits) {
    const int clamped = std::clamp(digits, 3, 5);
    return kFrameSpecs[clamped - 3];
}

struct HudFonts {
    Gdiplus::PrivateFontCollection collection;
    std::unique_ptr<Gdiplus::FontFamily> digit;   // Cinzel, else fallbacks
    std::unique_ptr<Gdiplus::Font> digitFont;     // rebuilt when scale changes
    std::unique_ptr<Gdiplus::Font> captionFont;
    std::unique_ptr<Gdiplus::Bitmap> frames[3];   // 3, 4, 5-digit demonic frames
    double builtForScale = -1.0;
    bool custom = false;
};

struct OverlayState {
    HANDLE gameProcess = nullptr;
    OverlayConfig config;
    std::filesystem::path iniPath;
    ULONGLONG iniLastChecked = 0;
    HANDLE mapping = nullptr;
    HudSharedState* shared = nullptr;
    // Bytes actually mapped: 48 for a v2 publisher, 40 for a legacy v1 one.
    std::size_t sharedBytes = 0;
    ULONGLONG mappingLastTried = 0;
    std::filesystem::path fontDir;
    HudFonts fonts;
    // Persistent offscreen graphics used only for text measurement, so no HDC
    // round-trips are needed per frame.
    std::unique_ptr<Gdiplus::Bitmap> measureBitmap;
    std::unique_ptr<Gdiplus::Graphics> measureGfx;
    RECT lastRect{};        // last applied window rect; avoids SetWindowPos spam
    bool haveLastRect = false;
    // Kill-flash visibility: the counter stays hidden until the first kill.
    std::uint64_t lastKillSeq = 0;
    bool haveBaselineKill = false;
    unsigned long long lastScore = 0;
    bool haveBaselineScore = false;
    ULONGLONG lastKillMs = 0;
    bool killFlashActive = false;
    bool hiddenCleared = false; // last painted frame was the transparent clear
};

OverlayState* g_state = nullptr;

std::filesystem::path LocalSettingsDir() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path(buffer) / L"DOOMTheDarkAges" / L"SurvivalShield";
}

void WriteDefaultIni(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wt") != 0 || f == nullptr) {
        return;
    }
    fwprintf(f,
             L"; Hellwalker HUD overlay\n"
             L"; The counter only appears when a demon is killed: it holds for\n"
             L"; 2 s and fades out within a 4 s window total, so it never covers\n"
             L"; map loads or the game's own animations.\n"
             L"; x,y   = top-left of the overlay window in screen pixels\n"
             L";         (-1 = automatic: top-right, per the reference mockup)\n"
             L"; scale = 0.6 .. 2.0 (1.0 matches the mockup proportions at 1080p;\n"
             L";         other resolutions scale automatically by height)\n"
             L"; show_progress = 1 draws a thin charge bar inside the plate\n"
             L"; always_show = 1 skips the kill-flash gating (visual testing only)\n"
             L"; Edits hot-reload once per second.\n"
             L"x=-1\n"
             L"y=-1\n"
             L"scale=1.0\n"
             L"show_progress=1\n"
             L"always_show=0\n");
    fclose(f);
}

OverlayConfig LoadIni(const std::filesystem::path& path) {
    OverlayConfig config;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rt") != 0 || f == nullptr) {
        return config;
    }
    wchar_t line[128]{};
    while (fgetws(line, 128, f) != nullptr) {
        wchar_t key[32]{};
        double value = 0.0;
        if (swscanf_s(line, L"%31[^=]=%lf", key, 32U, &value) == 2) {
            if (wcscmp(key, L"x") == 0) config.x = static_cast<int>(value);
            else if (wcscmp(key, L"y") == 0) config.y = static_cast<int>(value);
            else if (wcscmp(key, L"scale") == 0)
                config.scale = std::clamp(value, 0.6, 2.0);
            else if (wcscmp(key, L"show_progress") == 0)
                config.showProgress = value >= 0.5;
            else if (wcscmp(key, L"always_show") == 0)
                config.alwaysShow = value >= 0.5;
        }
    }
    fclose(f);
    return config;
}

void AttachShared(OverlayState& state) {
    const ULONGLONG now = GetTickCount64();
    if (state.mapping != nullptr || now - state.mappingLastTried < 1000ULL) {
        return;
    }
    state.mappingLastTried = now;
    state.mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kHudSharedName);
    if (state.mapping == nullptr) {
        return;
    }
    // Prefer the v2 layout (with killSeq); fall back to a legacy v1 publisher
    // whose section is only 40 bytes — mapping 48 there would fail.
    void* view = MapViewOfFile(state.mapping, FILE_MAP_READ, 0, 0, sizeof(HudSharedState));
    std::size_t bytes = sizeof(HudSharedState);
    if (view == nullptr) {
        view = MapViewOfFile(state.mapping, FILE_MAP_READ, 0, 0, kHudSharedV1Size);
        bytes = kHudSharedV1Size;
    }
    if (view == nullptr) {
        CloseHandle(state.mapping);
        state.mapping = nullptr;
        return;
    }
    auto* shared = static_cast<HudSharedState*>(view);
    const bool versionOk = shared->magic == kHudSharedMagic &&
                           ((bytes == sizeof(HudSharedState) && shared->version == 2) ||
                            (bytes == kHudSharedV1Size && shared->version == 1));
    if (!versionOk) {
        UnmapViewOfFile(shared);
        CloseHandle(state.mapping);
        state.mapping = nullptr;
        return;
    }
    state.shared = shared;
    state.sharedBytes = bytes;
}

bool SharedIsFresh(const OverlayState& state, HudSharedState& snapshot) {
    if (state.shared == nullptr) {
        return false;
    }
    // The publisher is the only writer; a torn read is impossible in practice
    // for our own aligned fields, but copy + magic check anyway. Only the
    // mapped prefix is copied: a v1 section has no killSeq bytes.
    std::memcpy(&snapshot, state.shared, state.sharedBytes);
    if (snapshot.magic != kHudSharedMagic) {
        return false;
    }
    return GetTickCount64() - snapshot.heartbeatMs < 2500ULL;
}

int DigitsFor(int value) {
    int digits = 1;
    while (value >= 10) {
        value /= 10;
        ++digits;
    }
    return digits;
}

// The plate has 3/4/5-digit forms per the reference; clamp so a tiny target
// still gets the 3-figure plate and a runaway one uses the 5-figure plate.
int FormDigits(int targetScore) {
    const int digits = DigitsFor(std::max(1, targetScore));
    return std::clamp(digits, 3, 5);
}

// Re-alpha an ARGB constant; GDI+ has no (BYTE, ARGB) constructor.
Gdiplus::Color ArgbWithAlpha(int alpha, Gdiplus::ARGB argb) {
    const Gdiplus::Color c(argb);
    return Gdiplus::Color(static_cast<BYTE>(std::clamp(alpha, 0, 255)),
                          c.GetR(), c.GetG(), c.GetB());
}

void LoadFrameBitmaps(HudFonts& fonts, const std::filesystem::path& fontDir) {
    const std::filesystem::path baseDir = fontDir.parent_path();
    const std::filesystem::path candidates[] = {
        baseDir / L"hud",
        baseDir.parent_path() / L"assets" / L"hud",
        fontDir / L".." / L"hud",
        fontDir / L".." / L".." / L"assets" / L"hud",
    };
    for (int i = 0; i < 3; ++i) {
        if (fonts.frames[i] != nullptr) {
            continue;
        }
        for (const auto& dir : candidates) {
            std::error_code ec;
            const auto p = dir / kFrameSpecs[i].fileName;
            if (std::filesystem::exists(p, ec)) {
                auto bmp = std::make_unique<Gdiplus::Bitmap>(p.wstring().c_str());
                if (bmp->GetLastStatus() == Gdiplus::Ok && bmp->GetWidth() > 0) {
                    fonts.frames[i] = std::move(bmp);
                    break;
                }
            }
        }
    }
}

bool InitHudFonts(HudFonts& fonts, const std::filesystem::path& fontDir) {
    LoadFrameBitmaps(fonts, fontDir);
    const std::filesystem::path digitPath = fontDir / L"Cinzel-Bold.ttf";
    if (std::filesystem::exists(digitPath) &&
        fonts.collection.AddFontFile(digitPath.wstring().c_str()) == Gdiplus::Ok) {
        Gdiplus::FontFamily families[1]{};
        INT count = 0;
        fonts.collection.GetFamilies(1, families, &count);
        if (count >= 1) {
            wchar_t name[LF_FACESIZE]{};
            families[0].GetFamilyName(name, LF_FACESIZE);
            auto custom = std::make_unique<Gdiplus::FontFamily>(name, &fonts.collection);
            if (custom->GetLastStatus() == Gdiplus::Ok) {
                fonts.digit = std::move(custom);
                fonts.custom = true;
                return true;
            }
        }
    }
    // System fallbacks when the gothic font is missing. FontFamily has no
    // public assignment, so the chain resolves through fresh allocations.
    auto palatino = std::make_unique<Gdiplus::FontFamily>(L"Palatino Linotype");
    if (palatino->GetLastStatus() == Gdiplus::Ok) {
        fonts.digit = std::move(palatino);
        return false;
    }
    fonts.digit = std::make_unique<Gdiplus::FontFamily>(L"Georgia");
    return false;
}

void EnsureScaleFonts(HudFonts& fonts, double effectiveScale) {
    if (fonts.digitFont != nullptr && fonts.builtForScale == effectiveScale) {
        return;
    }
    fonts.builtForScale = effectiveScale;
    fonts.digitFont = std::make_unique<Gdiplus::Font>(
        fonts.digit.get(), REAL(42.0 * effectiveScale), Gdiplus::FontStyleBold,
        Gdiplus::UnitPixel);
    fonts.captionFont = std::make_unique<Gdiplus::Font>(
        fonts.digit.get(), REAL(17.0 * effectiveScale), Gdiplus::FontStyleRegular,
        Gdiplus::UnitPixel);
}

double MeasureTextW(HudFonts& fonts, Gdiplus::Graphics& g, const wchar_t* text,
                    bool caption) {
    Gdiplus::Font* font = caption ? fonts.captionFont.get() : fonts.digitFont.get();
    Gdiplus::RectF bounds{};
    g.MeasureString(text, -1, font, Gdiplus::PointF(0, 0), &bounds);
    return bounds.Width;
}

struct HudLayout {
    double scale = 1.0;     // effective (ini * screen-height) scale
    int digits = 3;
    double textW = 0.0;     // measured digit-string width
    double captionW = 0.0;
    int panelW = 0;
    int panelH = 0;
    int tipU = 0;           // pointed-end horizontal extent
    int glowM = 0;          // outer margin kept for the glow inside the window
    int crossAbove = 0;     // cross visible above the plate's top edge
    int crossBelow = 0;     // cross tip below the plate's bottom edge
    int windowW = 0;
    int windowH = 0;
    // Plate position inside the overlay window.
    int panelX = 0;
    int panelY = 0;
};

HudLayout ComputeLayout(double effectiveScale, int digits, double textW,
                        double captionW) {
    const double s = effectiveScale;
    const FrameSpec& spec = GetFrameSpec(digits);
    const double artScale = s * kArtworkBaseScale;
    HudLayout lay;
    lay.scale = s;
    lay.digits = spec.digits;
    lay.textW = textW;
    lay.captionW = captionW;
    lay.panelW = static_cast<int>(std::ceil(spec.srcW * artScale));
    lay.panelH = static_cast<int>(std::ceil(spec.srcH * artScale));
    lay.tipU = 0;
    lay.glowM = static_cast<int>(std::ceil(8.0 * s));
    lay.crossAbove = 0;
    lay.crossBelow = 0;
    lay.windowW = lay.panelW + 2 * lay.glowM;
    lay.windowH = lay.panelH + 2 * lay.glowM;
    lay.panelX = lay.glowM;
    lay.panelY = lay.glowM;
    return lay;
}

// Automatic anchor: positioned on the upper-right margin directly below the
// vanilla Gold / Extra Life / Ruby HUD cluster (~98.2% right edge, ~14.5% top)
// so it stays completely clear of both the top-right vanilla icons and the
// top-center Boss health bar.
void ComputeAutoRect(const HudLayout& lay, int screenW, int screenH, RECT& out) {
    const int right = static_cast<int>(0.982 * screenW);
    int top = static_cast<int>(0.145 * screenH) - lay.glowM - lay.crossAbove;
    top = std::max(4, top);
    out.left = right - lay.windowW;
    out.top = top;
    out.right = out.left + lay.windowW;
    out.bottom = out.top + lay.windowH;
}

// Renders one overlay frame into the given graphics (window- or file-sized).
// Uses the demonic skull/Slayer-rune frame artwork for 3, 4, or 5 digits and
// renders molten fiery digits centered inside each individual cell.
void DrawHudFrame(Gdiplus::Graphics& g, const HudLayout& lay,
                  const HudSharedState& hud, bool fresh, bool showProgress,
                  double pulse, const HudFonts& fonts) {
    const double s = lay.scale;
    const bool charged = fresh && hud.enabled != 0 && hud.supercharged != 0;
    const int dimming = fresh ? 255 : 110;
    const int clampedDigits = std::clamp(lay.digits, 3, 5);
    const FrameSpec& spec = GetFrameSpec(clampedDigits);
    const double artScale = s * kArtworkBaseScale;

    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);

    const int panelX = lay.panelX;
    const int panelY = lay.panelY;
    const int panelW = lay.panelW;
    const int panelH = lay.panelH;

    Gdiplus::Bitmap* frameBmp = fonts.frames[clampedDigits - 3].get();
    if (frameBmp != nullptr) {
        Gdiplus::ImageAttributes imgAttr;
        if (dimming < 255) {
            const REAL a = REAL(dimming) / 255.0f;
            const Gdiplus::ColorMatrix cm = {{
                {1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
                {0.0f, 1.0f, 0.0f, 0.0f, 0.0f},
                {0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
                {0.0f, 0.0f, 0.0f, a,    0.0f},
                {0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
            }};
            imgAttr.SetColorMatrix(&cm, Gdiplus::ColorMatrixFlagsDefault,
                                   Gdiplus::ColorAdjustTypeBitmap);
        }
        const Gdiplus::Rect destRect(panelX, panelY, panelW, panelH);
        g.DrawImage(frameBmp, destRect, 0, 0, frameBmp->GetWidth(), frameBmp->GetHeight(),
                    Gdiplus::UnitPixel, dimming < 255 ? &imgAttr : nullptr);
    }

    // Format zero-padded score to exact digit count (3, 4, or 5)
    wchar_t text[16]{};
    const unsigned long long maxVal =
        (clampedDigits == 3) ? 999ULL : ((clampedDigits == 4) ? 9999ULL : 99999ULL);
    if (fresh) {
        const unsigned long long shown = std::min<unsigned long long>(hud.scorePoints, maxVal);
        wchar_t fmt[16]{};
        swprintf(fmt, 16, L"%%0%dllu", clampedDigits);
        swprintf(text, 16, fmt, shown);
    } else {
        for (int i = 0; i < clampedDigits; ++i) {
            text[i] = L'0';
        }
    }

    // Draw each digit inside its dedicated lava cell using GraphicsPath so we get
    // the exact molten fiery gradient + glowing crimson-orange outline from the artwork.
    Gdiplus::StringFormat cellFormat;
    cellFormat.SetAlignment(Gdiplus::StringAlignmentCenter);
    cellFormat.SetLineAlignment(Gdiplus::StringAlignmentCenter);

    const REAL cellTop = REAL(panelY + spec.cellY0 * artScale);
    const REAL cellH   = REAL((spec.cellY1 - spec.cellY0) * artScale);
    const REAL fontPx  = REAL(cellH * 0.78f);

    for (int i = 0; i < clampedDigits; ++i) {
        const wchar_t digitStr[2] = {text[i], L'\0'};
        const REAL cellX = REAL(panelX + spec.cells[i].x0 * artScale);
        const REAL cellW = REAL((spec.cells[i].x1 - spec.cells[i].x0) * artScale);
        const Gdiplus::RectF cellRect(cellX, cellTop + REAL(1.5 * s), cellW, cellH);

        if (charged) {
            // Pulsing magma chamber glow strictly inside each digit cell when ready
            const BYTE chamberAlpha = static_cast<BYTE>(
                std::clamp(static_cast<int>((30 + 45 * pulse) * dimming / 255), 0, 255));
            Gdiplus::SolidBrush chamberGlow(Gdiplus::Color(chamberAlpha, 255, 55, 0));
            const Gdiplus::RectF innerCell(cellX + REAL(2.0 * s), cellTop + REAL(2.0 * s),
                                           cellW - REAL(4.0 * s), cellH - REAL(4.0 * s));
            g.FillRectangle(&chamberGlow, innerCell);
        }

        Gdiplus::GraphicsPath glyphPath;
        glyphPath.AddString(digitStr, 1, fonts.digit.get(), Gdiplus::FontStyleBold,
                            fontPx, cellRect, &cellFormat);

        // 1) Deep dark shadow beneath the molten glyph
        {
            Gdiplus::Matrix shift;
            shift.Translate(REAL(1.5 * s), REAL(2.2 * s));
            glyphPath.Transform(&shift);
            Gdiplus::SolidBrush shadowBrush(Gdiplus::Color(static_cast<BYTE>(dimming * 210 / 255), 15, 2, 0));
            g.FillPath(&shadowBrush, &glyphPath);
            shift.Invert();
            glyphPath.Transform(&shift);
        }

        // 2) Outer fiery red/orange glow strokes
        const double glowMul = charged ? (1.0 + 0.35 * pulse) : 0.85;
        const BYTE outerGlowA = static_cast<BYTE>(
            std::clamp(static_cast<int>(95 * glowMul * dimming / 255), 0, 255));
        const BYTE innerGlowA = static_cast<BYTE>(
            std::clamp(static_cast<int>(175 * glowMul * dimming / 255), 0, 255));
        Gdiplus::Pen outerGlowPen(Gdiplus::Color(outerGlowA, 255, 28, 0), REAL(5.2 * s));
        outerGlowPen.SetLineJoin(Gdiplus::LineJoinRound);
        g.DrawPath(&outerGlowPen, &glyphPath);

        Gdiplus::Pen innerGlowPen(Gdiplus::Color(innerGlowA, 255, 85, 0), REAL(2.4 * s));
        innerGlowPen.SetLineJoin(Gdiplus::LineJoinRound);
        g.DrawPath(&innerGlowPen, &glyphPath);

        // 3) Molten lava vertical gradient fill
        const Gdiplus::Color topColor = charged
            ? ArgbWithAlpha(dimming, 0xFFFFF6B0)  // white-hot gold when supercharged
            : ArgbWithAlpha(dimming, 0xFFFFD038); // bright molten yellow-gold
        const Gdiplus::Color botColor = charged
            ? ArgbWithAlpha(dimming, 0xFFFF2400)  // intense scarlet-orange when supercharged
            : ArgbWithAlpha(dimming, 0xFFE84200); // fiery magma orange-red
        Gdiplus::LinearGradientBrush fillBrush(cellRect, topColor, botColor,
                                               Gdiplus::LinearGradientModeVertical);
        g.FillPath(&fillBrush, &glyphPath);

        // 4) Crisp bright inner rim highlight
        const BYTE rimA = static_cast<BYTE>(std::clamp(dimming * (charged ? 220 : 165) / 255, 0, 255));
        Gdiplus::Pen rimPen(Gdiplus::Color(rimA, 255, 230, 110), REAL(0.85 * s));
        rimPen.SetLineJoin(Gdiplus::LineJoinRound);
        g.DrawPath(&rimPen, &glyphPath);
    }

    if (showProgress) {
        const REAL barX = REAL(panelX + spec.cells[0].x0 * artScale);
        const REAL barRight = REAL(panelX + spec.cells[clampedDigits - 1].x1 * artScale);
        const REAL barW = std::max(REAL(10.0f), barRight - barX);
        const REAL barH = std::max(REAL(3.5f), REAL(4.5 * s));
        const REAL barY = REAL(panelY + spec.cellY1 * artScale + 2.5 * s);
        const Gdiplus::RectF trackRect(barX, barY, barW, barH);

        // Dark obsidian recessed track with subtle crimson rim
        Gdiplus::SolidBrush trackBrush(Gdiplus::Color(static_cast<BYTE>(dimming * 210 / 255), 14, 4, 2));
        g.FillRectangle(&trackBrush, trackRect);
        Gdiplus::Pen borderPen(Gdiplus::Color(static_cast<BYTE>(dimming * 180 / 255), 110, 18, 10), REAL(1.0 * s));
        g.DrawRectangle(&borderPen, trackRect.X, trackRect.Y, trackRect.Width, trackRect.Height);

        const float frac = fresh ? std::clamp(hud.scoreFraction, 0.0f, 1.0f) : 0.0f;
        const REAL fillW = barW * frac;
        if (fillW > REAL(1.0f)) {
            const Gdiplus::RectF fillRect(barX, barY, fillW, barH);
            const Gdiplus::Color cLeft = charged
                ? ArgbWithAlpha(dimming, 0xFFFF6A00)
                : ArgbWithAlpha(dimming, 0xFFD82600);
            const Gdiplus::Color cRight = charged
                ? ArgbWithAlpha(dimming, 0xFFFFF080)
                : ArgbWithAlpha(dimming, 0xFFFFB020);
            Gdiplus::LinearGradientBrush barFill(fillRect, cLeft, cRight,
                                                 Gdiplus::LinearGradientModeHorizontal);
            g.FillRectangle(&barFill, fillRect);
        }
    }
}

void Render(HWND hwnd) {
    OverlayState& state = *g_state;
    HudSharedState snapshot{};
    AttachShared(state);
    const bool fresh = SharedIsFresh(state, snapshot);

    // Kill-flash gating: baseline the first snapshot, then re-show the counter
    // on every killSeq bump (v2) or score delta (v1 fallback). The counter is
    // invisible at boot, over map loads, and between kills.
    const ULONGLONG nowMs = GetTickCount64();
    if (fresh) {
        if (state.sharedBytes >= sizeof(HudSharedState)) {
            if (!state.haveBaselineKill) {
                state.lastKillSeq = snapshot.killSeq;
                state.haveBaselineKill = true;
            } else if (snapshot.killSeq != state.lastKillSeq) {
                state.lastKillSeq = snapshot.killSeq;
                state.lastKillMs = nowMs;
                state.killFlashActive = true;
            }
        } else {
            // Legacy publisher without killSeq: score changes stand in for
            // kills. This cannot fire while the score is frozen.
            if (!state.haveBaselineScore) {
                state.lastScore = snapshot.scorePoints;
                state.haveBaselineScore = true;
            } else if (snapshot.scorePoints != state.lastScore) {
                state.lastScore = snapshot.scorePoints;
                state.lastKillMs = nowMs;
                state.killFlashActive = true;
            }
        }
    }

    int alpha = 0;
    if (state.config.alwaysShow) {
        alpha = 255;
    } else if (fresh && state.killFlashActive) {
        const ULONGLONG dt = nowMs - state.lastKillMs;
        if (dt < kKillHoldMs) {
            alpha = 255;
        } else if (dt < kKillHoldMs + kKillFadeMs) {
            alpha = static_cast<int>(255 - 255 * (dt - kKillHoldMs) / kKillFadeMs);
        } else {
            state.killFlashActive = false;
        }
    }

    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    const double effectiveScale = state.config.scale * (screenH / kDesignHeight);
    EnsureScaleFonts(state.fonts, effectiveScale);
    if (state.fonts.digitFont == nullptr || state.fonts.captionFont == nullptr ||
        state.measureGfx == nullptr) {
        return;
    }

    const int digits = FormDigits(fresh ? snapshot.targetScore : 1);
    wchar_t probe[40]{};
    if (fresh) {
        wchar_t fmt[16]{};
        swprintf(fmt, 16, L"%%0%dllu", digits);
        swprintf(probe, 40, fmt,
                 std::min<unsigned long long>(snapshot.scorePoints, 9999999ULL));
    } else {
        for (int i = 0; i < digits; ++i) probe[i] = L'0';
    }
    const double textW = MeasureTextW(state.fonts, *state.measureGfx, probe, false);
    const double captionW = MeasureTextW(state.fonts, *state.measureGfx, L"5 DIGITOS", true);
    const HudLayout lay = ComputeLayout(effectiveScale, digits, textW, captionW);
    const int w = lay.windowW;
    const int h = lay.windowH;
    if (w <= 0 || h <= 0) {
        return;
    }

    // Keep the window fitted to the current form; the target (and therefore
    // the digit count) can change mid-game from the settings JSON.
    RECT want{};
    if (state.config.x >= 0 && state.config.y >= 0) {
        want.left = state.config.x;
        want.top = state.config.y;
        want.right = want.left + w;
        want.bottom = want.top + h;
    } else {
        ComputeAutoRect(lay, GetSystemMetrics(SM_CXSCREEN), screenH, want);
    }
    if (!state.haveLastRect ||
        want.left != state.lastRect.left || want.top != state.lastRect.top ||
        want.right != state.lastRect.right || want.bottom != state.lastRect.bottom) {
        state.lastRect = want;
        state.haveLastRect = true;
        SetWindowPos(hwnd, HWND_TOPMOST, want.left, want.top, w, h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    if (alpha <= 0) {
        if (!state.hiddenCleared) {
            // Paint one fully transparent frame so the previous render leaves
            // the screen, then idle until the next kill.
            Gdiplus::Bitmap clear(w, h, PixelFormat32bppPARGB);
            Gdiplus::Graphics g(&clear);
            g.Clear(Gdiplus::Color(0, 0, 0, 0));
            HBITMAP clearBm = nullptr;
            if (clear.GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &clearBm) == Gdiplus::Ok &&
                clearBm != nullptr) {
                HDC screenDc = GetDC(hwnd);
                HDC memDc = CreateCompatibleDC(screenDc);
                HGDIOBJ old = SelectObject(memDc, clearBm);
                POINT dstPt{want.left, want.top};
                POINT zero{0, 0};
                SIZE size{w, h};
                BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
                UpdateLayeredWindow(hwnd, screenDc, &dstPt, &size, memDc, &zero,
                                    RGB(0, 0, 0), &blend, ULW_ALPHA);
                SelectObject(memDc, old);
                DeleteDC(memDc);
                ReleaseDC(hwnd, screenDc);
                DeleteObject(clearBm);
            }
            state.hiddenCleared = true;
        }
        return;
    }
    state.hiddenCleared = false;

    Gdiplus::Bitmap bitmap(w, h, PixelFormat32bppPARGB);
    Gdiplus::Graphics g(&bitmap);
    g.Clear(Gdiplus::Color(0, 0, 0, 0));

    const double now = GetTickCount64() / 1000.0;
    const double pulse = 0.5 + 0.5 * std::sin(now * 2.0 * 3.14159265 * 1.2);
    DrawHudFrame(g, lay, snapshot, fresh, state.config.showProgress, pulse,
                 state.fonts);

    HBITMAP hbm = nullptr;
    if (bitmap.GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &hbm) != Gdiplus::Ok || hbm == nullptr) {
        return;
    }
    HDC screenDc = GetDC(hwnd);
    HDC memDc = CreateCompatibleDC(screenDc);
    HGDIOBJ old = SelectObject(memDc, hbm);
    POINT dstPt{want.left, want.top};
    POINT zero{0, 0};
    SIZE size{w, h};
    // Per-pixel alpha carries the artwork; SourceConstantAlpha carries the
    // kill-flash fade so the whole plate eases out uniformly.
    BLENDFUNCTION blend{AC_SRC_OVER, 0, static_cast<BYTE>(alpha), AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd, screenDc, &dstPt, &size, memDc, &zero, RGB(0, 0, 0), &blend,
                        ULW_ALPHA);
    SelectObject(memDc, old);
    DeleteDC(memDc);
    ReleaseDC(hwnd, screenDc);
    DeleteObject(hbm);
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TIMER: {
            OverlayState& state = *g_state;
            if (state.gameProcess != nullptr &&
                WaitForSingleObject(state.gameProcess, 0) == WAIT_OBJECT_0) {
                DestroyWindow(hwnd);
                return 0;
            }
            const ULONGLONG now = GetTickCount64();
            if (now - state.iniLastChecked >= kIniReloadMs) {
                state.iniLastChecked = now;
                state.config = LoadIni(state.iniPath);
            }
            Render(hwnd);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

#ifdef SURVIVALSHIELD_HUD_PREVIEW

// Offscreen preview: renders the plate forms to PNGs with the exact code the
// live window uses, so the design can be checked without launching the game.
//   RunHudPreview <outDir> [fontDir] [scale]
int RunHudPreview(int argc, wchar_t** argv) {
    const std::filesystem::path outDir = argc > 1 ? argv[1] : L".";
    std::filesystem::path fontDir = argc > 2 ? argv[2] : (outDir / L"fonts");
    const double scale = argc > 3 ? _wtof(argv[3]) : 1.0;
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    Gdiplus::GdiplusStartupInput startup;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok) {
        return 10;
    }

    int rc = 0;
    {
        // All GDI+ objects (including the private font collection) must be
        // destroyed BEFORE GdiplusShutdown: their destructors call the flat
        // API, which is invalid once the session is shut down.
        HudFonts fonts;
        InitHudFonts(fonts, fontDir);
        EnsureScaleFonts(fonts, scale);
        Gdiplus::Bitmap measureBmp(8, 8, PixelFormat32bppPARGB);
        Gdiplus::Graphics measureGfx(&measureBmp);

        struct Case {
            const wchar_t* name;
            unsigned long long score;
            int target;
            bool charged;
            double pulse;
            bool fresh;
            int canvasW;
            int canvasH;
        };
        // Form cases frame the plate tightly (window rect); the position case
        // renders a full 1920x1080 canvas to check the automatic anchor.
        const Case cases[] = {
            {L"hud_form3", 666, 800, false, 0.0, true, 0, 0},
            {L"hud_form4", 1993, 2500, false, 0.0, true, 0, 0},
            {L"hud_form5", 4815, 10000, false, 0.0, true, 0, 0},
            {L"hud_ready", 1993, 1993, true, 0.85, true, 0, 0},
            {L"hud_dimmed", 0, 0, false, 0.0, false, 0, 0},
        };

        GUID pngClsid{};
        CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &pngClsid);

        for (const Case& c : cases) {
        HudSharedState hud{};
        hud.magic = kHudSharedMagic;
        hud.version = kHudSharedVersion;
        hud.enabled = 1;
        hud.supercharged = c.charged ? 1 : 0;
        hud.scorePoints = c.score;
        hud.targetScore = c.target;
        hud.scoreFraction = c.target > 0
            ? static_cast<float>(std::min(1.0, double(c.score) / c.target))
            : 0.0f;

        const int digits = FormDigits(c.target);
        wchar_t probe[40]{};
        if (c.fresh) {
            wchar_t fmt[16]{};
            swprintf(fmt, 16, L"%%0%dllu", digits);
            swprintf(probe, 40, fmt, c.score);
        } else {
            for (int i = 0; i < digits; ++i) probe[i] = L'0';
        }
        const double textW = MeasureTextW(fonts, measureGfx, probe, false);
        const double captionW = MeasureTextW(fonts, measureGfx, L"5 DIGITOS", true);
        HudLayout lay = ComputeLayout(scale, digits, textW, captionW);

        int cw = c.canvasW > 0 ? c.canvasW : lay.windowW;
        int ch = c.canvasH > 0 ? c.canvasH : lay.windowH;
        if (c.canvasW == 0) {
            // Frame cases: anchor the plate at the mockup's spot on a full
            // HD canvas, then crop to content + margins.
            cw = 1920;
            ch = 1080;
        }
        Gdiplus::Bitmap bmp(cw, ch, PixelFormat32bppPARGB);
        Gdiplus::Graphics g(&bmp);
        if (c.canvasW == 0) {
            // Frame cases: draw into a window-sized canvas exactly like the
            // live window does (plate relative to its own window rect), over
            // a dark backdrop so the glows read like they do over the game.
            const int cropW = lay.windowW;
            const int cropH = lay.windowH;
            Gdiplus::Bitmap cropped(cropW, cropH, PixelFormat32bppPARGB);
            Gdiplus::Graphics cg(&cropped);
            cg.Clear(Gdiplus::Color(255, 18, 15, 14));
            lay.panelX = lay.glowM;
            lay.panelY = lay.glowM + lay.crossAbove;
            DrawHudFrame(cg, lay, hud, c.fresh, true, c.pulse, fonts);
            std::filesystem::path path = outDir / (std::wstring(c.name) + L".png");
            if (cropped.Save(path.c_str(), &pngClsid, nullptr) != Gdiplus::Ok) {
                rc = 20;
            }
            continue;
        }
        g.Clear(Gdiplus::Color(255, 18, 15, 14));
        lay.panelX = lay.glowM;
        lay.panelY = lay.glowM + lay.crossAbove;
        DrawHudFrame(g, lay, hud, c.fresh, true, c.pulse, fonts);
        std::filesystem::path path = outDir / (std::wstring(c.name) + L".png");
        if (bmp.Save(path.c_str(), &pngClsid, nullptr) != Gdiplus::Ok) {
            rc = 20;
        }
    }

    // Full-screen position check: 4-digit form at the automatic anchor.
    {
        HudSharedState hud{};
        hud.magic = kHudSharedMagic;
        hud.version = kHudSharedVersion;
        hud.enabled = 1;
        hud.scorePoints = 234;
        hud.targetScore = 1500;
        hud.scoreFraction = 234.0f / 1500.0f;

        const int digits = FormDigits(hud.targetScore);
        wchar_t probe[40]{};
        wchar_t fmt[16]{};
        swprintf(fmt, 16, L"%%0%dllu", digits);
        swprintf(probe, 40, fmt, hud.scorePoints);
        const double textW = MeasureTextW(fonts, measureGfx, probe, false);
        const double captionW = MeasureTextW(fonts, measureGfx, L"5 DIGITOS", true);
        HudLayout lay = ComputeLayout(scale, digits, textW, captionW);
        lay.panelX = lay.glowM;
        lay.panelY = lay.glowM + lay.crossAbove;

        Gdiplus::Bitmap bmp(1920, 1080, PixelFormat32bppPARGB);
        Gdiplus::Graphics g(&bmp);
        g.Clear(Gdiplus::Color(255, 16, 13, 12));
        // Faint guide lines marking the mockup's anchor fractions.
        Gdiplus::Pen guide(Gdiplus::Color(60, 200, 60, 60), REAL(1.0f));
        g.DrawLine(&guide, REAL(0.982 * 1920), REAL(0), REAL(0.982 * 1920), REAL(1080));
        g.DrawLine(&guide, REAL(0), REAL(0.145 * 1080), REAL(1920), REAL(0.145 * 1080));
        // Draw with the auto offset: shift so the plate lands on the anchor.
        RECT autoRect{};
        ComputeAutoRect(lay, 1920, 1080, autoRect);
        lay.panelX = autoRect.left + lay.glowM;
        lay.panelY = autoRect.top + lay.glowM + lay.crossAbove;
        DrawHudFrame(g, lay, hud, true, true, 0.0, fonts);
        std::filesystem::path path = outDir / L"hud_position.png";
        if (bmp.Save(path.c_str(), &pngClsid, nullptr) != Gdiplus::Ok) {
            rc = 20;
        }
    }
    } // GDI+ objects (fonts, bitmaps) must be out of scope before shutdown.

    Gdiplus::GdiplusShutdown(token);
    return rc;
}

#endif // SURVIVALSHIELD_HUD_PREVIEW

// External linkage: the loader calls this after injection (or with --hud-only).
int RunHudOverlay(HANDLE gameProcess) {
    SetProcessDPIAware();

    const std::filesystem::path iniPath = LocalSettingsDir() / L"hud.ini";
    if (iniPath.empty()) {
        return 0;
    }
    if (!std::filesystem::exists(iniPath)) {
        WriteDefaultIni(iniPath);
    }

    Gdiplus::GdiplusStartupInput gdiplusStartup;
    ULONG_PTR gdiplusToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartup, nullptr) != Gdiplus::Ok) {
        return 10;
    }

    // Every GDI+ object (font collection, fonts, bitmaps) lives inside this
    // scope so the destructors run BEFORE GdiplusShutdown; calling the flat
    // API after the session ends crashes at teardown.
    int exitCode = 0;
    {
        OverlayState state;
        state.gameProcess = gameProcess;
        state.iniPath = iniPath;
        state.config = LoadIni(iniPath);

        wchar_t selfPath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
        state.fontDir = std::filesystem::path(selfPath).parent_path() / L"fonts";

        InitHudFonts(state.fonts, state.fontDir);
        const int screenH = GetSystemMetrics(SM_CYSCREEN);
        EnsureScaleFonts(state.fonts, state.config.scale * (screenH / kDesignHeight));
        state.measureBitmap = std::make_unique<Gdiplus::Bitmap>(8, 8, PixelFormat32bppPARGB);
        state.measureGfx = std::make_unique<Gdiplus::Graphics>(state.measureBitmap.get());

        WNDCLASSW wc{};
        wc.lpfnWndProc = OverlayWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kOverlayClass;
        if (RegisterClassW(&wc) == 0) {
            exitCode = 11;
        } else {
            // Initial rect: computed the same way the repaint tick keeps it
            // fitted. The first snapshot may still be absent, so size for the
            // smallest form; Render() refits as soon as data arrives.
            const int digits = FormDigits(1);
            const double textW = MeasureTextW(state.fonts, *state.measureGfx, L"000", false);
            const double captionW =
                MeasureTextW(state.fonts, *state.measureGfx, L"5 DIGITOS", true);
            const HudLayout lay = ComputeLayout(
                state.config.scale * (screenH / kDesignHeight), digits, textW, captionW);
            RECT rect{};
            if (state.config.x >= 0 && state.config.y >= 0) {
                rect.left = state.config.x;
                rect.top = state.config.y;
                rect.right = rect.left + lay.windowW;
                rect.bottom = rect.top + lay.windowH;
            } else {
                ComputeAutoRect(lay, GetSystemMetrics(SM_CXSCREEN), screenH, rect);
            }

            g_state = &state;
            HWND hwnd = CreateWindowExW(
                WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
                    WS_EX_NOACTIVATE,
                kOverlayClass, L"Hellwalker HUD", WS_POPUP, rect.left, rect.top,
                lay.windowW, lay.windowH, nullptr, nullptr, wc.hInstance, nullptr);
            if (hwnd == nullptr) {
                exitCode = 12;
            } else {
                state.lastRect = rect;
                state.haveLastRect = true;
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                SetTimer(hwnd, kRepaintTimerId, kRepaintMs, nullptr);

                MSG msg{};
                while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            g_state = nullptr;
        }

        if (state.shared != nullptr) {
            UnmapViewOfFile(state.shared);
        }
        if (state.mapping != nullptr) {
            CloseHandle(state.mapping);
        }
    }
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return exitCode;
}

} // namespace survival_shield
