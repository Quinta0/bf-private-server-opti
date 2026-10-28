#!/usr/bin/env python3
"""Offline test of tools/patch_apk.py's libgame.so patches (no device, no server).

    python3 -I tools/test_patch_apk.py [--lib re/lib/libgame.so]

Needs the original v2.19.6 arm64 libgame.so (re/lib/libgame.so in the main
checkout, or unzip it from the APK) and Xcode's command line tools (as,
nm, otool, xcrun llvm-objdump).  Checks that:
  - tools/own_helper_cave.s still assembles to OWN_HELPER_CAVE_HEX, with the
    entry points patch_apk.py calls;
  - patch_lib applies every patch to the real library (original bytes match);
  - in the patched library every hook site calls its routine, and every call
    or jump the new code makes lands inside the cave or on the PLT stub of the
    function the source names.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import patch_apk  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'tools/own_helper_cave.s'

HOOKS = {
    0x18C8CA4: 'keep_own_helper',
    0x18C9710: 'rotation_friend_ok',
    0x18C9F5C: 'rotation_friend_ok',
    0x18AC208: 'refresh_picker_list',
    0x12741B0: 'refresh_lookup_x22',
    0x1274044: 'refresh_lookup_x21',
}

# What each PLT stub the cave calls must be (mangled names as objdump prints them).
PLT_STUBS = {
    0xC7BB60: '_ZN23MissionRepeatConfigInfo6sharedEv',
    0xC895E0: '_ZN23MissionRepeatConfigInfo19getReinforcementFlgEv',
    0xC298C0: '_ZN9UserState6sharedEv',
    0xC6B1D0: '_ZN21ReinforcementInfoList6sharedEv',
    0xC74060: '_ZN21ReinforcementInfoList8getCountEv',
    0xC498C0: '_ZN21ReinforcementInfoList9getObjectEi',
    0xC171F0: '_ZN21ReinforcementInfoList19getObjectWithUserIDENSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE',
    0xC74A60: '_ZN29MissionResultRepeatCheckScene20getRandReinforcementEv',
    0xC14330: '_ZN16UserUnitInfoList14sharedOriginalEv',
    0xC5D310: '_ZN16UserUnitInfoList23getObjectWithUserUnitIDENSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE',
    0xC39B80: '_ZN16UserUnitInfoBase4copyEPS_',
    0xC42460: '_ZNSt6__ndk112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6assignEPKcm',
    0xC52840: '_ZdlPv',
    0xC64030: '_ZN14FriendInfoList11existTypeOKENSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE',
}

failures = []


def check(ok: bool, what: str) -> None:
    print(('PASS ' if ok else 'FAIL ') + what)
    if not ok:
        failures.append(what)


def assemble(tmp: Path) -> tuple[bytes, dict]:
    obj = tmp / 'cave.o'
    subprocess.run(['as', '-arch', 'arm64', str(SOURCE), '-o', str(obj)], check=True)
    listing = subprocess.run(['otool', '-X', '-t', str(obj)], check=True, capture_output=True, text=True).stdout
    # "0000000000000000 a9be7bfd 94000400 ...": 16-digit addresses, then 32-bit words
    code = b''.join(int(token, 16).to_bytes(4, 'little') for token in listing.split()
                    if re.fullmatch(r'[0-9a-f]{8}', token))
    symbols = {}
    for line in subprocess.run(['nm', '-n', str(obj)], check=True, capture_output=True, text=True).stdout.splitlines():
        value, kind, name = line.split()
        if kind == 't':
            symbols[name] = int(value, 16)
    return code, symbols


def disassemble(lib: Path, start: int, end: int) -> list[tuple[int, str, str]]:
    out = subprocess.run(['xcrun', 'llvm-objdump', '-d', f'--start-address={start:#x}', f'--stop-address={end:#x}',
                          str(lib)], check=True, capture_output=True, text=True).stdout
    rows = []
    for line in out.splitlines():
        m = re.match(r'\s+([0-9a-f]+):\s+[0-9a-f]{8}\s+(\S+)\s*(.*)', line)
        if m:
            rows.append((int(m.group(1), 16), m.group(2), m.group(3)))
    return rows


def branch_target(operands: str) -> tuple[int, str]:
    m = re.match(r'(0x[0-9a-f]+)(?:\s+<(.*)>)?', operands)
    return int(m.group(1), 16), (m.group(2) or '')


def check_disjoint() -> None:
    """No two patches anywhere in patch_apk.py touch the same byte."""
    patches = [p for name, value in vars(patch_apk).items() if name.endswith('_PATCHES') for p in value]
    patches += patch_apk.own_helper_patches()
    spans = sorted((offset, offset + len(original), purpose) for offset, original, _new, purpose in patches)
    clashes = [(a, b) for a, b in zip(spans, spans[1:]) if b[0] < a[1]]
    check(not clashes, f'{len(spans)} patch sites are disjoint' + ''.join(f'\n     {a[2]} / {b[2]}' for a, b in clashes))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--lib', type=Path, default=ROOT / 're/lib/libgame.so')
    args = ap.parse_args()
    check_disjoint()
    if not args.lib.exists():
        print(f'SKIP: no {args.lib} (pass --lib, e.g. the main checkout\'s re/lib/libgame.so)')
        return 0

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        code, symbols = assemble(tmp)
        check(code == bytes.fromhex(patch_apk.OWN_HELPER_CAVE_HEX), 'own_helper_cave.s assembles to OWN_HELPER_CAVE_HEX')
        for entry, offset in patch_apk.OWN_HELPER_ENTRY.items():
            check(symbols.get(entry) == offset, f'{entry} is at cave+{offset:#x}')

        original = args.lib.read_bytes()
        patched = patch_apk.patch_lib(original, '127.0.0.1:9960')
        check(True, 'patch_lib applied every patch to the original library')
        cave, cave_end = patch_apk.OWN_HELPER_CAVE, patch_apk.OWN_HELPER_CAVE + len(code)
        check(patched[0x186B4CC:0x186B4D8] == original[0x186B4CC:0x186B4D8], 'erratum veneer before the cave untouched')
        check(all(b == 0 for b in patched[cave_end:patch_apk.OWN_HELPER_CAVE_END]), 'rest of the cave still zero')
        lib = tmp / 'libgame.so'
        lib.write_bytes(patched)

        # Vortex scene exit: the 115 fallback branches into the cave, which rebuilds x28
        # and enters the "area dungeon list" block; the veneer before the cave is intact.
        v_cave = patch_apk.VORTEX_SCENE_CAVE
        check(original[v_cave - 12:v_cave] == patched[v_cave - 12:v_cave], 'erratum veneer before the Vortex cave untouched')
        check(original[v_cave:v_cave + 0x40] == bytes(0x40), 'Vortex cave was an unused zero block')
        (_, op, operands), = disassemble(lib, patch_apk.VORTEX_SCENE_FALLBACK, patch_apk.VORTEX_SCENE_FALLBACK + 4)
        check(op == 'b' and branch_target(operands)[0] == v_cave, 'the Vortex-top fallback branches into the cave')
        (_, op1, ops1), (_, op2, ops2) = disassemble(lib, v_cave, v_cave + 8)
        check(op1 == 'add' and ops1.replace(' ', '') == 'x28,x19,#0x350', f'cave sets x28 = this + 0x350 ({op1} {ops1})')
        check(op2 == 'b' and branch_target(ops2)[0] == patch_apk.VORTEX_SCENE_AREA_LIST, 'cave jumps to the area dungeon list block')
        (_, op, operands), = disassemble(lib, patch_apk.VORTEX_SCENE_AREA_LIST, patch_apk.VORTEX_SCENE_AREA_LIST + 4)
        check(op == 'mov' and operands.replace(' ', '').startswith('w0,#0x3c0'), 'the block starts with new DungeonSPSelectScene2 (0x3c0 bytes)')

        for site, entry in HOOKS.items():
            (_, op, operands), = disassemble(lib, site, site + 4)
            target, _ = branch_target(operands)
            check(op == 'bl' and target == cave + symbols[entry], f'{site:#x} calls {entry}')

        for at, op, operands in disassemble(lib, cave, cave_end):
            if op not in ('b', 'bl'):
                continue
            target, name = branch_target(operands)
            if cave <= target < cave_end:
                continue
            expected = PLT_STUBS.get(target)
            check(expected is not None and name == f'{expected}@plt', f'{at:#x} {op} {target:#x} is {expected}@plt')

    print('all passed' if not failures else f'{len(failures)} failed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
