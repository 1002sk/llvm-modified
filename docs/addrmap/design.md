# addrmap 実装メモ（設計・判断・現状）

Clang/LLVM アドレス対応表（addrmap）拡張の実装内容をまとめる。要求は
「Clang LLVM アドレス対応表（addrmap）拡張 要求仕様書」（以下、仕様書）に従う。
本書は仕様書 11 章の `design.md` にあたり、仕様書と異なる設計を選んだ点、
仕様書の曖昧な点をどう解釈したかを「判断・理由・代替案」として記録する。

最終更新: 2026-10-22（コミット `91f9d99c5cfc` 時点）

## 1. 現状の要約

| マイルストーン | 状態 | 内容 |
| --- | --- | --- |
| M0 調査・環境構築 | 一部 | ビルド環境を作成。ベースは `llvmorg-23.1.3` ではなく main 系（判断 D1） |
| M1 骨格 | 完了（Python ツールを除く） | オプション伝搬、対象命令の判定、`.llvm_addrmap` の出力 |
| M2 -O0 の完全対応 | 大部分 | DI 型によるパスと範囲、グローバル・ローカル・引数・pointee の解決。フロントエンド注釈は未実装 |
| M3 最適化対応 | 中心部分 | IR 解析パス、ISel での伝搬、MI 作り直し時の引き継ぎ、`not_pointer` 判定、インライン情報、中間値 |
| M4 応用ケース | 一部 | `derived_from`（ブロック内）、LSR、`mask`、整数経由、共用体、フレキシブル配列、範囲外 |
| M5 仕上げ | 一部 | `-addrmap-stats` のみ。asm コメント、性能測定、実コード評価、ドキュメントは未着手 |

コミット（ブランチ `additional-debuginfo`）:

| コミット | 内容 |
| --- | --- |
| `1ae9e078e708` | `-faddrmap` と `.llvm_addrmap` の出力、対象命令の判定 |
| `24f40a5b02a6` | DI 型によるパスと範囲、スタック変数・ポインタ変数の特定、`derived_from` |
| `e90981cc8026` | IR 解析パス（`!addrmap`）と MachineInstr までの伝搬、ALU 命令の記録、`-addrmap-stats` |
| `91f9d99c5cfc` | 分割された計算の中間値（`intermediate_of`） |

## 2. 使い方

```sh
clang -g -O2 -faddrmap -c foo.c            # .o に .llvm_addrmap が入る
clang -g -O2 -faddrmap -S foo.c            # 同じ内容をディレクティブで出す
clang -g -O2 -faddrmap -mllvm -addrmap-stats -c foo.c   # 統計を stderr に出す
llc -addrmap foo.ll                        # llc から直接使う場合
```

- `-faddrmap` / `-fno-addrmap` はドライバと cc1 の両方で受け付ける。
- 対象は C だけ。C++ や Objective-C の入力に指定するとエラーになる（判断 D14）。
- 出力するのは x86-64 の ELF だけ。i686 や他の形式では何も出さない。
- `-g` がないと型情報が取れないので、パスは空になり `confidence` は下がる。

ビルド構成（`build/`）: `-DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_ASSERTIONS=ON
-DLLVM_ENABLE_PROJECTS="clang;lld" -DLLVM_TARGETS_TO_BUILD=X86
-DLLVM_USE_LINKER=gold`。手元に lld がなかったため、lld も一緒にビルドしている。

## 3. 全体構成

```
Clang                  -faddrmap → CodeGenOptions::AddrMap → TargetOptions::EmitAddrMap
  │
IR 最適化（変更なし）
  │
CodeGenPrepare … StackProtector
  │
AddrMapAnnotate        GEP / inttoptr に !addrmap（ポインタの説明）、
  │  (IR パス)          整数演算に !addrmap "int" を付ける
  │
ISel                   SelectionDAG: NodeExtraInfo → MI
  │                    FastISel: 結果の vreg を定義する MI
MachineInstr::ExtraInfo::AddrMap
  │                    命令の作り直し（2 アドレス変換、FixupLEAs など）で引き継ぐ
後段の MI パス（変更なし）
  │
X86AsmPrinter          命令ごとに X86AddrMap::handleInstruction
  │                    → 対象命令なら直前にラベルを置き、レコードを作る
doFinalization         X86AddrMap::emitSection → .llvm_addrmap
```

