# addrmap の仕組み（概要）

`-faddrmap` を付けてコンパイルすると、**「アドレスを作る機械語命令」ごとに、それがソース上のどの式
（`p->items[i].pos` など）のアドレスなのか**を書いた表が、オブジェクトファイルの `.llvm_addrmap`
セクションに入る。本書では、その仕組みを図と例で簡単に説明する。
細かい規則や判断の理由は [design.md](design.md) を参照。

## 1. 何ができるか（例）

```c
struct item { int id; int pos; int val; };
struct box  { int n; struct item items[8]; };
struct box g;

int *f(struct box *p, long i) { return &p->items[i].pos; }
int *h(void)                  { return &g.items[3].val; }
```

```sh
clang -g -O2 -faddrmap -c am.c
```

生成されたコードと、各命令に付いたレコード（JSON の主要なフィールドだけ抜き出したもの）:

```
f:
  0: leaq (%rsi,%rsi,2), %rax   → not_pointer   i*3 の整数計算（ポインタではない）
  4: leaq (%rdi,%rax,4), %rax   → derive        p->items[?].pos の途中の値（intermediate_of: 2）
  8: addq $0x8, %rax            → derive        p->items[?].pos   （src_ptr = p, const = 8, var = 12*?）
  c: retq
h:
 10: leaq g+48(%rip), %rax      → addr          g.items[3].val    （base = global g）
```

ポイント:

- 最適化で式がばらばらの命令（`lea` → `lea` → `add`）に分かれても、最後の `addq` に
  `p->items[?].pos` が付き、途中の `lea` には「その途中の値である」ことが記録される。
- 添字が実行時に決まる部分は `?` になる。定数の部分（`g.items[3].val`）はそのまま出る。
- `i*3` のような、`lea` を使っただけの整数計算は `not_pointer` と区別される。

## 2. 全体の流れ

情報は 3 段階で集めて、最後に 1 つにまとめる。

```
 C ソース
   │  clang -faddrmap -g
   ▼
 LLVM IR ───────────── 通常の最適化（変更なし）
   │
   ▼
 ① AddrMapAnnotate（IR パス、ISel の直前）
   │   GEP などに「このポインタは何か」を !addrmap メタデータで付ける
   ▼
 ② ISel（SelectionDAG / FastISel）
   │   !addrmap を MachineInstr に運ぶ（MachineInstr::ExtraInfo::AddrMap）
   │   以後の MI パスで命令が作り直されても引き継ぐ
   ▼
 ③ X86AsmPrinter（命令を出力するところ）
   │   X86AddrMap::handleInstruction
   │     ・対象命令（LEA、ADD、GOT からの MOV など）か判定
   │     ・命令の直前にラベル .LaddrmapN を置く
   │     ・機械語オペランド + IR 注釈 + デバッグ情報（DI 型）からレコードを作る
   ▼
 doFinalization → X86AddrMap::emitSection → .llvm_addrmap セクション
```

IR やコードそのものは一切変えない（`-faddrmap` の有無で `.text` はバイト単位で一致する）。
メタデータを足して、それを最後まで運んでいるだけである。

## 3. 各段階の中身

### ① IR で「ポインタの説明」を付ける（AddrMapAnnotate）

機械語になるとポインタと整数の区別や、どの変数から来た値かが分からなくなる。
そこで ISel の直前、まだ IR の情報が残っているうちに、各ポインタ値を

```
ルート（どのオブジェクトか） ＋ 定数オフセット ＋ 可変項（scale × ?）
```

の形に推論して、命令にメタデータとして付けておく。上の例 `f` の IR は次のようになる。

```llvm
%arrayidx = getelementptr %struct.item, ptr %p, i64 %i, !addrmap !41
%pos      = getelementptr i8, ptr %arrayidx, i64 8,     !addrmap !44

!41 = !{!"ptr", !"pointee", !5, i64 0, !42, !"p", !34, i1 false}
!44 = !{!"ptr", !"pointee", !5, i64 8, !42, !"p", !34, i1 false}
!42 = !{i64 12}
;        種類   ルートの種類  型   定数   可変項  式    ポインタ型
```

`%pos` は「`p` の指す `struct box` の先頭から、8 バイト ＋ 12×? の位置」と読める。

ルートの推論は値をさかのぼって行う（再帰）。

| 値 | 推論 |
| --- | --- |
| グローバル変数 | ルート `global`、オフセット 0 |
| `alloca`（`#dbg_declare` あり） | ルート `var`（ローカル変数） |
| ポインタ型変数の値（`#dbg_value`）、ポインタの `load` | ルート `pointee`（`p`、`p->next` が指す先） |
| GEP | ベースの説明に、定数オフセットと可変項を足す |
| PHI / select | 入力のルートが同じなら引き継ぐ。ループで動くならオフセットは不明 |
| 整数の `add` / `mul` / `shl` など | `!addrmap !{!"int"}`（整数計算の印） |

### ② MachineInstr まで運ぶ

