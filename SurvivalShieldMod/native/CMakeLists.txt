cmake_minimum_required(VERSION 3.24)
project(SurvivalShieldNative LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "SurvivalShieldNative is supported only as a 64-bit Windows build.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep injected DLLs self-contained instead of requiring a matching VC++ runtime
# installation on the player's machine. No CRT-owned objects cross DLL boundaries.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(SurvivalShieldCore STATIC
    src/SurvivalShieldCore.cpp
    src/SettingsStore.cpp
    src/ModLibrary.cpp
    src/DeclReader.cpp
    src/ModsMenu.cpp
)
target_include_directories(SurvivalShieldCore PUBLIC include)
target_compile_definitions(SurvivalShieldCore PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0602)

# Vendored TsudaKageyu/minhook v1.3.3. Built from source so the injected DLL
# keeps its static-CRT, dependency-free profile.
add_library(MinHook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hde/hde32.c
    third_party/minhook/src/hde/hde64.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
)
target_include_directories(MinHook PUBLIC third_party/minhook/include)
target_include_directories(MinHook PRIVATE third_party/minhook/src)

add_library(SurvivalShield SHARED
    src/Plugin.cpp
    src/Log.cpp
    src/UiBridge.cpp
    src/GameplayBridge.cpp
    src/BurnEngine.cpp
)
# The UI bridge diverts only the Hellwalker settings rows; the gameplay
# adapters stay out until their ABI is validated against the supported build.
target_sources(SurvivalShield PRIVATE src/BuildGuard.cpp)
target_link_libraries(SurvivalShield PRIVATE SurvivalShieldCore MinHook bcrypt)
target_compile_definitions(SurvivalShield PRIVATE SURVIVALSHIELD_UI_BRIDGE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0602)
set_target_properties(SurvivalShield PROPERTIES PREFIX "" OUTPUT_NAME "SurvivalShield")


# Separate runtime probe for the supported executable. It observes only the
# known AI-death AddScore callsite, logs its arguments, and forwards unchanged.
# Never replace the normal release DLL with this diagnostic build.
add_library(SurvivalShieldDiagnostics SHARED
    src/Plugin.cpp
    src/Log.cpp
    src/BuildGuard.cpp
    src/GameHooks.cpp
)
target_link_libraries(SurvivalShieldDiagnostics PRIVATE SurvivalShieldCore MinHook bcrypt)
target_compile_definitions(SurvivalShieldDiagnostics PRIVATE SURVIVALSHIELD_DIAGNOSTIC_HOOKS WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0602)
set_target_properties(SurvivalShieldDiagnostics PROPERTIES PREFIX "" OUTPUT_NAME "SurvivalShieldDiagnostics")

# Enhanced observer build for the next runtime sample. It logs a bounded raw
# scoring-item memory window for the first 16 death-score calls; it still
# forwards every call unchanged and is never an installable gameplay DLL.
add_library(SurvivalShieldDiagnosticsProbe SHARED
    src/Plugin.cpp
    src/Log.cpp
    src/BuildGuard.cpp
    src/GameHooks.cpp
)
target_link_libraries(SurvivalShieldDiagnosticsProbe PRIVATE SurvivalShieldCore MinHook bcrypt)
target_compile_definitions(SurvivalShieldDiagnosticsProbe PRIVATE SURVIVALSHIELD_DIAGNOSTIC_HOOKS WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0602)
set_target_properties(SurvivalShieldDiagnosticsProbe PROPERTIES PREFIX "" OUTPUT_NAME "SurvivalShieldDiagnosticsProbe")

# Broad but bounded probe for the next combat sample. Unlike the first probe,
# this records every AddScore caller and its RVA so we can discover which call
# path actually reports arena kills. Never install it beside another probe.
add_library(SurvivalShieldDiagnosticsProbeAll SHARED
    src/Plugin.cpp
    src/Log.cpp
    src/BuildGuard.cpp
    src/GameHooks.cpp
)
target_link_libraries(SurvivalShieldDiagnosticsProbeAll PRIVATE SurvivalShieldCore MinHook bcrypt)
target_compile_definitions(SurvivalShieldDiagnosticsProbeAll PRIVATE SURVIVALSHIELD_DIAGNOSTIC_HOOKS SURVIVALSHIELD_DIAGNOSTIC_ALL_CALLSCORE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0602)
set_target_properties(SurvivalShieldDiagnosticsProbeAll PROPERTIES PREFIX "" OUTPUT_NAME "SurvivalShieldDiagnosticsProbeAll")