| ファイル | 役割 |
| --- | --- |
| `llvm/lib/CodeGen/AddrMapAnnotate.cpp` | IR 解析パス。値ごとに「ルート＋定数オフセット＋可変項」を推論して注釈を付ける |
| `llvm/lib/CodeGen/AddrMapTypes.cpp`、`llvm/include/llvm/CodeGen/AddrMapTypes.h` | DI 型の扱い（C の型名、サイズ）、パス解決、注釈のエンコードとデコード |
| `llvm/lib/Target/X86/X86AddrMap.cpp`、`.h` | 対象命令の判定、ベースの特定、レコードの組み立て、セクションの出力、統計 |
| `X86AsmPrinter.cpp`、`X86MCInstLower.cpp` | 呼び出しフック（開始時の有効判定、命令ごとの呼び出し、終了時の出力） |
| `MachineInstr.h/.cpp`、`MachineFunction.h/.cpp` | ExtraInfo の `AddrMap` フィールド |
| `SelectionDAG.h`、`SelectionDAGBuilder.cpp`、`ScheduleDAGSDNodes.cpp`、`SelectionDAGISel.cpp` | DAG と FastISel での伝搬 |
| `TwoAddressInstructionPass.cpp`、`MachineFunction.cpp`（`substituteDebugValuesForInst`） | 命令の作り直しでの引き継ぎ |
| `TargetPassConfig.cpp`、`Passes.h`、`InitializePasses.h`、`CodeGen.cpp` | 注釈パスの登録とパイプラインへの追加 |
| `TargetOptions.h`、`CommandFlags.*`、Clang の `Options.td` ほか | オプションの伝搬 |

## 4. 出力形式

### 4.1 セクション

仕様書 4.1 のとおり。`SHT_PROGBITS`、非アロケート、アラインメント 8。
ユニットは「ヘッダ 48 B → 索引エントリ（16 B × N）→ JSONL → 8 B 境界までの 0 埋め」。

- `pc` は対象命令の直前に置いた一時ラベル `.LaddrmapN` への `R_X86_64_64`。
- `tu_id` は、ターゲット文字列とレコード行の MD5。ソースパスと時刻は含まない。
- 索引の `flags` は今のところ常に 0（1 命令 1 レコード）。
- `-S` ではセクションの中身もディレクティブ（`.ascii` など）で出すので、
  アセンブルすると同じバイト列になる。

### 4.2 JSONL のフィールド

仕様書 4.2 のレコード形式に従う。実装上の補足:

| フィールド | 内容 |
| --- | --- |
| `kind` | `addr` / `addr_got` / `derive` / `mask` / `not_pointer` |
| `insn.addr` | LEA などはメモリオペランドそのもの。ALU 命令は `base_reg` = 演算元、`disp` = 即値（SUB は符号を反転）、`index_reg` = レジスタオペランド |
| `base.kind` | `global` / `static_local` / `tls` / `string_literal` / `compound_literal` / `local` / `param` / `pointee` / `unknown` |
| `base.decl` | **追加**。宣言位置 `{file, line}`（仕様書 3.3-3 の「宣言位置」） |
| `src_ptr` | `derive` のときだけ出す |
| `path` / `path_str` | 6 章の規則で解決したアクセスパス |
| `ptr_offset` | `const` は基準からの定数オフセット（不明なら null）。`var` は可変項 `{reg, scale}`（レジスタ不明なら `reg: null`） |
| `source_expr` | 未実装のため常に null |
| `confidence` | `exact`（IR 注釈で整数演算と確定した `not_pointer`）/ `derived`（機械語オペランド・DI 情報・IR 注釈から確定）/ `heuristic`（推測を含む） |
| `provenance` | `machine-operand` / `debuginfo-variable` / `debuginfo-type` / `ir-annotation` / `derived-from-record` / `intermediate` の組み合わせ |
| `intermediate_of` | **追加**。中間値のときだけ、最終値のレコード ID を `reason` の直前に出す |

`not_pointer` のレコードは仕様書どおり `insn`、`source`、`confidence`、`reason` だけを持つ。

## 5. 処理の詳細