```
 IR 命令 (!addrmap)
   │  SelectionDAGBuilder: 対応する SDNode の NodeExtraInfo に付ける
   │  （ノードが置き換わっても copyExtraInfo で引き継がれる）
   ▼
 SDNode ──EmitSchedule──▶ MachineInstr（ExtraInfo::AddrMap）
                             │ 2 アドレス変換、X86FixupLEAs、spill の folding など
                             │ 命令が作り直されたときも新しい命令へ写す
                             ▼
                          AsmPrinter に届く
```

`!pcsections` と似た仕組みだが、ISel 中に挿入された途中のノードには配らない（関係ない計算に付けないため）。

### ③ AsmPrinter でレコードを作る（X86AddrMap）

命令ごとに次を行う。

1. **対象か判定し、`kind` を決める**

   | 命令 | kind |
   | --- | --- |
   | `lea sym(%rip)`、`mov $sym`、TLS 疑似命令 | `addr`（アドレスの生成） |
   | `mov sym@GOTPCREL(%rip)` | `addr_got` |
   | `lea`/`add`/`sub` で既存ポインタからずらす | `derive` |
   | `and`/`or` でポインタをいじる | `mask` |
   | 整数計算に使われた `lea`（32 ビット、IR 注釈が `int`、`base == index` など） | `not_pointer` |

2. **ベース（何のアドレスか）を特定する**
   - シンボルがあれば、そのグローバル変数（TLS、文字列リテラル、関数内 static も区別）。
   - `%rsp`/`%rbp` 基準なら、デバッグ情報のスタック変数の配置と照合してローカル変数を探す。
   - `derive` なら、IR 注釈のルート → 同じ基本ブロック内で前に記録したポインタ → `DBG_VALUE` の順に試す。

3. **パスを解決する**（`AddrMapTypes` の `resolvePath`）
   ベースの DI 型（デバッグ情報の型）に沿って、オフセットをフィールドと添字に分解する。

   ```
   struct box (100 B)         オフセット 8 + 12×?
   ├ n          @0  int
   └ items      @4  struct item[8]  （要素 12 B）   8-4 = 4 → 12×? で添字 [?]
       ├ id     @0  int
       ├ pos    @4  int                            残り 4 → pos
       └ val    @8  int
                                    ⇒  p->items[?].pos
   ```

   共用体で候補が複数ある、オフセット 0 で「構造体自体か先頭メンバか」決まらない、
   といった場合は `alternatives` に候補を並べる。

4. **中間値を結び付ける**
   最終値（`addq`）のパスが解けたら、その計算元をたどって、パスが解けなかった途中の命令
   （`leaq (%rdi,%rax,4)`）に `intermediate_of` を付ける。

## 4. セクションのレイアウト

```
.llvm_addrmap（SHT_PROGBITS、非ロード、8 バイト整列）
┌────────────────────────────────────────────┐
│ ヘッダ 48 B                                │  "AMAP", version, header_size,
│                                            │  unit_size, entry_count,
│                                            │  json_offset, json_size, 予約, tu_id(16 B)
├────────────────────────────────────────────┤
│ 索引 16 B × N                              │  pc (u64, R_X86_64_64 → .LaddrmapN)
│                                            │  record_id (u32), flags (u32)
├────────────────────────────────────────────┤
│ JSON Lines                                 │  {"type":"header",...}
│                                            │  {"type":"record","id":0,...}
│                                            │  {"type":"record","id":1,...}  ...
├────────────────────────────────────────────┤
│ 0 埋め（8 B 境界まで）                     │
└────────────────────────────────────────────┘
```

- 索引の `pc` は再配置なので、リンク後は実際の命令アドレスになる。
  利用側は「PC → レコード ID → JSON の行」と引ける。
- 中身は `llvm-objcopy --dump-section .llvm_addrmap=out.bin foo.o` で取り出せ、JSON 部分はそのまま読める。

## 5. 主なファイル

| ファイル | 役割 |
| --- | --- |
| `llvm/lib/CodeGen/AddrMapAnnotate.cpp` | ① IR 注釈パス |
| `llvm/lib/CodeGen/AddrMapTypes.cpp` / `llvm/include/llvm/CodeGen/AddrMapTypes.h` | DI 型の扱い、パス解決、注釈のエンコード |
| `SelectionDAGBuilder.cpp`、`ScheduleDAGSDNodes.cpp`、`SelectionDAGISel.cpp`、`MachineInstr.h` ほか | ② MI への伝搬 |
| `llvm/lib/Target/X86/X86AddrMap.cpp` | ③ 対象命令の判定、ベースの特定、レコード、セクション出力 |
| `X86AsmPrinter.cpp`、`X86MCInstLower.cpp` | ③ を呼び出すフック |

## 6. 制限（抜粋）

- 対象は C、x86-64 ELF だけ。`-g` がないと型が分からず、パスは空になる。
- `source_expr`（フロントエンドでの注釈）は未実装で、常に null。
- `derived_from` の追跡は基本ブロック内だけ。

詳しくは [design.md](design.md) の 8 章を参照。
