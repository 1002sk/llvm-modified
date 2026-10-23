# Clang/LLVM アドレス対応表（addrmap）拡張 要求仕様書

Oct 21, 2026 · @So

## 1. 目的と背景

Clang/LLVM を改造し、生成された機械語命令のうち**ポインタを生成・導出する命令**ごとに、そのポインタが**どのオブジェクトの、どの部分（構造体フィールド・配列要素など）を指すか**を記録した対応表（以下 addrmap）を ELF の独自セクションとして出力する。

利用目的は fat pointer の生成である。後段の解析・変換で「この命令が作ったポインタは、どのオブジェクトのどの範囲を指すか」を知り、ポインタの伝播を追跡できるようにする。特に、構造体の内側を指すポインタ（`&s.inner`、`&p->field`）の取得時に、外側のオブジェクトと内側の部分オブジェクトの両方の範囲を得られることが最重要である。

- 対象は x86-64 / C 言語 / Linux（ELF）。
- 対象命令はアドレス生成とアドレス演算のみ。メモリの load/store は対象外。
- 最適化（-O1〜-O3, -Os, -Oz）が有効でも、最終的な機械語命令が存在する限り、その命令に対応する記録を出す。
- 出力は pyelftools で読める非アロケート ELF セクション。中身は Python で容易にパースできる JSON Lines を含む。
- 構造体フィールドの場合は、フィールド名とネストしたアクセスパス（例: `s.inner.arr[3].x`）、およびその部分オブジェクトの範囲（オフセットとサイズ）を記録する。

本書は実装を担当する Claude（エージェント）への指示書を兼ねる。要求レベルは MUST（必須）/ SHOULD（強く推奨）/ MAY（任意）で示す。

## 2. 前提・スコープ・用語

### 2.1 前提環境