### 5.1 対象命令（X86AddrMap）

| 命令 | 条件 | kind |
| --- | --- | --- |
| LEA（`LEA64r`、`LEA32r`、`LEA64_32r`、`LEA16r`） | すべて | 下の分類表で決める |
| TLS 疑似命令（`TLS_addr64`、`TLS_base_addr64`、`TLS_desc64`） | すべて。ラベルは `data16` プレフィックスの前（命令の先頭）に置く | `addr` |
| `MOV32ri` / `MOV64ri32` / `MOV64ri` | 即値がシンボル | `addr` |
| `MOV64rm` | GOT 参照（`GOTPCREL`、`GOTPCREL_NORELAX`、`GOTTPOFF`） | `addr_got` |
| `MOV64rr` | 元がスタックポインタまたはフレームポインタ | `addr` |
| `ADD64ri32/ri8/rr/rm`、`SUB64*`、`INC64r`、`DEC64r` | ポインタの注釈がある、または元レジスタが記録済みのポインタを持つ | `derive` |
| `AND64ri32/ri8/rr`、`OR64ri32/ri8/rr` | 同上 | `mask` |

`FrameSetup` / `FrameDestroy` の付いた命令（プロローグとエピローグ）は対象外にする。

LEA の分類（上から順に判定）:

1. シンボルがある → `addr`
2. 書き込み先が 32 ビット以下 → `not_pointer`（`derived`）
3. ベースがスタックポインタ、フレームポインタ、ベースポインタ → `addr`
4. IR 注釈が `int` → `not_pointer`（`exact`）
5. IR 注釈がポインタ → `derive` または `mask`
6. ベースがない、または base == index → `not_pointer`（`heuristic`）
7. それ以外 → `derive`。ただし DBG_VALUE からベースレジスタが整数変数と分かれば `not_pointer`

### 5.2 ベースの特定（X86AddrMap）

- **シンボル**: `GlobalVariable` と DI 情報から種別を決める。
  - TLS → `tls`
  - `.str*` → `string_literal`
  - `.compoundliteral*` → `compound_literal`
  - DI のスコープが関数内 → `static_local`
  - それ以外 → `global`
  - サイズは DataLayout から取る。宣言だけでサイズが 0 のものは `size_unknown: "incomplete type"`。
  - 関数、ジャンプテーブル、定数プール、外部シンボルは `unknown` とし、`reason` に何かを書く。
- **スタック**: `MF.getInStackSlotVariableDbgInfo()`（dbg.declare の付いた変数とスロット）の各変数について、
  `getFrameIndexReference`（rsp 基準のときは `getFrameIndexReferencePreferSP` も）で
  「レジスタ＋オフセット」を求め、命令のアドレスがその変数の範囲に入るかを照合する。
  - 複数の変数が当てはまったときは、命令のスコープ（インライン先を含む）に含まれる変数を優先する。
    それでも残った変数は `alternatives` に入れ、それぞれの型でパスも解決する（StackColoring 対策）。
  - どの変数にも当てはまらなければ、IR 注釈のルートを使う。それもなければ `unknown`。
- **derive / mask**: 次の順に試す。
  1. IR 注釈のルートが分かっていれば、それを使う。オフセットと可変項も注釈の値（IR の値全体の説明）で置き換える。
  2. 元レジスタが、同じブロック内で記録済みのポインタを持っていれば、そのベースを引き継ぎ、オフセットを足す。
  3. 同じブロック内で元レジスタを指す DBG_VALUE を後ろ向きに探し、ポインタ変数ならその pointee を使う。
  4. どれも当てはまらなければ `pointee`、型不明。
- **ブロック内の追跡**: 記録した命令の書き込み先レジスタを `LiveValues` に登録する。
  後の命令が上書きしたり、call の regmask で破壊されたりしたら外す。
  基本ブロックが変わったら全部消す。これを `derived_from` と、中間値の判定に使う。
- **ループで動くポインタ**: ルートは分かるが定数オフセットが不明（PHI で値が変わる）な `derive` は、
  要素サイズ刻みの可変添字として扱う（`b[?]`、`heuristic`、理由「offset varies in a loop」）。
