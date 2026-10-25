# .memlock_map を pyelftools だけで読む最小リーダ（.o とリンク済み ELF の両対応）
#   python3 read_memlock_map.py a.out [--disasm]
# --disasm を付けると llvm-objdump で各エントリの命令を表示する。
import io, shutil, struct, subprocess, sys
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationHandler

SECTION = ".memlock_map"
REASONS = {
    0x01: "no_mmo", 0x02: "not_alloca", 0x03: "captured", 0x05: "inline_asm",
    0x06: "other_psv", 0x07: "mc_layer", 0x09: "unexpected",
    0x81: "local", 0x82: "stack_slot", 0x83: "sp_implicit", 0x84: "read_only",
}

def uleb(data, off):
    val = shift = 0
    while True:
        b = data[off]; off += 1
        val |= (b & 0x7f) << shift; shift += 7
        if not b & 0x80:
            return val, off

def parse(data):
    """1 セクション分のバイト列からレコードを順に返す"""
    off = 0
    while off < len(data):
        ver, flags = data[off], data[off + 1]
        if ver != 1:   # 仕様 8.4: 未知のバージョンはモジュール全体をロック必要扱い
            raise ValueError(f"unknown version {ver} at {off:#x}")
        base, = struct.unpack_from("<Q", data, off + 2)
        off += 10
        size, off = uleb(data, off)
        nlen, off = uleb(data, off)
        name = data[off:off + nlen].decode(); off += nlen
        n, off = uleb(data, off)
        ents = []
        for _ in range(n):
            o, off = uleb(data, off)
            kind = data[off]; off += 1
            length = None
            if kind == 1:
                length, off = uleb(data, off)
            reason = data[off]; off += 1
            ents.append((o, kind, length, reason))
        yield dict(flags=flags, base=base, size=size, name=name, entries=ents)

def read(path):
    with open(path, "rb") as f:
        elf = ELFFile(f)
        relocatable = elf["e_type"] == "ET_REL"
        rh = RelocationHandler(elf) if relocatable else None
        # .memlock_map はテキストセクションごとにあり名前が同じなので、
        # 再配置セクションは名前ではなく sh_info（適用先の番号）で対応づける
        relsecs = {s["sh_info"]: s for s in elf.iter_sections()
                   if s["sh_type"] in ("SHT_RELA", "SHT_REL")}
        for idx, sec in enumerate(elf.iter_sections()):
            if sec.name != SECTION:
                continue
            data = sec.data()
            text = elf.get_section(sec["sh_link"]).name if sec["sh_link"] else "?"
            if relocatable:   # .o: 再配置を適用し、base を「リンク先セクション内オフセット」にする
                stream = io.BytesIO(bytearray(data))
                rels = relsecs.get(idx)
                if rels is not None:
                    rh.apply_section_relocations(stream, rels)
                data = stream.getvalue()
            for r in parse(data):
                r["text"] = text
                yield r

def disasm(path):
    """アドレス -> 逆アセンブル文字列（.o ではセクション内オフセットをキーにする）"""
    objdump = shutil.which("llvm-objdump") or "build/bin/llvm-objdump"
    out = subprocess.run([objdump, "-d", "--no-show-raw-insn", path],
                         capture_output=True, text=True, check=True).stdout
    insns, sec = {}, None
    for line in out.splitlines():
        if line.startswith("Disassembly of section "):
            sec = line[len("Disassembly of section "):].rstrip(":")
            continue
        head, sep, rest = line.partition(":")
        if sep and head.strip() and all(c in "0123456789abcdef" for c in head.strip()):
            insns[(sec, int(head, 16))] = rest.strip()
    return insns

if __name__ == "__main__":
    path = sys.argv[1]
    with open(path, "rb") as f:
        relocatable = ELFFile(f)["e_type"] == "ET_REL"
    insns = disasm(path) if "--disasm" in sys.argv else {}
    for r in read(path):
        mode = "all" if r["flags"] & 1 else "lock"
        print(f'{r["name"]}  [{r["text"]}]  base={r["base"]:#x} size={r["size"]:#x} '
              f'mode={mode} entries={len(r["entries"])}')
        for o, kind, length, reason in r["entries"]:
            lock = "nolock" if reason & 0x80 else "LOCK"
            rng = f" len={length:#x}" if kind == 1 else ""
            addr = r["base"] + o
            insn = insns.get((r["text"], addr), "") if insns else ""
            print(f'  +{o:#06x} ({addr:#x})  {lock:<6} {reason:#04x} '
                  f'{REASONS.get(reason, "?"):<12}{rng}  {insn}')
