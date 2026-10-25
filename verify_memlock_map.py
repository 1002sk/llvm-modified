# .memlock_map の受け入れチェック（仕様 9.3 の 1〜4）
#   python3 verify_memlock_map.py NONE.o LOCK.o ALL.o
# NONE.o は本機能なし、LOCK.o は -fmemlock-map、ALL.o は -fmemlock-map=all で
# 同じ入力・同じオプションからコンパイルした .o。問題がなければ終了コード 0。
import shutil, subprocess, sys
from elftools.elf.elffile import ELFFile
from read_memlock_map import SECTION, read

import capstone
from capstone import x86 as csx86

def is_map(name):
    return name in (SECTION, ".rela" + SECTION, ".rel" + SECTION)

def uleb_list(data):
    out, val, shift = [], 0, 0
    for b in data:
        val |= (b & 0x7f) << shift; shift += 7
        if not b & 0x80:
            out.append(val); val = shift = 0
    return out

class Obj:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        self.elf = ELFFile(self.f)
        self.secs = list(self.elf.iter_sections())
        self.symtab = self.elf.get_section_by_name(".symtab")
        self.syms = list(self.symtab.iter_symbols()) if self.symtab else []

    def secname(self, idx):
        return self.secs[idx].name if isinstance(idx, int) and idx < len(self.secs) else str(idx)

    def symkey(self, i):
        """シンボル番号を、本機能の有無で変わらない名前に変換する"""
        s = self.syms[i]
        if s["st_info"]["type"] == "STT_SECTION":
            return ("section", self.secname(s["st_shndx"]))
        return ("sym", s.name)

    def symbols(self):
        out, sections = [], set()
        for s in self.syms:
            if s["st_info"]["type"] == "STT_SECTION":
                sections.add(self.secname(s["st_shndx"]))
                continue
            out.append((s.name, s["st_value"], s["st_size"], s["st_info"]["type"],
                        s["st_info"]["bind"], s["st_other"]["visibility"],
                        self.secname(s["st_shndx"])))
        return out, sections

    def relocs(self, sec):
        return [(r["r_offset"], r["r_info_type"], self.symkey(r["r_info_sym"]),
                 r["r_addend"] if sec.is_RELA() else None)
                for r in sec.iter_relocations()]

    def group(self, sec):
        data = sec.data()   # フラグ（4 バイト）の後に構成セクションの番号が続く
        idx = [int.from_bytes(data[i:i + 4], "little") for i in range(4, len(data), 4)]
        return [self.secname(i) for i in idx if not is_map(self.secname(i))]

    def addrsig(self, sec):
        return [self.symkey(i) for i in uleb_list(sec.data())]

    def contents(self):
        """本セクション以外のセクションを、比較できる形にして並べる"""
        out = []
        for sec in self.secs:
            if is_map(sec.name):
                continue
            t = sec["sh_type"]
            if t in ("SHT_SYMTAB", "SHT_STRTAB") and sec.name in (".symtab", ".strtab"):
                v = None   # .symtab は別に比べる、.strtab は比べない
            elif t in ("SHT_RELA", "SHT_REL"):
                v = self.relocs(sec)
            elif t == "SHT_GROUP":
                v = self.group(sec)
            elif t in ("SHT_LLVM_ADDRSIG", 0x6FFF4C03):   # pyelftools は名前を知らない
                v = self.addrsig(sec)
            elif t == "SHT_NOBITS":
                v = sec["sh_size"]
            else:
                v = sec.data()
            out.append((sec.name, t, v))
        return out

def check_identical(none, other, errs):
    a, b = none.contents(), other.contents()
    if [(n, t) for n, t, _ in a] != [(n, t) for n, t, _ in b]:
        errs.append(f"{other.path}: section list differs")
        return
    for (n, t, va), (_, _, vb) in zip(a, b):
        if va != vb:
            errs.append(f"{other.path}: section {n} ({t}) differs")
    sa, seca = none.symbols()
    sb, secb = other.symbols()
    if sa != sb:
        errs.append(f"{other.path}: symbols differ")
    if not seca <= secb:
        errs.append(f"{other.path}: section symbols removed: {sorted(seca - secb)}")

def code_sections(obj):
    return {i: s for i, s in enumerate(obj.secs) if s["sh_flags"] & 0x4}   # SHF_EXECINSTR