- **中間値**: パスを解決できたレコードが、同じブロック内の「パスを解決できなかった `derive`」から
  導出されていた場合、その導出元をたどれるだけたどり、それぞれに `intermediate_of` と理由を付ける。
  導出元のベースが不明なら最終値のベースで補う（その場合 `ptr_offset.const` は null）。
  そのため、レコードは構造体で持っておき、セクションを出すときにまとめて JSON にする。

### 5.3 パス解決（AddrMapTypes）

`resolvePath(Root, Offset, Vars, ViaPointer)` は「ルートの型＋定数オフセット＋可変項」を
フィールドと添字の列に変換する。

- 型は DI 型だけを使う。IR の型は使わない（仕様書 7.2）。typedef と修飾子は外す。
  多次元配列は次元ごとに 1 段とする。
- **構造体**: オフセットを含むメンバに降りる。ビットフィールドは対象外。
  無名メンバは段としては残すが、`path_str` には名前を出さない（`ga.s2`）。
  フレキシブル配列メンバは長さ不明として扱う（`length: null`、`flexible: true`）。
- **共用体**: オフセットを含むメンバが 1 つならそこに降りる。複数なら、そこで止めて全候補を `alternatives` に入れる。
- **配列**: `index = floor(Offset / 要素サイズ)` で添字を求める。範囲外なら `out_of_bounds: true` にして止める。
- **可変項**: 配列の段では、「scale が要素サイズと一致する」か「scale が 1」のときに消費して、添字を `?` にする。
  scale 1 は事前にバイト数へ掛け算済みのものとみなし、外側の配列に当てる。
  これで `lea 12(%rdi,%rcx)` → `p->items[?].pos`（仕様書 4.2 の例）が再現できる。
  消費されずに残った可変項は「途中の値」とみなし、パスを出さない（`p->n` のような誤表示を避ける）。
- **ポインタ自体の添字**（`ViaPointer`）: 可変項の scale が pointee のサイズと一致するとき、
  またはオフセットが pointee の範囲外のときは、ポインタの添字として扱う（`p[?]`、`p[-1]`）。
  `p[-1]` は範囲外とする。
- **オフセット 0**（R-PATH-5）: そこで止めて `path` を空にし、先頭メンバ（配列なら `[0]`）の連鎖を `alternatives` に入れる。
  共用体に当たったら全メンバに分岐する。
- **フィールドの途中**（R-PATH-2）: スカラやパディングの途中を指すときは `byte_offset_in_field` を入れる。
  この場合 `result_type` は空にする。
- **`void *` / `char *`**（R-PTR-6）: フィールドに分解しない。パスは空、`result_type` は元の型、`confidence` は `derived`。

### 5.4 IR 解析パス（AddrMapAnnotate）

`TargetPassConfig::addISelPrepare` の末尾（StackProtector の後、ISel の直前）に、
`-faddrmap` のときだけ入れる。メタデータを付けるだけで、IR は変えない。

注釈の形式（`llvm/include/llvm/CodeGen/AddrMapTypes.h`）:

```
!addrmap !{!"int"}                                   ; 整数演算
!addrmap !{!"ptr"|!"mask", !"<root kind>", <root>, i64 <offset>|null,
           !{i64 <scale>...}, !"<root expr>", <source pointer DIType>|null,
           i1 <heuristic>}
```

ルートの種類は次の 3 つ。

- `global`: `GlobalVariable`
- `var`: dbg.declare の付いた `DILocalVariable`
- `pointee`: ポインタ値の指す型。`root expr` でその値を表す（`p`、`p->next`、`(*q)`）。null は void を表す。

値ごとの推論規則（再帰、メモ化、深さ 32 まで）:

| 値 | 規則 |
| --- | --- |
| `GlobalVariable` | ルート `global`、オフセット 0 |
| GEP（命令・定数式） | ベースの情報に `collectOffset` の定数部と可変部（scale）を足す |
| #dbg_value のある値 | その変数がポインタ型なら、ルート `pointee`（その変数名）。GEP には適用しない（GEP は構造的な説明を優先） |
| `alloca` | #dbg_declare の変数をルート `var` にする |
| `load` | 読み出し元のパスを解決し、その型がポインタならルート `pointee`（式は `p->next` など）。オフセット 0 で型がポインタでなければ、先頭メンバの連鎖からポインタ型を探す。ポインタ自体を読むときは `(*q)` |
| PHI / select | 入力のルートがすべて同じなら、そのルート。オフセットが違う、または循環する（ループ）ならオフセットを不明にする。循環の途中で求めた値はキャッシュしない |
| `inttoptr` | `ptrtoint` ± 定数 → 元ポインタ＋定数（`heuristic`）。`and` / `or` → `mask` |

