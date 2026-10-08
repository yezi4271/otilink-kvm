#!/usr/bin/env python3
"""macho_disasm.py —— 从 Mach-O 里挑函数反汇编，用于逆向 OTiTransfer 的 SCSI 传输层。

用法:
  /tmp/capvenv/bin/python macho_disasm.py <macho> --imports          # 列出导入符号（找 SCSITask/IOUSB API）
  /tmp/capvenv/bin/python macho_disasm.py <macho> --symbols OTi_     # 列出定义的符号
  /tmp/capvenv/bin/python macho_disasm.py <macho> --disasm _OTi_SendData [--bytes 400]

不依赖 otool/llvm-objdump：自己解析 LC_SEGMENT_64 / LC_SYMTAB，再用 capstone 反汇编。
"""
import argparse
import struct
import sys

try:
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
except ImportError:
    sys.exit("需要 capstone：python3 -m venv /tmp/capvenv && /tmp/capvenv/bin/pip install capstone")

MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x02
LC_DYSYMTAB = 0x0B

# section flags 低 8 位：这些类型的 section 通过 indirect symbol table 关联符号
S_INDIRECT_TYPES = {0x6, 0x7, 0x8, 0x10}

# SCSI CDB 构造常见的立即数写入指令（x86-64）：
#   C6 /0 ib   mov byte ptr [r/m+disp], imm8
#   C7 /0 id   mov dword ptr [r/m+disp], imm32
#   B0+r ib    mov r8b, imm8
# 我们把这类"往栈上写常量"的指令标出来，CDB 就在其中。


