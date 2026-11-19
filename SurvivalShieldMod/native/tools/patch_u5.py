#!/usr/bin/env python3
"""Hellwalker Update 5 unified disk patcher for DOOMTheDarkAges.exe.

Reads the clean vanilla Update 5 executable (read-only), applies the three
Atlan Mod Loader binary patches in memory, installs all ten static disk
thunks (4 UI bridge + 6 gameplay/diagnostic hooks) using unused .text padding
caves and .mydata slots, and writes the resulting image + manifest outside the
game and repository folders (by default under
%LOCALAPPDATA%\\DOOMTheDarkAges\\SurvivalShield\\exe-cache).

Slots remain zero on disk so an un-injected session runs identically to
Atlan-patched vanilla.
"""
import argparse
import hashlib
import json
import os
import re
import struct
import sys

EXPECTED_VANILLA_SHA256 = (
    "D998C7C3D7B7E2C767824E51621C7F0AA3CDAD3A5805BCDC7CD8C18F273E140E"
)


def default_out_dir() -> str:
    local_app = os.environ.get("LOCALAPPDATA")
    if local_app:
        return os.path.join(local_app, "DOOMTheDarkAges", "SurvivalShield", "exe-cache")
    script_dir = os.path.dirname(os.path.abspath(__file__))
    return os.path.abspath(os.path.join(script_dir, "..", "..", "exe-cache"))


