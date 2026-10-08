#!/usr/bin/env python3
"""scan_cdb.py —— 扫描 Mach-O 的 __text，聚类"往栈/结构体写立即数"的指令，找出协议常量。

用途：逆向厂商软件里以栈上字节数组形式构造的 SCSI CDB、帧头等常量。
输出：按相邻性聚类，标注所在函数（最近的符号）与文件偏移。

用法: /tmp/capvenv/bin/python scan_cdb.py <macho> [--min N] [--window W] [--all]
"""
import argparse
import re
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from macho_disasm import MachO  # noqa: E402

from capstone import Cs, CS_ARCH_X86, CS_MODE_64  # noqa: E402

IMM_RE = re.compile(r"^(?:mov|movabs)\s+(byte|word|dword|qword) ptr \[([^\]]+)\],\s*(0x[0-9a-f]+|\d+)$")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("macho")
    ap.add_argument("--min", type=int, default=2, help="一个聚类至少几条常量写入")
    ap.add_argument("--window", type=int, default=24, help="指令间距超过此值就断开聚类")
    ap.add_argument("--all", action="store_true", help="不聚类，逐条打印")
    a = ap.parse_args()

    m = MachO(a.macho)
    text = [s for s in m.sections if s[0] == "__text"]
    if not text:
        sys.exit("没有 __text")
    _, _, addr, size, off = text[0]
    code = m.data[off:off + size]
    syms = sorted([(v, n) for n, t, s, v in m.syms])

    def func_of(x):
        lo, hi = 0, len(syms) - 1
        best = "?"
        while lo <= hi:
            mid = (lo + hi) // 2
            if syms[mid][0] <= x:
                best = syms[mid][1]
                lo = mid + 1
            else:
                hi = mid - 1
        return best

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    hits = []
    idx = 0
    for ins in md.disasm(code, addr):
        idx += 1
        mt = IMM_RE.match(f"{ins.mnemonic} {ins.op_str}")
        if mt:
            width, dst, imm = mt.group(1), mt.group(2), int(mt.group(3), 0)
            # 只关心可能构成协议常量的写入（跳过大栈偏移的普通初始化）
            if 0x20 <= imm <= 0xFFFF:
                hits.append((ins.address, idx, width, dst, imm, ins.mnemonic + " " + ins.op_str))

    print(f"# {a.macho}: __text {size} bytes @0x{addr:x}, 常量写入 {len(hits)} 条")
    if a.all:
        for h in hits:
            print(f"  0x{h[0]:06x}  {h[5]}")
        return

    cluster = []
    prev = None
    clusters = []
    for h in hits:
        if prev is not None and h[1] - prev > a.window:
            clusters.append(cluster)
            cluster = []
        cluster.append(h)
        prev = h[1]
    if cluster:
        clusters.append(cluster)

    for c in clusters:
        if len(c) < a.min:
            continue
        print(f"\n--- 函数 {func_of(c[0][0])}  (+0x{c[0][0]:x})  {len(c)} 条常量 ---")
        for x in c:
            print(f"   0x{x[0]:06x}  {x[5]}")


if __name__ == "__main__":
    main()