class MachO:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, res = struct.unpack_from(
            "<8I", self.data, 0)
        if magic != MH_MAGIC_64:
            raise SystemExit(f"不是 64 位小端 Mach-O（magic=0x{magic:x}）")
        self.filetype = filetype
        self.segments = []      # (segname, vmaddr, vmsize, fileoff, filesize)
        self.sections = []      # (sectname, segname, addr, size, offset)
        self.syms = []          # (name, n_type, n_sect, n_value)
        self.imports = []       # 未定义符号名
        self.dysymtab = None    # (indirectsymoff, nindirectsyms)
        self.sect_flags = []    # 与 sections 平行：(flags, reserved1)
        self.slot_to_sym = {}   # __got/__la_symbol_ptr/__stubs 地址 -> 符号名

        off = 32
        symtab = None
        for _ in range(ncmds):
            cmd, cmdsize = struct.unpack_from("<2I", self.data, off)
            if cmd == LC_SEGMENT_64:
                segname = self.data[off + 8:off + 24].rstrip(b"\0").decode(errors="replace")
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<4Q", self.data, off + 24)
                self.segments.append((segname, vmaddr, vmsize, fileoff, filesize))
                nsects = struct.unpack_from("<I", self.data, off + 64)[0]
                soff = off + 72
                for _s in range(nsects):
                    sectname = self.data[soff:soff + 16].rstrip(b"\0").decode(errors="replace")
                    ssegname = self.data[soff + 16:soff + 32].rstrip(b"\0").decode(errors="replace")
                    addr, size = struct.unpack_from("<2Q", self.data, soff + 32)
                    offset = struct.unpack_from("<I", self.data, soff + 48)[0]
                    sflags = struct.unpack_from("<I", self.data, soff + 64)[0]
                    reserved1 = struct.unpack_from("<I", self.data, soff + 68)[0]
                    reserved2 = struct.unpack_from("<I", self.data, soff + 72)[0]
                    self.sections.append((sectname, ssegname, addr, size, offset))
                    self.sect_flags.append((sflags, reserved1, reserved2))
                    soff += 80
            elif cmd == LC_DYSYMTAB:
                indirectsymoff = struct.unpack_from("<I", self.data, off + 56)[0]
                nindirectsyms = struct.unpack_from("<I", self.data, off + 60)[0]
                self.dysymtab = (indirectsymoff, nindirectsyms)
            elif cmd == LC_SYMTAB:
                symtab = struct.unpack_from("<4I", self.data, off + 8)
            off += cmdsize

        if symtab:
            symoff, nsyms, stroff, strsize = symtab
            strtab = self.data[stroff:stroff + strsize]
            self.all_syms = []
            for i in range(nsyms):
                n_strx, n_type, n_sect, n_desc, n_value = struct.unpack_from(
                    "<IBBHQ", self.data, symoff + i * 16)
                name = strtab[n_strx:strtab.find(b"\0", n_strx)].decode(errors="replace")
                self.all_syms.append(name)
                if (n_type & 0x0E) == 0x00:      # N_UNDF = 导入
                    self.imports.append(name)
                else:
                    self.syms.append((name, n_type, n_sect, n_value))

        # __got / __la_symbol_ptr / __stubs 的地址 -> 符号名（经 indirect symbol table）
        if self.dysymtab and hasattr(self, "all_syms"):
            ioff, nind = self.dysymtab
            ind = list(struct.unpack_from(f"<{nind}I", self.data, ioff)) if nind else []
            for (sectname, segname, addr, size, offset), (sflags, r1, r2) in zip(
                    self.sections, self.sect_flags):
                if (sflags & 0xFF) not in S_INDIRECT_TYPES:
                    continue
                entsize = r2 if (sflags & 0xFF) == 0x8 else 8
                if entsize == 0:
                    continue
                for i in range(size // entsize):
                    k = r1 + i
                    if k >= len(ind):
                        break
                    si = ind[k]
                    if si & 0x80000000 or si >= len(self.all_syms):   # INDIRECT_SYMBOL_LOCAL/ABS
                        continue
                    nm = self.all_syms[si]
                    if nm:
                        self.slot_to_sym[addr + i * entsize] = nm

    def addr_to_off(self, addr):
        for sectname, segname, a, size, offset in self.sections:
            if a <= addr < a + size:
                return offset + (addr - a)
        for segname, vmaddr, vmsize, fileoff, filesize in self.segments:
            if vmaddr <= addr < vmaddr + filesize:
                return fileoff + (addr - vmaddr)
        return None

    def find_sym(self, name):
        for n, t, s, v in self.syms:
            if n == name:
                return v
        return None


def disasm(m, name, nbytes):
    addr = m.find_sym(name)
    if addr is None:
        print(f"找不到符号 {name}（试试 --symbols 前缀过滤）")
        return
    off = m.addr_to_off(addr)
    code = m.data[off:off + nbytes]
    print(f"=== {name} @ 0x{addr:x} (file 0x{off:x}, {len(code)} bytes) ===")
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    for ins in md.disasm(code, addr):
        line = f"  {ins.address:08x}  {ins.mnemonic:<7} {ins.op_str}"
        # 注解经桩调用的真实导入名
        for op in ins.operands:
            if op.type == 2:            # X86_OP_IMM
                sym = m.slot_to_sym.get(op.imm)
                if sym:
                    line += f"   ; {sym}"
                    break
        print(line)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("macho")
    ap.add_argument("--imports", action="store_true")
    ap.add_argument("--grep", default=None, help="配合 --imports 过滤")
    ap.add_argument("--symbols", default=None, help="列出定义符号（前缀过滤）")
    ap.add_argument("--disasm", default=None)
    ap.add_argument("--bytes", type=int, default=320)
    a = ap.parse_args()

    m = MachO(a.macho)
    print(f"# {a.macho}: segments={len(m.segments)} sections={len(m.sections)} "
          f"defined={len(m.syms)} imports={len(m.imports)}")

    if a.imports:
        pat = (a.grep or "").lower()
        for n in sorted(set(m.imports)):
            if pat in n.lower():
                print("  " + n)
    if a.symbols is not None:
        for n, t, s, v in sorted(m.syms, key=lambda x: x[3]):
            if a.symbols in n:
                print(f"  0x{v:08x}  {n}")
    if a.disasm:
        disasm(m, a.disasm, a.bytes)


if __name__ == "__main__":
    main()