# Broadcast observer for locating the event that reports arena/player kills.
# It filters a short allowlist of score and demon-kill IDs, captures bounded
# payload bytes, and always forwards the original event unchanged.
add_library(SurvivalShieldDiagnosticsHudEvents SHARED
    src/Plugin.cpp
    src/Log.cpp
    src/BuildGuard.cpp
    src/GameHooks.cpp
)
target_link_libraries(SurvivalShieldDiagnosticsHudEvents PRIVATE SurvivalShieldCore MinHook bcrypt)
target_compile_definitions(SurvivalShieldDiagnosticsHudEvents PRIVATE SURVIVALSHIELD_DIAGNOSTIC_HOOKS SURVIVALSHIELD_DIAGNOSTIC_HUD_EVENTS WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0602)
set_target_properties(SurvivalShieldDiagnosticsHudEvents PROPERTIES PREFIX "" OUTPUT_NAME "SurvivalShieldDiagnosticsHudEvents")

# Bisect variant: identical plugin but every detour disabled via compile flag.
# Used to tell "injected DLL presence" apart from "our hooks" when diagnosing
# game-side crashes.
add_library(SurvivalShieldNoHooks SHARED
    src/Plugin.cpp
    src/Log.cpp
    src/BuildGuard.cpp
)
target_link_libraries(SurvivalShieldNoHooks PRIVATE SurvivalShieldCore bcrypt)
target_compile_definitions(SurvivalShieldNoHooks PRIVATE SURVIVALSHIELD_NO_HOOKS WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0602)
set_target_properties(SurvivalShieldNoHooks PROPERTIES PREFIX "" OUTPUT_NAME "SurvivalShieldNoHooks")

include(CTest)
if(BUILD_TESTING)
    add_executable(SurvivalShieldTests tests/SurvivalShieldTests.cpp)
    target_link_libraries(SurvivalShieldTests PRIVATE SurvivalShieldCore)
    target_compile_definitions(SurvivalShieldTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0602)
    add_test(NAME SurvivalShieldCoreAndSettings COMMAND SurvivalShieldTests)
endif()

# Legacy proxy build retained for reference. It is not part of the supported
# load path and must not be copied into the game directory. The reason a direct
# executable launch returned code 53 was not isolated to this proxy.
add_library(SurvivalShieldProxy SHARED
    src/VersionProxy.cpp
    src/version_proxy.def
)
target_compile_definitions(SurvivalShieldProxy PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0602)
set_target_properties(SurvivalShieldProxy PROPERTIES PREFIX "" OUTPUT_NAME "version")

# External injector used for validation. Start the game through Steam, then
# inject into the running process; direct-launch code 53 is not treated as proof
# that the game rejects every added DLL. After injecting it hosts the gothic
# HUD overlay until the game process exits.
add_executable(SurvivalShieldLoader
    src/SurvivalShieldLoader.cpp
    src/HudOverlay.cpp
)
target_link_libraries(SurvivalShieldLoader PRIVATE Gdiplus)
target_include_directories(SurvivalShieldLoader PRIVATE include)
target_compile_definitions(SurvivalShieldLoader PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0602)
set_target_properties(SurvivalShieldLoader PROPERTIES OUTPUT_NAME "SurvivalShieldLoader")

# Offscreen HUD preview: renders the counter forms and the automatic anchor
# to PNGs with the same drawing code as the live overlay. Development aid
# only; nothing in the game or the loader depends on it.
add_executable(SurvivalShieldHudPreview
    src/HudOverlay.cpp
    src/HudPreviewMain.cpp
)
target_link_libraries(SurvivalShieldHudPreview PRIVATE Gdiplus)
target_include_directories(SurvivalShieldHudPreview PRIVATE include)
target_compile_definitions(SurvivalShieldHudPreview PRIVATE SURVIVALSHIELD_HUD_PREVIEW WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0602)
set_target_properties(SurvivalShieldHudPreview PROPERTIES OUTPUT_NAME "SurvivalShieldHudPreview")

if(MSVC)
    foreach(target SurvivalShieldCore SurvivalShield SurvivalShieldDiagnostics SurvivalShieldDiagnosticsProbe SurvivalShieldDiagnosticsProbeAll SurvivalShieldDiagnosticsHudEvents SurvivalShieldProxy SurvivalShieldLoader)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc)
    endforeach()
endif()