def _read_steam_library_roots() -> list[str]:
    roots: list[str] = []
    try:
        import winreg
        for hkey, subkey, val in (
            (winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam", "SteamPath"),
            (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath"),
            (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Valve\Steam", "InstallPath"),
        ):
            try:
                with winreg.OpenKey(hkey, subkey) as k:
                    p, _ = winreg.QueryValueEx(k, val)
                    if isinstance(p, str) and p:
                        roots.append(os.path.normpath(p))
            except OSError:
                pass
    except ImportError:
        pass

    expanded = list(roots)
    for r in roots:
        vdf = os.path.join(r, "steamapps", "libraryfolders.vdf")
        if not os.path.isfile(vdf):
            continue
        try:
            with open(vdf, "r", encoding="utf-8", errors="ignore") as f:
                text = f.read()
            for m in re.finditer(r'"path"\s+"([^"]+)"', text):
                p = os.path.normpath(m.group(1).replace("\\\\", "\\"))
                if p not in expanded:
                    expanded.append(p)
        except OSError:
            pass
    return expanded


def find_vanilla_exe(explicit: str | None = None) -> str | None:
    if explicit:
        return explicit

    env_exe = os.environ.get("DOOM_DARK_AGES_EXE")
    if env_exe and os.path.isfile(env_exe):
        return env_exe

    env_dir = os.environ.get("DOOM_DARK_AGES_DIR")
    if env_dir:
        cand = os.path.join(env_dir, "DOOMTheDarkAges.exe")
        if os.path.isfile(cand):
            return cand

    for root in _read_steam_library_roots():
        cand = os.path.join(root, "steamapps", "common", "DOOMTheDarkAges", "DOOMTheDarkAges.exe")
        if os.path.isfile(cand):
            return cand

    for drive in ("C", "D", "E", "F", "G", "Z"):
        for base in (
            rf"{drive}:\SteamLibrary\steamapps\common\DOOMTheDarkAges",
            rf"{drive}:\Program Files (x86)\Steam\steamapps\common\DOOMTheDarkAges",
            rf"{drive}:\Program Files\Steam\steamapps\common\DOOMTheDarkAges",
        ):
            cand = os.path.join(base, "DOOMTheDarkAges.exe")
            if os.path.isfile(cand):
                return cand
    return None

MYDATA_RVA = 0x0B613000
PAD_BYTES = {0xCC, 0x90}

ATLAN_PATCHES = [
    (
        "Cvars & Launch Parameters",
        bytes.fromhex("CCCCCCCCCCCCCCCC40534883EC50488B842488000000488B"),
        bytes.fromhex("4183C91053EB0390EBF64883EC50488B842488000000488B"),
    ),
    (
        "Cvars & Console Commands",
        bytes.fromhex("084C8B0EBA01"),
        bytes.fromhex("084C8B0EBA00"),
    ),
    (
        "Resource Hash Checks",
        bytes.fromhex("741F418B564841B8EFBEADDE"),
        bytes.fromhex("EB1F418B564841B8EFBEADDE"),
    ),
]

# Update 5 RVAs and verified position-independent prologues.
HOOK_SPECS = [
    {
        "name": "get",
        "symbol": "GetSettingsDataValue",
        "entry": 0x01EAD5A0,
        "prologue": bytes.fromhex("4889742418"),
        "slot": MYDATA_RVA + 0x10,
    },
    {
        "name": "set",
        "symbol": "SetSettingsDataValue",
        "entry": 0x01EB4150,
        "prologue": bytes.fromhex("4889742418"),
        "slot": MYDATA_RVA + 0x18,
    },
    {
        "name": "addscore",
        "symbol": "AddScore",
        "entry": 0x019C4B90,
        "prologue": bytes.fromhex("405541554157"),
        "slot": MYDATA_RVA + 0x20,
    },
    {
        "name": "damage",
        "symbol": "idAI_DeferredDamage",
        "entry": 0x00EF9750,
        "prologue": bytes.fromhex("405553574154415541564157"),
        "slot": MYDATA_RVA + 0x28,
    },
    {
        "name": "parry",
        "symbol": "ShieldParry_Execute",
        "entry": 0x01AD76C0,
        "prologue": bytes.fromhex("4055535657415541564157"),
        "slot": MYDATA_RVA + 0x30,
    },
    {
        "name": "killed",
        "symbol": "idAI_Killed",
        "entry": 0x00EEFC10,
        "prologue": bytes.fromhex("48895C2418"),
        "slot": MYDATA_RVA + 0x38,
    },
    {
        "name": "hud",
        "symbol": "BroadcastNow",
        "entry": 0x004A4A80,
        "prologue": bytes.fromhex("48895C241855574157"),
        "slot": MYDATA_RVA + 0x40,
    },
    {
        "name": "packet",
        "symbol": "DamagePacketCtor",
        "entry": 0x0142DEC0,
        "prologue": bytes.fromhex("405553565741564157488DAC2418F3FFFF"),
        "slot": MYDATA_RVA + 0x48,
    },
    {
        "name": "radio_sync",
        "symbol": "RadioButtonSync",
        "entry": 0x01EB4A80,
        "prologue": bytes.fromhex("40535556574156"),
        "slot": MYDATA_RVA + 0x50,
    },
    {
        "name": "radio_apply",
        "symbol": "RadioButtonEvent0xC",
        "entry": 0x01EB2CF0,
        "prologue": bytes.fromhex("4055415441564157"),
        "slot": MYDATA_RVA + 0x58,
    },
]


def load_sections(exe: bytes) -> dict:
    head = exe[:0x1000]
    e_lfanew = struct.unpack_from("<I", head, 0x3C)[0]
    nsec = struct.unpack_from("<H", head, e_lfanew + 6)[0]
    opt_size = struct.unpack_from("<H", head, e_lfanew + 20)[0]
    first = e_lfanew + 24 + opt_size
    secs = {}
    for i in range(nsec):
        s = head[first + 40 * i : first + 40 * i + 40]
        name = s[:8].rstrip(b"\x00").decode("ascii", "replace")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", s, 8)
        secs[name] = (vaddr, vsize, rawptr, rawsize)
    return secs


def rva_to_off(secs: dict, rva: int) -> int:
    for vaddr, vsize, rawptr, rawsize in secs.values():
        if vaddr <= rva < vaddr + max(vsize, rawsize):
            return rawptr + (rva - vaddr)
    raise ValueError(f"rva 0x{rva:X} not mapped")


def off_to_rva(secs: dict, off: int) -> int:
    for vaddr, _vsize, rawptr, rawsize in secs.values():
        if rawptr <= off < rawptr + rawsize:
            return vaddr + (off - rawptr)
    raise ValueError(f"file offset 0x{off:X} not mapped")


def rel32(src_rva: int, dst_rva: int) -> bytes:
    d = dst_rva - (src_rva + 5)
    assert -(1 << 31) <= d < (1 << 31), f"rel32 out of range: 0x{src_rva:X} -> 0x{dst_rva:X}"
    return struct.pack("<i", d)


def build_dispatcher(disp_rva: int, slot_rva: int, tramp_rva: int) -> bytes:
    # cmp qword ptr [rip+d1], 0 ; jne +5 ; jmp trampoline ; jmp qword ptr [rip+d2]
    code = b"\x48\x83\x3D" + struct.pack("<i", slot_rva - (disp_rva + 8)) + b"\x00"
    code += b"\x75\x05"
    code += b"\xE9" + rel32(disp_rva + 10, tramp_rva)
    code += b"\xFF\x25" + struct.pack("<i", slot_rva - (disp_rva + 21))
    assert len(code) == 21
    return code


def build_trampoline(tramp_rva: int, displaced: bytes, entry_rva: int) -> bytes:
    return displaced + b"\xE9" + rel32(tramp_rva + len(displaced), entry_rva + len(displaced))


def find_padding_runs(data: bytearray, secs: dict) -> list[tuple[int, int]]:
    vaddr, _vsize, rawptr, rawsize = secs[".text"]
    text = data[rawptr : rawptr + rawsize]
    runs = []
    i = 0
    while i < len(text):
        if text[i] in PAD_BYTES:
            j = i
            while j < len(text) and text[j] in PAD_BYTES:
                j += 1
            if j - i >= 16:
                runs.append((vaddr + i, j - i))
            i = j
        else:
            i += 1
    return runs


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Build Hellwalker U5 patched exe cache")
    parser.add_argument("--vanilla", help="Path to clean vanilla DOOMTheDarkAges.exe")
    parser.add_argument("--out-dir", default=default_out_dir(), help="Output directory for exe cache")
    args = parser.parse_args(argv)

    vanilla_exe = find_vanilla_exe(args.vanilla)
    if not vanilla_exe or not os.path.exists(vanilla_exe):
        print(f"ERROR: vanilla EXE not found: {vanilla_exe or '(auto-detect failed; pass --vanilla)'}")
        return 1

    exe_cache_dir = os.path.abspath(args.out_dir)
    out_mod_exe = os.path.join(exe_cache_dir, "DOOMTheDarkAges.exe.u5.mod")
    out_manifest = os.path.join(exe_cache_dir, "u5_patch_manifest.json")

    with open(vanilla_exe, "rb") as f:
        raw = f.read()
    vanilla_sha = hashlib.sha256(raw).hexdigest().upper()
    if vanilla_sha != EXPECTED_VANILLA_SHA256:
        print(f"ERROR: vanilla SHA-256 mismatch: {vanilla_sha} != {EXPECTED_VANILLA_SHA256}")
        return 2

    data = bytearray(raw)
    secs = load_sections(data)
    if ".mydata" not in secs or secs[".mydata"][0] != MYDATA_RVA:
        print(f"ERROR: unexpected .mydata section: {secs.get('.mydata')}")
        return 3

    # 1) Apply Atlan binary patches and track their RVAs to keep caves clear.
    forbidden_rvas = set()
    for name, vanilla_pat, patch_pat in ATLAN_PATCHES:
        assert len(vanilla_pat) == len(patch_pat)
        first = data.find(vanilla_pat)
        if first == -1 or data.find(vanilla_pat, first + 1) != -1:
            print(f"ERROR: Atlan patch '{name}' did not match uniquely")
            return 4
        data[first : first + len(patch_pat)] = patch_pat
        rva = off_to_rva(secs, first)
        forbidden_rvas.add(rva)
        print(f"atlan: {name} @ RVA 0x{rva:08X}")

    for spec in HOOK_SPECS:
        forbidden_rvas.add(spec["entry"])

    # 2) Find padding caves AFTER Atlan patches are applied.
    runs = find_padding_runs(data, secs)

    def take_run(min_len: int) -> int:
        for aligned in (True, False):
            for idx, (rva, ln) in enumerate(runs):
                if ln < min_len:
                    continue
                if aligned and (rva & 0xF) != 0:
                    continue
                if any(abs(rva - u) <= 0x100 for u in forbidden_rvas):
                    continue
                forbidden_rvas.add(rva)
                runs.pop(idx)
                return rva
        raise RuntimeError(f"no .text padding cave >= {min_len} bytes available")

    def check_pad(rva: int, need: int) -> None:
        o = rva_to_off(secs, rva)
        chunk = data[o : o + need]
        if len(chunk) < need or any(b not in PAD_BYTES for b in chunk):
            raise RuntimeError(f"padding cave 0x{rva:X} unusable")

    # 3) Verify slots & prologues, then install the 8 thunks.
    manifest_hooks = {}
    for spec in HOOK_SPECS:
        name = spec["name"]
        entry_rva = spec["entry"]
        prol = spec["prologue"]
        slot_rva = spec["slot"]

        slot_off = rva_to_off(secs, slot_rva)
        if any(data[slot_off : slot_off + 8]):
            raise RuntimeError(f"{name}: slot 0x{slot_rva:08X} is not zero")

        entry_off = rva_to_off(secs, entry_rva)
        actual = bytes(data[entry_off : entry_off + len(prol)])
        if actual != prol:
            raise RuntimeError(
                f"{name}: prologue mismatch at 0x{entry_rva:08X}: {actual.hex()} != {prol.hex()}"
            )

        tramp_size = len(prol) + 5
        disp_rva = take_run(21)
        tramp_rva = take_run(tramp_size)
        check_pad(disp_rva, 21)
        check_pad(tramp_rva, tramp_size)

        data[entry_off : entry_off + 5] = b"\xE9" + rel32(entry_rva, disp_rva)
        disp_off = rva_to_off(secs, disp_rva)
        data[disp_off : disp_off + 21] = build_dispatcher(disp_rva, slot_rva, tramp_rva)
        tramp_off = rva_to_off(secs, tramp_rva)
        data[tramp_off : tramp_off + tramp_size] = build_trampoline(tramp_rva, prol, entry_rva)

        manifest_hooks[name] = {
            "symbol": spec["symbol"],
            "entry": f"0x{entry_rva:08X}",
            "disp": f"0x{disp_rva:08X}",
            "tramp": f"0x{tramp_rva:08X}",
            "slot": f"0x{slot_rva:08X}",
            "prologueBytes": len(prol),
        }
        print(
            f"{name:10s}: entry 0x{entry_rva:08X} -> disp 0x{disp_rva:08X} -> "
            f"tramp 0x{tramp_rva:08X}, slot 0x{slot_rva:08X}"
        )

    os.makedirs(exe_cache_dir, exist_ok=True)
    with open(out_mod_exe, "wb") as f:
        f.write(data)
    mod_sha = hashlib.sha256(data).hexdigest().upper()

    manifest = {
        "schema": 1,
        "targetBuild": "Update5_20261112",
        "vanillaSha256": vanilla_sha,
        "modSha256": mod_sha,
        "mydataRva": f"0x{MYDATA_RVA:08X}",
        "hooks": manifest_hooks,
    }
    with open(out_manifest, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    print("written:", out_mod_exe)
    print("manifest:", out_manifest)
    print("mod sha256:", mod_sha)
    return 0


if __name__ == "__main__":
    sys.exit(main())