| 項目 | 内容 |
| --- | --- |
| ターゲット | x86\_64-unknown-linux-gnu のみ（i386 は対象外） |
| 言語 | C（C11/C17 + GNU 拡張）。C++/Objective-C は対象外 |
| オブジェクト形式 | ELF |
| ベース LLVM | 安定版 23.1 系の最新パッチリリース（2026 年 10 月時点で `llvmorg-23.1.3`、[参考](https://pkgs.alpinelinux.org/package/edge/main/x86/llvm23-dev)）に固定し、そのタグからのブランチで作業する。依頼者の手元にある 24.0.0git（main）は開発版のため採用しない |
| ビルド | `LLVM_ENABLE_PROJECTS=clang`、`LLVM_TARGETS_TO_BUILD=X86`、Release + Assertions、lld でリンク |
| デバッグ情報 | `-g` 併用を前提とする（型・変数情報の取得源） |
| 利用側 | 依頼者は Python + pyelftools で ELF を読む |

### 2.2 スコープ外

- メモリの load/store 命令（アドレスを使うだけの命令）。最適化でアドレス計算が load/store のアドレッシングモードに吸収され、アドレス生成命令が消えた場合は記録しない。
- LTO（-flto）、ThinLTO。将来要件に加える可能性があるため、設計上は LTO のコード生成段でも同じパスが動く構成を妨げないこと。
- インラインアセンブリ内部のアドレス計算。
- リンク後のアドレス解決はコンパイラではなく付属 Python ツールで行う。

### 2.3 用語

| 用語 | 意味 |
| --- | --- |
| 対象命令 | ポインタ値を生成（オブジェクトのアドレスを取る）または導出（既存ポインタから別のポインタを計算）する機械語命令 |
| ベースオブジェクト | ポインタが指す領域の大元。ローカル変数、引数、グローバル、静的ローカル、TLS、文字列リテラル、複合リテラル、ポインタの指す先（pointee）など |
| 部分オブジェクト | ベースオブジェクト内の、パスで選ばれた領域（フィールド、配列、配列要素）。fat pointer の狭い範囲に相当 |
| アクセスパス | ベースから目的位置までの「フィールド選択」と「配列添字」の列 |
| レコード | 1 つの対象命令と 1 つの解釈（ベース + パス）の組。1 命令に複数レコードがありうる |

## 3. 機能要求

### 3.1 対象命令の分類（kind）

| kind | 例（AT&T 記法） | 意味 | 要求 |
| --- | --- | --- | --- |
| `addr` | `lea 8(%rsp),%rdi` / `lea g+12(%rip),%rax` / `mov $g,%eax` / `movabs $g,%rax` | 名前付きオブジェクト（またはその一部）のアドレスを生成 | MUST |
| `addr_got` | `mov g@GOTPCREL(%rip),%rax` | GOT 経由でオブジェクトのアドレスを取得（形式上は load だが、アドレス生成として扱う） | MUST |
| `derive` | `lea 16(%rdi),%rax` / `add $16,%rdi` / `lea (%rdi,%rsi,8),%rax` / `sub $4,%rax` / ループ内の `add $4,%rax` | 既存ポインタから派生ポインタを計算（`&p->f`、`p + i`、`&p[i].x`、誘導変数化されたポインタの更新） | MUST |
| `mask` | `and $-16,%rax` / `or $1,%rax` | ポインタのビット演算（アラインメント、タグ付け） | SHOULD |
| `not_pointer` | `lea (%rax,%rax,2),%rcx` | 整数演算に使われた LEA。網羅性検査のために明示する | MUST |

- メモリの load/store（`mov 8(%rdi),%eax` 等）は対象外。アドレス計算がアドレッシングモードに畳み込まれ、アドレス生成命令が残らない場合は記録しない。
- ポインタがメモリへ格納されたり関数に渡されたりする（エスケープする）場合は、必ずアドレス生成命令が残るため、fat pointer 化に必要な点は捕捉できる。
- 記録対象外: スタックプロテクタ（`%fs:0x28`）、プロローグ/エピローグのフレーム操作、暗黙のスタック操作（push/pop/call/ret）。

### 3.2 網羅性要求（最重要）

- **R-COV-1 (MUST)**: 最終出力に残った全ての LEA 命令、およびシンボルのアドレスを即値・RIP 相対・GOT で取り込む全ての命令に、1 件以上のレコードがあること（`not_pointer` を含む）。逆アセンブルで機械的に検査できる。
- **R-COV-2 (MUST)**: IR 上のポインタ演算（GEP、`ptradd`、`ptrtoint`→整数演算→`inttoptr`）から生成され、最終出力に残った全ての `add`/`sub`/`inc`/`dec`/`and`/`or` 等に `derive` または `mask` のレコードがあること。ポインタかどうかは逆アセンブルでは判別できないため、コンパイラ内部の統計（注釈の消失件数 = 0）で検査する。
- **R-COV-3 (MUST)**: ベースを特定できない場合も命令を落とさず、`base.kind = "unknown"` と理由 `reason` を記録すること。
- **R-COV-4 (MUST)**: 最適化で機械語命令自体が消えた場合（変数のレジスタ化、アドレッシングモードへの畳み込み等）は、レコードを出さないことが正しい挙動である。
- **R-COV-5 (MUST)**: インライン展開・ループ展開・複製で 1 つのソース式が複数の命令になった場合、各命令にレコードを出し、`inlined_at` 連鎖を付けること。
- **R-COV-6 (SHOULD)**: 命令統合により 1 命令が複数のソース式に対応する場合、全ての解釈を別レコード（同一 PC）で出すこと。

### 3.3 記録内容

各レコードは以下を持つ（詳細は 4 章のスキーマ）。

1. 命令の位置: 関数名、PC（セクション内の再配置で解決）、ニーモニック、アドレス式（base/index/scale/disp/segment/symbol）と、結果を書き込むレジスタ。
2. ソース位置: ファイル・行・列、インライン元の連鎖。
3. ベースオブジェクト: 種別、名前、C の型、サイズ、宣言位置。
4. アクセスパス: フィールド選択と配列添字の列。各段にオフセットとサイズを持たせる。
5. 生成されたポインタの位置: ベースからのバイトオフセット（定数部 + 可変部）。
6. 範囲情報（3.4）: オブジェクト全体の範囲と、各部分オブジェクトの範囲。
7. 導出元（`derive` のみ）: 元ポインタの説明と、同じ関数内で元ポインタを作ったレコード ID。
8. 信頼度と情報源: `exact` / `derived` / `heuristic`、および `provenance`。

### 3.4 範囲情報（fat pointer 用）

範囲は全て「ベースの先頭」からのバイトオフセットで表す。ベースが `pointee` の場合は「元ポインタの値」を原点とする。

- **R-BND-1 (MUST)**: オブジェクト全体の範囲 `object: {size}` を記録する。静的に決まらない場合（VLA、`alloca()`、pointee の全体長）は `size: null` とし、理由を `size_unknown` に入れる。pointee の場合も、その型のサイズは `type_size` に入れる。
- **R-BND-2 (MUST)**: アクセスパスの各段（フィールド、配列、配列要素）に `offset` と `size` を付ける。これにより外側から内側までの入れ子の範囲が全て得られる。
- **R-BND-3 (MUST)**: 添字が可変の配列要素は、配列全体の範囲（`offset`、`length`、`stride`）を配列の段に、要素の範囲を `offset: null` + `size` で要素の段に記録する。
- **R-BND-4 (MUST)**: フレキシブル配列メンバは `length: null`、`flexible: true` とする。
- **R-BND-5 (SHOULD)**: 生成されたポインタが部分オブジェクトの範囲外（配列末尾の次 `&a[N]`、負オフセット）を指す場合は `out_of_bounds: true` を付ける。

### 3.5 アクセスパスの規則

- **R-PATH-1 (MUST)**: 定数オフセットは DI 型情報を辿ってフィールド名の列に変換する。ネストした構造体・構造体内配列・配列内構造体・無名構造体/共用体メンバを任意深さで扱う。
- **R-PATH-2 (MUST)**: オフセットがフィールド境界の途中を指す場合は、最も内側の包含フィールド + 残りバイト数 `byte_offset_in_field` を記録する。
- **R-PATH-3 (MUST)**: 添字が定数なら `index` に値、可変なら `index: null` と index レジスタ・scale を記録する。
- **R-PATH-4 (MUST)**: 共用体は全メンバを候補として `alternatives` に列挙する。ソース上で使われたメンバが分かる場合はそれを `path` とし `exact` にする。
- **R-PATH-5 (MUST)**: オフセット 0 は「オブジェクト全体」と「先頭フィールド」の両方に一致する。ソース上の意図（3.7）が分かればそれを `path` とし、分からなければ `path` を空にして先頭フィールドのパスを `alternatives` に入れる。
- **R-PATH-6 (MUST)**: 配列から先頭要素へのポインタへの暗黙変換（`int *q = s.arr;`）は、パス `.arr` に `decay: true` を付ける。範囲は配列全体とする。

### 3.6 ポインタ経由の導出（最重要）

- **R-PTR-1 (MUST)**: `&p->f`、`&p->a.b`、`&p->arr[i]`、`p + i`、`&p[i].x` を `derive` として記録する。ベースは `base.kind = "pointee"`、型は p の pointee 型（p の DI 型、またはフロントエンド注釈から）。パスと範囲は元ポインタの値を原点とする。
- **R-PTR-2 (MUST)**: 元ポインタの説明 `source` を記録する。p が変数なら変数名、フィールドから読んだポインタ（`s.inner->x`、`p->next->val`）ならそのアクセス式（例: `p->next`）とその型。
- **R-PTR-3 (SHOULD)**: 元ポインタが同じ関数内の別の対象命令で作られた場合、そのレコード ID を `derived_from` に入れる（例: `q = &s.inner;` → `r = &q->x;`）。ポインタ伝播の追跡に使う。関数をまたぐ連鎖（引数・戻り値経由）は対象外とする。
- **R-PTR-4 (MUST)**: 構造体を指すポインタから内側のフィールドへのポインタを得るケースを最優先とし、受け入れ基準を他より厳しくする（10 章）。
- **R-PTR-5 (SHOULD)**: 整数を経由したポインタ演算（`(T*)((uintptr_t)p + 8)`）は `confidence: heuristic` で記録する。`offsetof` / `container_of` 形式の負方向の導出も同様に扱い、該当すれば外側の構造体型とフィールドを `alternatives` に入れる。
- **R-PTR-6 (MUST)**: pointee 型は、その式の時点での静的型（キャスト後の型）とする。`((struct A *)vp)->x` は `struct A` として解釈する。`void *` / `char *` に対する加減算はフィールドに分解せず、バイトオフセットのみを記録する（パスは空、`result_type` は元の型）。

### 3.7 ソース上の意図（フロントエンド注釈）

- **R-SRC-1 (MUST)**: Clang フロントエンドで `&expr`、メンバアクセス（`.` / `->`）、配列添字、配列の暗黙変換、ポインタ加減算の各式について、ソース上のアクセス式（例: `&p->items[i].pos`）と各段の C の型（`DIType` 参照）を IR に注釈し、`source_expr` として出力する。pointee 型の主な取得源とする。
- **R-SRC-2 (MUST)**: フロントエンド注釈が最適化で失われても、機械語側からの復元（ベース + オフセット + DI 型）で R-COV-1〜3 を満たすこと。

## 4. 出力形式

出力は ELF の独自セクション `.llvm_addrmap` 1 本に全情報を入れる。命令アドレスはバイナリの索引部（再配置付き）、レコード本体は同じセクション内の JSON Lines に置き、pyelftools だけで読めるようにする。デバッグ用に JSONL を別ファイルにも書き出せるようにする（MAY）。

### 4.1 セクション `.llvm_addrmap`（MUST）

- タイプ `SHT_PROGBITS`、フラグは非アロケート（`SHF_ALLOC` なし）、アラインメント 8。`.debug_*` と同様にリンカが TU ごとの中身を連結し、実行時メモリを消費しない。
- TU ごとに 1 つの「ユニット」を出す。ユニットは ヘッダ → 索引エントリ列 → JSONL 本体 → 0 埋め（8 バイト境界まで）で構成する。`unit_size` を辿ればリンク後の連結セクションを先頭から順に読める。

| 部分 | フィールド | サイズ |
| --- | --- | --- |
| ヘッダ | magic `"AMAP"` | 4 B |
| ヘッダ | version（=1） | 2 B |
| ヘッダ | header\_size（=48） | 2 B |
| ヘッダ | unit\_size（ヘッダ・0 埋めを含む全長、8 の倍数） | 4 B |
| ヘッダ | entry\_count | 4 B |
| ヘッダ | json\_offset（ユニット先頭から） | 4 B |
| ヘッダ | json\_size | 4 B |
| ヘッダ | reserved（0） | 8 B |
| ヘッダ | tu\_id（内容ハッシュ 128bit） | 16 B |
| 索引エントリ | pc（`R_X86_64_64` で命令先頭を参照） | 8 B |
| 索引エントリ | record\_id | 4 B |
| 索引エントリ | flags（bit0: 同一 PC に後続エントリあり） | 4 B |

- 全てリトルエンディアン。同一命令に複数レコードがある場合は、同じ pc のエントリを複数並べる。
- pc は命令直前に AsmPrinter が発行する一時ラベルへの絶対再配置とする。`.o` では pyelftools の `RelocationHandler` で再配置を適用して `.text` 内オフセットを得る。リンク済み ELF（PIE 含む）では非アロケートセクションの再配置はリンク時に解決され、リンク時の仮想アドレスが入る。
- `--gc-sections` で関数が除去された場合、そのエントリの pc は 0 またはリンカのトゥームストーン値になる。リンク済み ELF ではツールはこれらを読み飛ばす（MUST）。.o では pc 0 は .text 先頭を指す正当な値なので、読み飛ばしてはならない（4.4 の検証で確認した落とし穴）。`SHF_LINK_ORDER` で関数ごとに除去される構成は MAY。
- 出力は決定的であること（同一入力・同一オプションで同一バイト列）。`tu_id` はソースパス・時刻を含まない内容ハッシュとする。

### 4.2 JSONL 本体（MUST）

- UTF-8、1 行 1 JSON オブジェクト。1 行目はユニットのヘッダ、2 行目以降がレコード。`id` は TU 内で 0 からの連番で、索引の record\_id と対応する。
- JSON Schema（`addrmap.schema.json`）を成果物に含め、`version` を持たせる。

ヘッダ行の例:

```json
{"type":"header","version":1,"tu_id":"9f3c…","source":"src/world.c","target":"x86_64-unknown-linux-gnu","opt_level":"O2","producer":"clang 23.1.3 addrmap-patch <git-hash>"}
```

レコード行の例。`q = &p->items[i].pos;`（`p` は `struct World *`）が `shl $5,%rcx` + `lea 12(%rdi,%rcx),%rax` になった場合（整形して表示）:

```json
{
  "type": "record",
  "id": 17,
  "function": "update",
  "kind": "derive",
  "insn": {"mnemonic": "leaq", "dest_reg": "rax",
           "addr": {"segment": null, "base_reg": "rdi", "index_reg": "rcx", "scale": 1,
                    "disp": 12, "symbol": null, "rip_relative": false}},
  "source": {"file": "src/world.c", "line": 42, "column": 9,
             "inlined_at": [{"function": "main", "file": "src/world.c", "line": 80, "column": 5}]},
  "base": {"kind": "pointee", "name": null, "type": "struct World", "type_size": 520,
           "object": {"size": null, "size_unknown": "pointee"}},
  "src_ptr": {"expr": "p", "var": "p", "type": "struct World *", "derived_from": []},
  "path": [
    {"op": "field", "name": "items", "record": "struct World", "type": "struct Item[16]",
     "offset_in_parent": 8, "offset": 8, "size": 512, "length": 16, "stride": 32},
    {"op": "index", "index": null, "index_reg": "rcx", "type": "struct Item",
     "offset_in_parent": null, "offset": null, "size": 32},
    {"op": "field", "name": "pos", "record": "struct Item", "type": "struct Vec2",
     "offset_in_parent": 4, "offset": null, "size": 8}
  ],
  "path_str": "p->items[?].pos",
  "ptr_offset": {"const": 12, "var": [{"reg": "rcx", "scale": 1, "stride": 32}]},
  "byte_offset_in_field": 0,
  "result_type": "struct Vec2 *",
  "source_expr": "&p->items[i].pos",
  "alternatives": [{"path_str": "p->items[?].pos.x", "reason": "offset 0 of struct Vec2"}],
  "out_of_bounds": false,
  "confidence": "exact",
  "provenance": ["frontend", "debuginfo-type"],
  "reason": null
}
```

- `base.kind` の値: `local` / `param` / `global` / `static_local` / `tls` / `string_literal` / `compound_literal` / `pointee` / `unknown`。
- パスの各段の `offset` はベース先頭（pointee なら元ポインタの値）からの絶対オフセットで、可変添字より内側では `null` になる。`offset_in_parent` は常に親の段からの相対値。
- `kind = not_pointer` のレコードは `insn` と `source` のみ持つ。

### 4.3 アセンブリコメント（SHOULD）

`-S` 出力時、オプション指定で対象命令の行末にコメントを付ける。

```
    leaq    12(%rdi,%rcx), %rax     # addrmap#17 derive p->items[?].pos (struct Vec2, 8B)
```

### 4.4 実例（手組みで検証済み）

改造後の Clang が出すはずのセクションを、clang 18 の `-S` 出力に手で付けて組み立て、lld でリンクし、pyelftools だけで読めることを確認した。実装時の期待出力として使う。

ソース（`ex.c`）:

```c
struct Vec2 { int x, y; };
struct Item { int id; struct Vec2 pos; };
struct Item g_item;

int *get_y(struct Item *p) { return &p->pos.y; }
struct Vec2 *get_gpos(void) { return &g_item.pos; }
int main(void) { return *get_y(&g_item) + get_gpos()->x; }
```

`-O2 -fno-pie` のアセンブリに、対象命令の直前ラベルとセクションを付けたもの（抜粋）:

```
get_y:
.Laddrmap0:
        leaq    8(%rdi), %rax               # derive  p->pos.y
        retq
get_gpos:
.Laddrmap1:
        movl    $g_item+4, %eax             # addr    g_item.pos
        retq
main:                                       # get_y/get_gpos がインライン展開され、
        movl    g_item+4(%rip), %eax        # アドレス計算は load に吸収された
        addl    g_item+8(%rip), %eax        # → 記録なし（R-COV-4）
        retq

        .section .llvm_addrmap,"",@progbits
        .p2align 3
.Lam_unit:
        .ascii  "AMAP"                      # magic
        .short  1                           # version
        .short  48                          # header_size
        .long   .Lam_end-.Lam_unit          # unit_size
        .long   2                           # entry_count
        .long   .Lam_json-.Lam_unit         # json_offset
        .long   .Lam_json_end-.Lam_json     # json_size
        .quad   0                           # reserved
        .byte   80, 109, 44, ...            # tu_id (16 B)
        .quad   .Laddrmap0                  # [0] pc
        .long   0, 0                        #     record_id, flags
        .quad   .Laddrmap1                  # [1] pc
        .long   1, 0
.Lam_json:
        .ascii  "{\"type\":\"header\",...}\n{\"type\":\"record\",\"id\":0,...}\n..."
.Lam_json_end:
        .p2align 3, 0x0
.Lam_end:
```

`.o` のセクションと再配置（`readelf -SW` / `-rW`）:

```
[ 7] .llvm_addrmap      PROGBITS  0000000000000000 000098 0007c8 00     0   0  8
[ 8] .rela.llvm_addrmap RELA      0000000000000000 000950 000030 18  I 10   7  8

Relocation section '.rela.llvm_addrmap' contains 2 entries:
    Offset   Type          Symbol's Name + Addend
000000000030 R_X86_64_64   .text + 0     # entry[0].pc → get_y の leaq
000000000040 R_X86_64_64   .text + 10    # entry[1].pc → get_gpos の movl
```

フラグ欄に `A` がない（非アロケート）ため、リンク後も実行時メモリには載らない。セクション先頭のバイト列:

```
0000: 41 4d 41 50 01 00 30 00 c8 07 00 00 02 00 00 00  AMAP..0.........
0010: 50 00 00 00 75 07 00 00 00 00 00 00 00 00 00 00  P...u...........
0020: 50 6d 2c e0 1d 97 9f d2 e7 2d d9 5d 5b 2d b3 63  Pm,......-.][-.c
0030: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................   <- entry[0] (pc は再配置で埋まる)
0040: 00 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00  ................   <- entry[1]
0050: 7b 22 74 79 70 65 22 3a 22 68 65 61 64 65 72 22  {"type":"header"   <- JSONL 本体
```

pyelftools のリーダで読んだ結果（`.o` は再配置を適用、実行ファイルはそのまま）:

```
== ex.o
0x00000000  get_y+0x0     derive  pointee  p->pos.y    int *          ranges=[('pos', 4, 8), ('y', 8, 4)]
0x00000010  get_gpos+0x0  addr    global   g_item.pos  struct Vec2 *  ranges=[('pos', 4, 8)]
== ex (lld でリンク)
0x002015c0  get_y+0x0     derive  pointee  p->pos.y    int *          ranges=[('pos', 4, 8), ('y', 8, 4)]
0x002015d0  get_gpos+0x0  addr    global   g_item.pos  struct Vec2 *  ranges=[('pos', 4, 8)]
```

`-ffunction-sections -Wl,--gc-sections` で両関数が除去された場合、lld は非アロケートセクションからの参照で関数を残さず、該当エントリの pc を 0 にした。リーダはそれらを読み飛ばし、出力は空になった。リーダの実装は `parse_unit` の参考実装として 9 章のツールに取り込む。

## 5. コマンドラインインタフェース

ドライバ（`clang`）と cc1 の両方で受け付け、ドライバから cc1、cc1 からバックエンド（`TargetOptions` 等）へ伝搬させる。

| オプション | 意味 | 要求 |
| --- | --- | --- |
| `-faddrmap` / `-fno-addrmap` | 機能の有効/無効。既定は無効 | MUST |
| `-faddrmap-kinds=<list>` | 出力する kind を限定（例: `addr,addr_got,derive`）。既定は全て | SHOULD |
| `-faddrmap-json=<path>` | セクションと同じ JSONL を別ファイルにも書き出す（デバッグ用） | MAY |
| `-faddrmap-asm-comments` | `-S` 出力にコメントを付ける | SHOULD |
| `-mllvm -addrmap-stats` | 対象命令数、kind 別件数、`unknown` 件数、注釈の消失件数を stderr に出す | MUST |

- `-faddrmap` 指定時に `-g` がない場合は警告を出し、型情報なし（パスは空、`confidence: heuristic`）で続行する。
- `-S`、`-save-temps` でも破綻しないこと。`-S` の出力をアセンブルしても同じセクションが得られること（セクションはディレクティブで出す）。`-emit-llvm` ではセクションを出さない。

## 6. 非機能要求

- **R-NF-1 コード不変性 (MUST)**: `-faddrmap` の有無で `.text` 等のアロケートセクションの内容がバイト単位で一致すること（`-g` 有無で生成コードが変わらないのと同じ原則）。注釈メタデータが最適化判断に影響してはならない。
- **R-NF-2 堅牢性 (MUST)**: addrmap 処理内部で解釈できないケースに遭遇してもコンパイルを失敗させない。該当レコードを `unknown` で出すか、警告を 1 回出して続行する。Assertions 有効ビルドでアサート失敗を起こさない。
- **R-NF-3 決定性 (MUST)**: 同一入力・同一オプションで出力（サイドカー、セクション）が同一。ハッシュマップの反復順に依存しない。
- **R-NF-4 性能 (SHOULD)**: `-O2 -g` でのコンパイル時間増加は 10% 以内（測定対象は 10 章の実コード評価）。
- **R-NF-5 局所性 (SHOULD)**: 変更は新規ファイルに集約し、既存ファイルへの差分は呼び出しフックとオプション伝搬に限定する。将来の LLVM バージョンへの追従を容易にするため。
- **R-NF-6 無効時のゼロコスト (MUST)**: `-faddrmap` 未指定時は既存挙動・既存テスト結果を一切変えない。

## 7. 推奨アーキテクチャ（実装指針）

中核は 2 つの情報源の突き合わせである。(a) IR 上でポインタを作る命令（GEP 等）に付けた注釈を機械語命令まで運ぶ。(b) 注釈がない命令は、機械語命令のオペランド（フレームインデックス、グローバルシンボル）からベースを復元し、DI 型でパスに変換する。以下は推奨であり、M0 の調査でより良い方法が見つかれば設計メモに理由を書いて変更してよい。

&#91;embedded content: addrmap 処理の流れ · 8 段階、うち 5 段階が新規\]

命令選択以降で MI が作り直されても、AsmPrinter が注釈とオペランドを突き合わせて最終レコードを確定する。

### 7.1 処理段階

1. **フロントエンド注釈（Clang CodeGen）**: `EmitLValueForField`、配列添字、配列→ポインタ変換、ポインタ加減算、`UO_AddrOf` の各生成箇所で、作った GEP に `!addrmap.src` を付ける。中身はソース式の文字列、各段の `DIType` 参照、元ポインタの式と型。pointee 型の主な取得源になる。
2. **後段 IR 解析パス**: IR 最適化終了後、命令選択の直前（`CodeGenPrepare` の後）に新規パスを挿入する。
   - ポインタを作る命令（GEP、`ptradd` 形の GEP、`ptrtoint`→整数演算→`inttoptr`、`ptrtoint` への `and`/`or`）を、ベース + 定数オフセット + 可変項に分解する。
   - ベース: `AllocaInst` → `#dbg_declare` の `DILocalVariable`。`GlobalVariable` → `DIGlobalVariableExpression`。それ以外 → pointee。型は 1. の注釈、なければ元ポインタ変数の `#dbg_value` / 宣言の DI 型。
   - 元ポインタを定義する命令が注釈付きなら、その関係を `derived_from` として保持する。
   - 整数演算（`add`/`mul`/`shl` 等で LEA になりうるもの）には `!addrmap.int` を付ける。注釈のない LEA を「消失」と「整数演算」に区別するためである。
   - 結果を `!addrmap` メタデータとして命令に付ける。
3. **命令選択での伝搬**: `!addrmap` / `!addrmap.int` を SelectionDAG と FastISel（x86 の -O0 は FastISel が主）の両方で、その IR 命令の結果を定義する MachineInstr に伝搬させる。`!pcsections` の伝搬経路（`SelectionDAG::addPCSections`、`MachineInstr::setPCSections`）を手本に、MachineInstr の ExtraInfo に専用フィールドを追加する。`!pcsections` 自体は流用しない。1 つの GEP が複数の MI（`shl` + `lea` 等）になる場合、最終的なポインタ値を定義する MI に付ける。
4. **フレームインデックス確定前の MI パス**: `PrologEpilogInserter` の直前に新規 MachineFunctionPass を入れる。FrameIndex を持つ LEA（`&x` 等、IR 上に命令がないアドレス生成）に「FI → 変数」の対応を付与する。`MachineFrameInfo::getObjectAllocation(FI)` と変数デバッグ情報を使う。PEI 後は FI が `rsp/rbp + 定数` に置き換わり情報が失われるため、この位置が必須。
5. **AsmPrinter での確定・出力**: 全 MI パスの後、`X86AsmPrinter` で命令出力の直前に、対象命令（全 LEA、シンボルを取り込む MOV、GOTPCREL の MOV、注釈付きの全 MI）を判定する。注釈、FI 情報、`MO_GlobalAddress` オペランドとオフセットを統合してレコードを確定し、命令直前に一時ラベルを発行する。関数ごとにレコードを溜め、`doFinalization` でユニットを組み立てて `.llvm_addrmap` に出力する。
6. **リンク後の結合（Python）**: 9 章のツールが `.llvm_addrmap` を読み、PC とレコードを結合して最終表を出す。

### 7.2 実装上の要点

- **不透明ポインタ**: IR のポインタは型を持たず、定数オフセットの GEP は `i8` ベース（`ptradd` 形）に正規化されやすい。フィールド解決に IR の型を使ってはならず、DI 型（またはフロントエンド注釈の型）を使う。
- **x86 のアドレス式**: LEA のオペランドは 5 個（Base, Scale, Index, Disp, Segment）。`X86II::getMemoryOperandNo` と `X86II::getOperandBias` で位置を求める。Disp は即値・グローバル・FI・ConstantPool 等の種類がある。
- **MI を作り直す後段処理**: `X86FixupLEAs`（LEA ⇄ ADD/INC の変換）、`X86OptimizeLEAs`、2 アドレス変換（`convertToThreeAddress` による ADD → LEA）、レジスタ割り当ての再実体化（FI の LEA の複製）、MachineCSE、末尾複製・分岐統合などで MI が作り直される。新しい MI に注釈が引き継がれるよう各経路に処理を入れ、漏れは `-addrmap-stats` で計数する。
- **DI 型の走査**: `DIDerivedType`（typedef、const、volatile）を剥がし、`DICompositeType` のメンバ（ビット単位オフセット）、配列の `DISubrange`、無名構造体/共用体、フレキシブル配列メンバ、VLA を扱う。
- **LTO への備え**: 解析・伝搬・出力は全て LLVM 側のパスとし、Clang 固有の処理はフロントエンド注釈だけにする。注釈はビットコードに残るメタデータとすることで、将来 LTO のコード生成段でも同じパスを動かせるようにする。
- **ファイル配置**: 新規ファイルは `llvm/lib/CodeGen/AddrMap*.cpp`、`llvm/lib/Target/X86/X86AddrMap*.cpp`、`clang/lib/CodeGen/CGAddrMap*.cpp` を目安とする。

## 8. 既知の難所と期待される扱い

実装者はこれらを個別のテストケースとして用意し、期待出力を満たすこと。

| ケース | 起きること | 期待する記録 |
| --- | --- | --- |
| 構造体ポインタから内側へ（最重要） | `&p->inner` が `lea 16(%rdi),%rax` または `add $16,%rdi` になる | `derive`、pointee、パス `->inner`、内側の範囲 |
| 内側の内側 | `&p->a.b[2].c` が 1 命令に畳み込まれる | `derive`、全段のパスと各段の範囲 |
| 読んだポインタからの導出 | `&p->next->val` | `src_ptr.expr = p->next`、pointee は `next` の指す型 |
| 導出の連鎖 | `q = &s.inner; r = &q->x;` が同じ関数内で 2 命令 | `r` のレコードの `derived_from` に `q` のレコード ID |
| 定数畳み込み | `&g.b.c` が `lea g+24(%rip)` になる | `addr`、`global g`、パス `.b.c` |
| アドレッシングモードへの吸収 | GEP が load/store に吸収され、アドレス生成命令が消える | レコードなし（R-COV-4） |
| SROA / mem2reg | 変数がレジスタに昇格し、アドレス命令が消える | レコードなし |
| スタックスロット共有（StackColoring） | 生存区間が重ならない 2 変数が同じスロットを使う | FI 付与時点の変数。不明なら両変数を `alternatives` に |
| ループ強度低減（LSR） | `&a[i]` が `lea a(%rip)` + ループ内 `add $4` になる | `lea` は `addr` で `a`（配列全体の範囲）、`add` は `derive` で `a[?]` |
| 整数演算の LEA | `x * 3` が `lea (%rax,%rax,2)` になる | `not_pointer` |
| LEA ⇄ ADD 変換 | `X86FixupLEAs` 等が命令を置き換える | 置き換え後の命令に同じ内容のレコード |
| 再実体化 | `&x` の LEA がレジスタ割り当てで複数箇所に複製される | 各複製にレコード |
| インライン展開 | 同じ式が複数の呼び出し元に複製される | 各命令にレコード + `inlined_at` |
| 分岐統合 | 2 つのソース式の命令が 1 つに統合される | 同一 PC に 2 レコード |
| 共用体 | 同じオフセットに複数メンバ | `alternatives` に全メンバ |
| オフセット 0 | 構造体全体と先頭フィールドが同じアドレス | ソースの意図があればそれ、なければ `path` 空 + `alternatives` |
| 配列の暗黙変換 | `int *q = s.arr;` | パス `.arr`、`decay: true`、範囲は配列全体 |
| 範囲外・末尾の次 | `&a[N]`、`p - 1` | `out_of_bounds: true` |
| 整数経由の演算・`container_of` | `(T*)((char*)p - offsetof(T, f))` | `derive`、`heuristic`、外側の型とフィールドを `alternatives` に |
| アラインメント・タグ | `(uintptr_t)p & ~15` | `mask`、元ポインタ |
| PIC / PIE | GOT 経由でグローバルのアドレスを取得 | `addr_got`、`global` |
| TLS | `%fs:` 相対のアドレス生成 | `addr`、`tls` |
| VLA / `alloca()` | 動的スタック領域 | 変数名 + 要素型、`object.size: null` |
| 文字列リテラル・複合リテラル | 無名オブジェクト | `string_literal` / `compound_literal` + ソース位置 + サイズ |
| フレキシブル配列メンバ | `&p->data[i]` | `length: null`、`flexible: true` |
| `-fomit-frame-pointer` 有無 | `rbp` 基準か `rsp` 基準か | どちらでも同じ変数・パス |
| 型情報の欠落（`-g` なし、不完全型） | フィールドを解決できない | `heuristic`、`path` 空、`reason` に理由 |

## 9. 付属ツール（Python）

Python 3.10 以上。必須の依存は `pyelftools` のみとし、逆アセンブルを使う `verify` だけ `capstone` を追加依存とする。ライブラリ（`import addrmap`）と CLI（`addrmap-tool`）の両方として提供する。

| サブコマンド | 入力 | 出力 | 要求 |
| --- | --- | --- | --- |
| `dump` | `.o` またはリンク済み ELF | 命令アドレス順の表（JSONL / CSV / 人間向けテキスト） | MUST |
| `verify` | 同上 | R-COV-1 の検査: `.text` を逆アセンブルし、LEA・シンボル取り込み命令でレコードのないものを列挙。終了コードで合否 | MUST |
| `stats` | 同上 | kind 別・base.kind 別・confidence 別の件数 | SHOULD |
| `lookup` | ELF + アドレス | そのアドレスの命令のレコード | SHOULD |
| `chains` | 同上 | `derived_from` を辿ったポインタ導出の連鎖（関数ごと） | SHOULD |

- `.o` 入力では pyelftools の `RelocationHandler` で `.rela.llvm_addrmap` を適用し、「セクション + オフセット」を得る。リンク済み ELF では仮想アドレスをそのまま読む。
- ユニットは `unit_size` で順に辿る。リンク済み ELF では pc が 0 またはトゥームストーン値のエントリを読み飛ばす（.o では読み飛ばさない）。
- `dump` の 1 行には、仮想アドレス、関数名 + オフセット、レコード本体を含める（`verify` 用の逆アセンブル文字列は capstone がある場合のみ）。
- ライブラリ API の例: `AddrMap.load(elf_path).records_at(addr)` → `list[Record]`（dataclass）。
- 依頼者の既存 pyelftools コードから直接使えるよう、セクションの解析部分は `parse_unit(bytes, relocs)` のような小さな関数として切り出す。

## 10. テスト・受け入れ基準

### 10.1 テストの種類

1. **lit / FileCheck テスト（LLVM ツリー内）**: `clang/test/CodeGen/addrmap-*.c`（フロントエンド注釈）、`llvm/test/CodeGen/X86/addrmap-*.ll`（後段解析・伝搬・出力）。`-S` のコメントとサイドカーの内容を FileCheck で検証する。
2. **E2E テスト（Python, pytest）**: `tests/addrmap/` に C ソースと期待値を置き、ビルドしたコンパイラで `.o` とリンク済み実行ファイルを作り、`addrmap-tool dump/verify` の結果を検証する。構造体ポインタから内側を指すケース（8 章の先頭 4 行）は題材を特に厚くする。
3. **コード不変性テスト**: 全 E2E ケースで `-faddrmap` 有無の `.text` を比較（R-NF-1）。
4. **実コード評価**: 公開 C コード（例: SQLite amalgamation、zlib、Lua）を `-O0/-O2/-O3 -g` でビルドし、クラッシュなし・`verify` 合格・`unknown` 率・コンパイル時間を計測する。

### 10.2 テストマトリクス

- 最適化: `-O0`、`-O1`、`-O2`、`-O3`、`-Os`、`-Oz`。
- コードモデル: `-fno-pic`、`-fPIC`、`-fPIE`。
- フレーム: `-fomit-frame-pointer`、`-fno-omit-frame-pointer`。
- その他: `-fstack-protector-strong`、`-march=x86-64-v3`。
- 題材: 8 章の全ケース + 構造体のネスト（3 段以上）、2 次元配列、構造体配列、配列を含む構造体、関数ポインタを含む構造体、グローバル/静的/TLS/引数/ローカル、`const`/`volatile`、typedef 経由の型。

### 10.3 受け入れ基準

| ID | 基準 | 合格条件 |
| --- | --- | --- |
| AC-1 | 網羅性（機械検査） | 全マトリクス × 全題材で `verify` が合格（LEA・シンボル取り込み命令のレコード欠落 0 件） |
| AC-2 | 網羅性（コンパイラ統計） | 全マトリクス × 全題材で `-addrmap-stats` の注釈消失件数が 0 |
| AC-3 | 正確性（構造体ポインタ → 内側） | 期待値を持つ E2E ケースで、`-O0` は 100%、`-O2` 以上は 98% 以上が期待パス・期待範囲と一致 |
| AC-4 | 正確性（その他） | `-O0` は 100%、`-O2` 以上は 95% 以上が期待パス・期待範囲と一致 |
| AC-5 | unknown 率 | 実コード評価で `unknown` が対象命令の 5% 未満 |
| AC-6 | コード不変性 | 全ケースで `.text` 等のアロケートセクションが `-faddrmap` 有無で一致 |
| AC-7 | 無効時 | `-faddrmap` なしで `check-clang`、`check-llvm-codegen-x86` が改造前と同一結果 |
| AC-8 | 決定性 | 同一ビルドを 2 回行い `.llvm_addrmap` がバイト一致 |
| AC-9 | 性能 | 実コード評価で `-O2 -g` のコンパイル時間増加が 10% 以内 |
| AC-10 | 読み取り | `.o`、静的リンク、PIE、`--gc-sections` 付きの各 ELF を pyelftools だけで読める |

## 11. マイルストーンと成果物

各マイルストーンの終わりに、完了条件の結果と次段階での変更点を `docs/addrmap/progress.md` に追記する。

| 段階 | 内容 | 完了条件 |
| --- | --- | --- |
| M0 調査・環境構築 | `llvmorg-23.1.3` をビルド。`!pcsections` の伝搬経路、FastISel、FI と変数デバッグ情報、StackColoring、LEA を作り直すパス（FixupLEAs、2 アドレス変換、再実体化）のコードを読み、設計メモを作成 | ビルド成功、`docs/addrmap/design.md` 作成 |
| M1 骨格 | オプション伝搬、AsmPrinter での対象命令判定、`.llvm_addrmap` ユニットの出力（中身は `unknown` のみでよい）、Python `dump`/`verify` | `-O0`〜`-O3` で AC-1・AC-10 合格（全て `unknown` で可） |
| M2 -O0 の完全対応 | フロントエンド注釈、グローバル・ローカル・引数の解決、DI 型によるパスと範囲、pointee の `derive` | `-O0` で AC-3・AC-4 合格 |
| M3 最適化対応 | 後段 IR 解析パス、ISel 伝搬、PEI 前の FI 付与、MI 作り直し時の引き継ぎ、`not_pointer` 判定、インライン情報 | `-O2` で AC-2・AC-3・AC-6 合格 |
| M4 応用ケース | `derived_from`、LSR、`mask`、整数経由・`container_of`、共用体・フレキシブル配列・範囲外 | 8 章の全ケースが期待どおり |
| M5 仕上げ | asm コメント、統計、性能測定、実コード評価、ドキュメント | AC-1〜AC-10 全て合格 |

### 成果物

- 固定タグからのブランチ上のパッチ（機能単位のコミット）と `git format-patch` 一式。
- ビルド手順書（`docs/addrmap/BUILD.md`）。
- 設計メモ（`design.md`）、進捗記録（`progress.md`）、利用者向け説明（`README.md`）。
- JSON Schema（`addrmap.schema.json`）。
- Python パッケージ（`tools/addrmap/`）とテスト（`tests/addrmap/`）。
- 受け入れ基準の測定結果レポート。

## 12. 作業エージェント向け指示

- **進め方**: マイルストーン順に進め、各段階で「テストを先に書く → 実装 → テスト合格 → コミット」を守る。1 コミット 1 機能。
- **最初にやること**: M0 の調査。推測で API を使わず、固定タグのソースを実際に読んで確認する。LLVM の API は版ごとに変わるため、記憶にある関数名は必ず grep で存在を確かめる。
- **ビルド資源**: リンク時のメモリ不足を避けるため lld と `LLVM_PARALLEL_LINK_JOBS=1〜2` を使う。`ccache` があれば有効にする。全体ビルド後は `ninja clang` の差分ビルドで回す。
- **テスト実行**: 変更のたびに関連する lit テストと `tests/addrmap/` の pytest を実行する。マイルストーン完了時に `check-clang` と `check-llvm-codegen-x86` を実行する（AC-5）。
- **変更範囲**: R-NF-5 に従い既存ファイルの変更は最小限にする。無関係なリファクタリング、整形のみの変更はしない。
- **判断の記録**: 本書と異なる設計を選んだ場合、本書で曖昧な点を解釈した場合は、`design.md` に「判断・理由・代替案」を書く。
- **停止条件**: 受け入れ基準を満たせない根本的な理由（LLVM の構造上不可能など）が判明したら、回避策の案を添えて報告し、基準を勝手に緩めない。
- **報告**: 各マイルストーン終了時に、完了条件の結果、`-addrmap-stats` の主要値、未解決事項を簡潔に報告する。

## 13. 決定事項

依頼者に確認済みの事項は以下のとおり。本文には反映済み。

| # | 項目 | 決定 |
| --- | --- | --- |
| Q1 | アーキテクチャ | x86-64 のみ |
| Q2 | LLVM のバージョン | 安定版で新しいもの → `llvmorg-23.1.3`（23.1 系の最新パッチ） |
| Q3 | 対象命令 | アドレス生成とアドレス演算のみ。load/store は対象外 |
| Q4 | `-g` | 併用前提 |
| Q5 | ポインタ経由 | 含める。構造体の内側を指すポインタの取得が最重要 |
| Q6 | 出力の置き場所 | pyelftools で読める ELF 独自セクション `.llvm_addrmap`（JSON は別ファイルにも出せる） |
| Q7 | LTO | 当面対象外。将来追加の可能性があるため設計上の備えだけ行う（7.2） |
| Q8 | 利用目的 | fat pointer の生成。ポインタの生成・導出時の範囲と伝播を知るため |

追加の決定事項:

- レジスタ間のポインタコピーは記録しない（`copy` kind は削除）。
- 関数をまたぐ導出の連鎖は扱わない。各命令の位置で、その命令が行うポインタ操作（元ポインタの静的型から見たパスと範囲）が記録されれば十分とする。例えば `f` 内の `addq $4,%rdi` は「`o->in`」、`pick_b` 内の `leaq 4(%rdi),%rax` は「`ip->b`」と、それぞれ独立に記録する。

未決事項はない。
