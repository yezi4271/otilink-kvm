#!/usr/bin/env python3
"""find_calls.py —— 找出所有调用/跳转到某个函数或导入符号的位置（含经 __stubs/__got 的间接调用）。

用法:
  python find_calls.py <macho> --symbol _OTi_SendHIDPacket
  python find_calls.py <macho> --symbol _OTi_SendHIDPacket --ctx 30
  python find_calls.py <macho> --addr 0x3306
"""
import argparse
import struct
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from macho_disasm import MachO  # noqa: E402

from capstone import Cs, CS_ARCH_X86, CS_MODE_64  # noqa: E402


def enclosing_sym(m, addr):
    best = ("?", -1)
    for n, t, s, v in m.syms:
        if v <= addr and v > best[1]:
            best = (n, v)
    return best[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("macho")
    ap.add_argument("--symbol", default=None)
    ap.add_argument("--addr", default=None)
    ap.add_argument("--ctx", type=int, default=0)
    a = ap.parse_args()

    m = MachO(a.macho)
    name = a.symbol
    target = m.find_sym(name) if name else int(a.addr, 0)

    # 导入符号：__stubs 里的桩地址 + __got/__la_symbol_ptr 里的指针槽地址
    stubs, slots = set(), set()
    for sa, sn in m.slot_to_sym.items():
        if name and sn == name:
            sect = next((s[0] for s in m.sections if s[2] <= sa < s[2] + s[3]), "?")
            (stubs if sect in ("__stubs", "__auth_stubs") else slots).add(sa)

    print(f"# {a.macho}")
    print(f"# 目标 {name or hex(target)}: 定义地址={hex(target) if target else None} "
          f"桩={[hex(x) for x in sorted(stubs)]} 指针槽={[hex(x) for x in sorted(slots)]}")

    text = [s for s in m.sections if s[0] == "__text"][0]
    _, _, taddr, tsize, toff = text
    blob = m.data[toff:toff + tsize]

    hits = []
    for i in range(len(blob) - 6):
        op = blob[i]
        if op in (0xE8, 0xE9):
            dst = taddr + i + 5 + struct.unpack_from("<i", blob, i + 1)[0]
            if target and dst == target:
                hits.append((taddr + i, "call" if op == 0xE8 else "jmp", "直接"))
            elif dst in stubs:
                hits.append((taddr + i, "call" if op == 0xE8 else "jmp", f"经桩 0x{dst:x}"))
        elif op == 0xFF and blob[i + 1] in (0x15, 0x25):
            slot = taddr + i + 6 + struct.unpack_from("<i", blob, i + 2)[0]
            if slot in slots:
                hits.append((taddr + i, "call" if blob[i + 1] == 0x15 else "jmp",
                             f"经指针 0x{slot:x}"))

    if not hits:
        print("  未发现调用点")
    for addr, kind, how in hits:
        print(f"  0x{addr:06x}  {kind}  ({how})  in {enclosing_sym(m, addr)}")
        if a.ctx:
            off = m.addr_to_off(addr - a.ctx)
            md = Cs(CS_ARCH_X86, CS_MODE_64)
            for ins in md.disasm(m.data[off:off + a.ctx * 2], addr - a.ctx):
                mark = " <==" if ins.address == addr else ""
                print(f"      {ins.address:06x}  {ins.mnemonic:<7} {ins.op_str}{mark}")


if __name__ == "__main__":
    main()