注釈を付ける対象:

- `getelementptr` と `inttoptr` → `ptr` / `mask`
- 整数の `add` / `sub` / `mul` / `shl` / `or` → `int`。ただし結果が `inttoptr` に使われるものは除く。

### 5.5 MachineInstr への伝搬

- **MachineInstr**: `ExtraInfo` の MDNode の並びに `AddrMap` を追加した（`getAddrMap` / `setAddrMap`）。
  - `isIdenticalTo` では比較しない。そのため CSE などの判断は変わらない。
  - コピーコンストラクタは `Info` をそのまま引き継ぐので、複製（再実体化、末尾複製など）でも注釈は残る。
- **SelectionDAG**: `NodeExtraInfo::AddrMap` を追加した。
  - `SelectionDAGBuilder::visit` が、IR 命令の値に対応するノードにだけ付ける。
  - ISel でノードが置き換わると、`ReplaceAllUsesWith` → `copyExtraInfo` で引き継がれる。
  - `EmitSchedule` で MI に設定する。
  - `!pcsections` と違い、ISel 中に挿入された全ノードには配らない（途中の計算に付けないため）。
- **FastISel**: `SelectionDAGISel::SelectAllBasicBlocks` で、命令の選択に成功した直後に、
  結果の vreg を定義する MI のうち「この選択で新たに作られたもの」に付ける。
- **命令の作り直し**:
  - `MachineFunction::substituteDebugValuesForInst` の先頭で Old から New へ写す。
    X86FixupLEAs、InlineSpiller の folding などがこの関数を経由する。
  - 2 アドレス変換（`convertToThreeAddress`）では個別に写す。

## 6. 判断・理由・代替案

