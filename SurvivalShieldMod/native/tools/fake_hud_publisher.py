#!/usr/bin/env python3
"""Fake HUD publisher for visual checks of the overlay without the game.

Creates the Local\\HellwalkerHudState section the DLL normally owns (v2
layout, 48 bytes, with killSeq) and publishes a scripted sequence: a "kill"
every 500 ms while charging 0..2000 (each one re-shows the counter), then the
charged state with sparse kill flashes. Run alongside
SurvivalShieldLoader.exe --hud-only; Ctrl+C or kill to stop.
"""
import ctypes
import struct
import time

k32 = ctypes.windll.kernel32
PAGE_READWRITE = 0x04
FILE_MAP_ALL_ACCESS = 0xF001F

mapping = k32.CreateFileMappingW(
    ctypes.c_void_p(-1), None, PAGE_READWRITE, 0, 48, "Local\\HellwalkerHudState")
if not mapping:
    raise SystemExit("CreateFileMappingW failed")
view = k32.MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 48)
if not view:
    raise SystemExit("MapViewOfFile failed")

MAGIC = 0x48573131


def pack(score, target, charged, fraction, kill_seq):
    # Layout mirrors HudSharedState v2: magic, version, enabled, supercharged,
    # scorePoints(u64), targetScore(i32), scoreFraction(f32), heartbeat(u64),
    # killSeq(u64).
    return struct.pack(
        "<IIIIQifQQ",
        MAGIC, 2, 1, 1 if charged else 0,
        int(score), int(target), fraction, k32.GetTickCount64(), int(kill_seq),
    )


def publish(score, target, charged, kill_seq):
    frac = min(1.0, score / float(target)) if target else 0.0
    ctypes.memmove(view, pack(score, target, charged, frac, kill_seq), 48)


try:
    print("publishing fake HUD state; Ctrl+C to stop")
    kill_seq = 0
    while True:
        # charging phase: one "kill" every 500 ms
        for score in range(0, 2001, 100):
            kill_seq += 1
            publish(score, 2000, False, kill_seq)
            time.sleep(0.5)
        # charged phase: score frozen, but kills still flash the counter
        for _ in range(24):
            kill_seq += 1
            publish(2000, 2000, True, kill_seq)
            time.sleep(1.5)
except KeyboardInterrupt:
    pass