def objdump_starts(path):
    objdump = shutil.which("llvm-objdump") or "build/bin/llvm-objdump"
    out = subprocess.run([objdump, "-d", "--no-show-raw-insn", path],
                         capture_output=True, text=True, check=True).stdout
    starts, sec = set(), None
    for line in out.splitlines():
        if line.startswith("Disassembly of section "):
            sec = line[len("Disassembly of section "):].rstrip(":")
            continue
        head, sep, _ = line.partition(":")
        h = head.strip()
        if sep and h and all(c in "0123456789abcdef" for c in h):
            starts.add((sec, int(h, 16)))
    return starts

# 暗黙にスタックやメモリに触る命令（明示的なメモリオペランドなしでも対象）
IMPLICIT = {"push", "pushq", "pushfq", "pop", "popq", "popfq", "call", "ret",
            "leave", "enter", "movsb", "movsw", "movsd", "movsq", "stosb",
            "stosw", "stosd", "stosq", "lodsb", "lodsw", "lodsd", "lodsq",
            "cmpsb", "cmpsw", "cmpsd", "cmpsq", "scasb", "scasw", "scasd",
            "scasq", "xlatb", "push2", "push2p", "pop2", "pop2p", "pushp", "popp"}
# メモリオペランドの形でもメモリに触らない命令
NO_ACCESS = {"lea", "nop", "nopw", "nopl"}

def touches_memory(insn):
    m = insn.mnemonic.split()[-1]   # rep などのプレフィックスを除く
    if m in NO_ACCESS:
        return False
    if m in IMPLICIT or m.startswith(("call", "ret")):
        return True
    return any(op.type == csx86.X86_OP_MEM for op in insn.operands)

def check_records(obj, errs, all_mode):
    starts = objdump_starts(obj.path)
    recs = list(read(obj.path))
    names = {s.name: s for s in obj.secs}
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    for r in recs:
        if bool(r["flags"] & 1) != all_mode:
            errs.append(f'{obj.path}: {r["name"]}: wrong mode flag')
        offs = [e[0] for e in r["entries"]]
        if offs != sorted(set(offs)):
            errs.append(f'{obj.path}: {r["name"]}: offsets not strictly increasing')
        for o, kind, length, reason in r["entries"]:
            if (r["text"], r["base"] + o) not in starts:
                errs.append(f'{obj.path}: {r["name"]}+{o:#x}: not an instruction start')
            if not all_mode and reason & 0x80:
                errs.append(f'{obj.path}: {r["name"]}+{o:#x}: no-lock entry in lock mode')
        if not all_mode:
            continue
        # 9.3-3: 網羅性。Capstone で逆アセンブルし、メモリに触る命令がすべて載っているか
        listed = {o for o, k, _, _ in r["entries"] if k == 0}
        ranges = [(o, o + l) for o, k, l, _ in r["entries"] if k == 1]
        code = names[r["text"]].data()[r["base"]:r["base"] + r["size"]]
        for insn in md.disasm(code, 0):
            o = insn.address
            if touches_memory(insn) and o not in listed and \
               not any(b <= o < e for b, e in ranges):
                errs.append(f'{obj.path}: {r["name"]}+{o:#x}: memory access not listed: '
                            f'{insn.mnemonic} {insn.op_str}')
    return recs

def main():
    none, lock, all_ = (Obj(p) for p in sys.argv[1:4])
    errs = []
    check_identical(none, lock, errs)                 # 9.3-1
    check_identical(none, all_, errs)
    lrecs = check_records(lock, errs, False)          # 9.3-2
    arecs = check_records(all_, errs, True)           # 9.3-2, 9.3-3
    key = lambda recs, f: {(r["text"], r["base"], e) for r in recs
                           for e in r["entries"] if f(e)}
    if key(lrecs, lambda e: True) != key(arecs, lambda e: not e[3] & 0x80):
        errs.append("lock mode entries differ from the lock entries of all mode")  # 9.3-4
    for e in errs:
        print(e)
    n = sum(len(r["entries"]) for r in arecs)
    print(f"{'FAIL' if errs else 'OK'}: {len(arecs)} records, {n} entries (all mode)")
    sys.exit(1 if errs else 0)

if __name__ == "__main__":
    main()