| # | 判断 | 理由 | 代替案 |
| --- | --- | --- | --- |
| D1 | ベースを `llvmorg-23.1.3` ではなく、このリポジトリの main 系（23.0.0git）にした | 依頼者がこのリポジトリの改変を指示したため | 23.1.3 のタグから切り直し、パッチを移植する（API の違いを要確認） |
| D2 | 注釈のメタデータ種別を `!addrmap` の 1 種類にし、先頭のタグで `ptr` / `mask` / `int` を区別する（仕様書の `!addrmap.int` はタグで表す） | MI に持たせるフィールドが 1 つで済む。固定のメタデータ種別を増やすとビットコードの種別表が変わり、既存テストに影響する | 種別を 2 つ（`!addrmap`、`!addrmap.int`）にし、ExtraInfo のフィールドも 2 つにする |
| D3 | フレームインデックスを PEI の前に変数へ対応付けるパス（仕様書 7.1-4）は作らず、AsmPrinter で「dbg.declare のスロットのフレームオフセット」と照合する | PEI の後でも `getFrameIndexReference` でスロットの位置が分かる。パスを 1 つ減らせ、MI の作り直しで情報が失われる心配もない | 仕様書どおり PEI の前に FI を MI に記録する。dbg.declare のないスタック領域（一時領域、複合リテラル）も扱えるようになる |
| D4 | `movq %rsp, %rdi` などフレームレジスタのコピーを `addr` として記録する | 決定事項の「レジスタ間のポインタコピーは記録しない」は、既存ポインタのコピーを指すと解釈した。これはローカル変数のアドレス生成（`sink(a)` など）であり、記録しないと fat pointer 化に必要な点を落とす | 決定事項を字義どおりに適用し、記録しない |
| D5 | ALU 命令（ADD/SUB/INC/DEC/AND/OR）は、IR 注釈があるか、元レジスタが記録済みのポインタを持つときだけ対象にする | ポインタかどうかは機械語では判別できない（仕様書 R-COV-2）。注釈がなくても、ブロック内で追跡できれば `-O0` の段階的な ADD を拾える | 注釈がある命令だけを対象にする |
| D6 | 可変項が配列の添字に当てはまらないとき、パスを出さない | 途中の値（`p + 12*i` など）を、オフセット 0 の `p->n` のように誤って表示するのを避けるため | 当てはまった所までのパスを出し、`heuristic` にする |
| D7 | 中間値に `intermediate_of` を追加した（仕様書にないフィールド） | 分割された計算の途中経過を、最終値と結び付けて説明するため。既存フィールドの並びを崩さないよう、中間値のときだけ `reason` の直前に出す | `reason` の文字列だけで表す |
| D8 | `base.decl` を追加した | 仕様書 3.3-3 の「宣言位置」に対応するフィールドが 4.2 の例にないため | 出さない |
| D9 | ExtraInfo の `numTrailingObjects(MDNode*)` が `HasMMRAs` を数えていなかったのを直した | 新しいフィールドを同じ MDNode の並びに置くため、正しく数えないと後ろの `uint32_t`（CFIType）と重なる。既存の潜在バグでもある | 新しいフィールドを別の型の並びに置く |
| D10 | 注釈パスは旧パスマネージャの CodeGen パイプラインにだけ入れた | Clang の x86 コード生成は旧パスマネージャを使う | 新パスマネージャの `CodeGenPassBuilder` にも追加する（LTO への備え、仕様書 7.2） |
| D11 | ループで動くポインタは、要素サイズ刻みの可変添字として扱う | 仕様書 8 章の LSR の期待値（`a[?]`）に合わせるため | オフセット不明としてパスを出さない |
| D12 | `confidence` の使い分けを決めた。`exact` は IR 注釈で整数演算と確定した `not_pointer` に限る。機械語オペランド・DI 情報・IR 注釈から決まったものは `derived` | 仕様書に定義がないため。フロントエンド注釈を実装したら、それ由来のものを `exact` にする予定 | IR 注釈由来もすべて `exact` にする |
| D14 | `-faddrmap` は C だけで受け付け、C++、Objective-C などでは cc1 でエラーにする（`invalid argument '-faddrmap' not allowed with 'C++'`）。LLVM IR の入力は通す | 仕様書の対象は C だけ。C++ で試したところ、次の不具合があった。<br>・inline 関数などの COMDAT を含むオブジェクトを gold でリンクすると、捨てられたセクションへの参照でエラーになる<br>・基底クラスの部分オブジェクト（`DW_TAG_inheritance`）を扱わず、基底クラスのメンバへのパスが誤る<br>・参照型をポインタとして扱わない<br>IR を通すのは `-save-temps`（C → IR → オブジェクト）を動かすため | C++ に対応する。COMDAT は関数ごとのユニット（`SHF_LINK_ORDER` か COMDAT グループ）にし、継承と参照型を型の解釈に加える |
| D13 | TLS の initial exec（`movq x@GOTTPOFF(%rip)`）は `addr_got` にしている | GOT を読む命令の形として統一したため | GOT から読むのは TLS オフセットなので、`addr`（`tls`）と後続の `addq %fs:0` などを組み合わせて扱う |

## 7. 検証

### 7.1 lit テスト

| テスト | 内容 |
| --- | --- |
| `llvm/test/CodeGen/X86/addrmap-section.ll` | セクションのレイアウト、ラベル、再配置、PIC と static、i686 では出さないこと |
| `llvm/test/CodeGen/X86/addrmap-tls.ll` | TLS の general dynamic（`data16` の前のラベル）と local exec |
| `llvm/test/CodeGen/X86/addrmap-annotate.ll` | IR 注釈の形式（`-stop-after=addrmap-annotate`）と、`-O0`（FastISel）/ `-O2`（SelectionDAG）での伝搬 |
| `clang/test/CodeGen/addrmap-section.c` | Clang からの出力、`-emit-llvm` では出さないこと |
| `clang/test/CodeGen/addrmap-path.c` | パス解決（入れ子、多次元、範囲外、無名メンバ、共用体、フィールドの途中、スタック変数、引数、`derived_from`） |
| `clang/test/CodeGen/addrmap-arith.c` | `-O0` の ADD の連鎖、`p->next->val`、ループ、マスク、`(*q)[2]`、`char *` の演算、中間値、`-addrmap-stats` |
| `clang/test/Driver/faddrmap.c` | ドライバから cc1 への伝搬 |

