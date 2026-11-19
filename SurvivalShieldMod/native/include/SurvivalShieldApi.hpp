#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

// Stable C ABI for a future UI/settings adapter loaded from SurvivalShield.dll.
extern "C" {
BOOL WINAPI SurvivalShield_GetOptions(BOOL* enabled, int* targetScore) noexcept;
BOOL WINAPI SurvivalShield_SetOptions(BOOL enabled, int targetScore) noexcept;
BOOL WINAPI SurvivalShield_GetPoweredDashDamage(int* damage) noexcept;
BOOL WINAPI SurvivalShield_SetPoweredDashDamage(int damage) noexcept;
BOOL WINAPI SurvivalShield_GetSettings(BOOL* enabled,
                                      int* targetScore,
                                      int* poweredDashDamage,
                                      int* pctFodder,
                                      int* pctHeavy,
                                      int* pctSuperheavy) noexcept;
BOOL WINAPI SurvivalShield_SetSettings(BOOL enabled,
                                      int targetScore,
                                      int poweredDashDamage,
                                      int pctFodder,
                                      int pctHeavy,
                                      int pctSuperheavy) noexcept;
BOOL WINAPI SurvivalShield_GetScoreState(unsigned long long* scorePoints,
                                        int* targetScore,
                                        BOOL* supercharged) noexcept;
// ARGB icon tint is blue while locked and LED red once the active mod reaches
// its target score.
BOOL WINAPI SurvivalShield_GetHudState(unsigned long long* scorePoints,
                                      int* targetScore,
                                      BOOL* supercharged,
                                      unsigned int* iconColorArgb) noexcept;
// Mod library, phase 1 "reader". SurvivalShield_GetModLibrary writes a UTF-8
// JSON snapshot into utf8Buffer; call with utf8Buffer == nullptr to query the
// required size (NUL included) via *size. SurvivalShield_RescanModLibrary
// re-scans <game>\mods\ and re-persists modlibrary.json.
BOOL WINAPI SurvivalShield_GetModLibrary(char* utf8Buffer, unsigned int* size) noexcept;
BOOL WINAPI SurvivalShield_RescanModLibrary() noexcept;
}
