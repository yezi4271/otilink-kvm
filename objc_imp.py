#!/usr/bin/env python3
"""objc_imp.py —— 在 Mach-O 里按 selector 名字找 Objective-C 方法的 IMP 地址。

做法：selector 名在 __objc_methname，方法的 method_t 结构 {name*, types*, imp} 在 __objc_const。
所以：定位名字字符串的 vmaddr -> 在 __objc_const 里找指向它的 8 字节指针 -> 其后第 3 个 qword 就是 imp。

用法:
  python objc_imp.py <macho> --list sendKeyData sendMouseData
  python objc_imp.py <macho> --imp sendKeyData      # 直接给地址，配合 macho_disasm.py --disasm 用不了，
                                                    # 可用 --dump 反汇编该 IMP
"""
import argparse
import struct
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from macho_disasm import MachO  # noqa: E402

from capstone import Cs, CS_ARCH_X86, CS_MODE_64  # noqa: E402


def section(m, name, seg=None):
    for sn, sg, addr, size, off in m.sections:
        if sn == name and (seg is None or sg == seg):
            return addr, size, off
    return None


def find_imps(m, names):
    meth = section(m, "__objc_methname")
    const = section(m, "__objc_const")
    if not meth or not const:
        return {}
    maddr, msize, moff = meth
    caddr, csize, coff = const
    blob = m.data[moff:moff + msize]
    cblob = m.data[coff:coff + csize]

    out = {}
    for want in names:
        idx = blob.find(want.encode() + b"\0")
        if idx < 0:
            continue
        name_va = maddr + idx
        # 在 __objc_const 里找指向 name_va 的指针（8 字节对齐）
        pat = struct.pack("<Q", name_va)
        pos = 0
        while True:
            pos = cblob.find(pat, pos)
            if pos < 0:
                break
            if pos % 8 == 0 and pos + 24 <= len(cblob):
                types_va, imp = struct.unpack_from("<2Q", cblob, pos + 8)
                out[want] = (imp, name_va, caddr + pos, types_va)
                break
            pos += 1
    return out


def dump(m, addr, nbytes):
    off = m.addr_to_off(addr)
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    print(f"=== 0x{addr:x} ===")
    for ins in md.disasm(m.data[off:off + nbytes], addr):
        print(f"  {ins.address:08x}  {ins.mnemonic:<7} {ins.op_str}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("macho")
    ap.add_argument("--list", nargs="*", default=[])
    ap.add_argument("--imp", default=None)
    ap.add_argument("--bytes", type=int, default=400)
    a = ap.parse_args()

    m = MachO(a.macho)
    if a.imp:
        res = find_imps(m, [a.imp])
        if not res:
            sys.exit(f"没找到 selector {a.imp}")
        dump(m, res[a.imp][0], a.bytes)
        return

    res = find_imps(m, a.list)
    for k, (imp, name_va, rec_va, types_va) in sorted(res.items()):
        off = m.addr_to_off(imp)
        if off is None:
            print(f"  {k:<28} IMP=0x{imp:x} (地址不在任何 section，疑似误匹配；method_t@0x{rec_va:x})")
            continue
        print(f"  {k:<28} IMP=0x{imp:x} (file 0x{off:x})  method_t@0x{rec_va:x}")
    missing = [x for x in a.list if x not in res]
    if missing:
        print("  未找到: " + ", ".join(missing))


if __name__ == "__main__":
    main()