### 7.2 回帰テスト
- `llvm/test/CodeGen`（全体）と `llvm/test/DebugInfo`、計 30890 件で失敗なし。
- CodeGen の単体テスト（MachineInstr を含む）260 件で失敗なし。
- `clang/test/CodeGen` と `clang/test/Driver` の失敗 8 件は、どれも未ビルドのツール
  （`clang-offload-bundler`、`clang-linker-wrapper`）によるもの。

### 7.3 コーパスでの確認
compiler-rt の builtins、`llvm/lib/Support/reg*.c`、自作のケースを合わせた約 195 ファイルで、
`-O0`/`-O1`/`-O2`/`-O3`/`-Os`/`-Oz`、`-fPIC`、`-fno-pie`、`-fno-omit-frame-pointer`、`-g0` を組み合わせて確認した。

- **網羅性**（R-COV-1）: 逆アセンブルした全 LEA、シンボルの即値取り込み、GOT からの MOV にレコードがあることを、
  簡易 verify スクリプトで確認。漏れは 0 件。
- **コード不変性**（R-NF-1）: `-faddrmap` の有無で `.text` がバイト単位で一致。
- **決定性**（R-NF-3）: 2 回ビルドした `.o` が一致。
- **堅牢性**（R-NF-2）: Assertions 有効ビルドでクラッシュ 0 件。
- **読み取り**（AC-10）: `.o`、lld でリンクした PIE と非 PIE、`--gc-sections` 付きの実行ファイルを pyelftools で読める。

ポインタのレコードのうち `confidence: derived` の割合（同じコーパス、`-O2`）は、段階が進むにつれ次のように上がった。

| 段階 | `derived` の割合 | パスあり |
| --- | --- | --- |
| DI 型による解決まで | 46.9 % | 47.5 % |
| IR 注釈の導入後 | 65.8 % | 56.7 % |

`-O0` では、IR 注釈の導入によって ADD による計算 295 件が新たに `derive` として記録されるようになった。

## 8. 既知の制限と未実装

**MUST**:
- フロントエンド注釈（7.1-1、R-SRC-1）。`source_expr` は常に null。そのため次の 4 つも未対応。
  - オフセット 0 の意図
  - `decay: true`（R-PATH-6）
  - 共用体で使われたメンバ
  - キャスト後の型（R-PTR-6。今は変数の宣言型を使う）
- `-g` なしで `-faddrmap` を指定したときの警告（5 章）。
- JSON Schema（`addrmap.schema.json`）。
- Python ツール（`tools/addrmap/`、`dump` / `verify`）と E2E テスト（`tests/addrmap/`）。
- 受け入れ基準の測定（AC-3〜5、AC-7 の `check-clang` 全体、AC-9 の性能）。
- 注釈の消失件数（AC-2）。アドレッシングモードへの吸収と区別できないため、
  `-addrmap-stats` の `derive_without_annotation` は「消失の候補」の件数でしかない。

**SHOULD / MAY**:
- `-faddrmap-kinds=`、`-faddrmap-asm-comments`（4.3）、`-faddrmap-json=`。
- 同一 PC に複数レコード（R-COV-6、分岐統合）。
- 基本ブロックをまたぐ `derived_from`（R-PTR-3）。
- `container_of` で外側の型を `alternatives` に入れること（R-PTR-5）。
- `-O0` のマスク。FastISel では `inttoptr` が何もしない変換になり、AND が後から選択されるため注釈が付かない。
- `SHF_LINK_ORDER` で関数ごとに除去できる構成。

**その他**:
- ヘッダの `producer` が仕様書の形式（`clang 23.1.3 addrmap-patch <git-hash>`）になっていない。
  `opt_level` で `-Os` / `-Oz` を区別していない。
- dbg.declare のないスタック領域（一時領域、スタック上の複合リテラル）は `unknown` になる。VLA と `alloca()` は未確認。
- 新パスマネージャの CodeGen パイプラインと GlobalISel では、注釈を付けない・運ばない（D10）。
- MIR の出力と読み込みは `AddrMap` フィールドを扱わない。
- ドキュメント（`progress.md`、`BUILD.md`、`README.md`）と `git format-patch` 一式は未作成。
