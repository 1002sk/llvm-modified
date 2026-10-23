# .llvm_addrmap を pyelftools だけで読む最小リーダ（.o とリンク済み ELF の両対応）
import io, json, struct, sys
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationHandler

HDR = struct.Struct("<4sHHIIII8s16s")   # 48 B
ENT = struct.Struct("<QII")             # 16 B

def read(path):
    with open(path, "rb") as f:
        elf = ELFFile(f)
        sec = elf.get_section_by_name(".llvm_addrmap")
        data = sec.data()
        relocatable = elf["e_type"] == "ET_REL"
        if relocatable:   # .o: 再配置を適用し、pc を「.text 内オフセット」にする
            stream = io.BytesIO(bytearray(data))
            rh = RelocationHandler(elf)
            rh.apply_section_relocations(stream, rh.find_relocations_for_section(sec))
            data = stream.getvalue()
        # 関数名の解決用にシンボル表を引く
        syms = [s for s in elf.get_section_by_name(".symtab").iter_symbols()
                if s["st_info"]["type"] == "STT_FUNC"]
        def func_of(pc):
            for s in syms:
                if s["st_value"] <= pc < s["st_value"] + s["st_size"]:
                    return f'{s.name}+{pc - s["st_value"]:#x}'
            return "?"
        off = 0
        while off + HDR.size <= len(data):
            magic, ver, hsz, usz, n, joff, jsz, _, tu = HDR.unpack_from(data, off)
            if magic != b"AMAP":            # 連結時の 0 埋めを読み飛ばす
                off += 8; continue
            lines = data[off+joff: off+joff+jsz].decode().splitlines()
            header, recs = json.loads(lines[0]), [json.loads(l) for l in lines[1:]]
            for i in range(n):
                pc, rid, flags = ENT.unpack_from(data, off + hsz + i * ENT.size)
                # --gc-sections で消えた関数（リンク済み ELF のみ。.o では 0 は正当な .text 先頭）
                if not relocatable and pc in (0, 0xffffffffffffffff, 0xfffffffffffffffe):
                    continue
                r = recs[rid]
                yield pc, func_of(pc), header["source"], r
            off += usz

if __name__ == "__main__":
    for pc, where, src, r in read(sys.argv[1]):
        if r["kind"] == "not_pointer":   # 仕様 4.2: not_pointer は insn と source のみ
            print(f'{pc:#010x}  {where:<14} {r["kind"]:<11} {r["insn"]["mnemonic"]}')
            continue
        print(f'{pc:#010x}  {where:<14} {r["kind"]:<7} {r["base"]["kind"]:<8} '
              f'{r["path_str"]:<12} {r["result_type"]:<14} '
              f'ranges={[(p.get("name") if p["op"] == "field" else f"[{'?' if p['index'] is None else p['index']}]", p["offset"], p["size"]) for p in r["path"]]}')
