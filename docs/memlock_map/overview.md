# memlock_map の仕組み（概要）

`-fmemlock-map` を付けてコンパイルすると、**メモリに触る機械語命令のうち、
他のスレッドと共有されうる（＝デバッグツールが排他制御をする必要がある）ものの一覧**が、
オブジェクトファイルの `.memlock_map` セクションに入る。
逆に言えば、一覧に載っていない命令は「そのスレッドの中だけで完結するアクセス」なので、ロックを省ける。

本書では、その仕組みを図と例で簡単に説明する。細かい規則や判断の理由は [design.md](design.md)、
要求は `memaccess_lockmap.md`（リポジトリ直下）を参照。

## 1. 何ができるか（例）

```c
int shared;
void use(int *);
int f(int x) {
  int a[4];          /* アドレスが外に出ない */
  int b;             /* use() に渡す = 捕獲される */
  a[x & 3] = x;
  b = a[1];
  use(&b);
  shared += b;
  return a[2];
}
```

```sh
clang -O1 -fmemlock-map=all -c ml.c          # 全メモリアクセスと理由を載せる
python3 read_memlock_map.py ml.o --disasm
```

```
f  [.text]  base=0x0 size=0x32 mode=all entries=8
  +0x09  nolock 0x81 local         movl %edi, 0x10(%rsp,%rax,4)   a[x & 3] = x
  +0x0d  nolock 0x81 local         movl 0x14(%rsp), %eax          a[1]
  +0x11  LOCK   0x03 captured      movl %eax, 0xc(%rsp)           b = ...
  +0x1a  nolock 0x83 sp_implicit   callq use                      call の push
  +0x1f  LOCK   0x03 captured      movl 0xc(%rsp), %eax           b を読む
  +0x23  LOCK   0x02 not_alloca    addl %eax, shared(%rip)        グローバル変数
  +0x29  nolock 0x81 local         movl 0x18(%rsp), %eax          a[2]
  +0x31  nolock 0x83 sp_implicit   retq                           ret の pop
```

- `a` はアドレスが関数の外に漏れないので、他スレッドからは触れない → ロック不要（0x81）。
- `b` は `use(&b)` でアドレスが渡されるので、他スレッドに見えうる → ロック必要（0x03）。
- `shared` はグローバル変数 → ロック必要（0x02）。
- `call` / `ret` の暗黙のスタック操作 → ロック不要（0x83）。

通常モード（`-fmemlock-map`）では、ロック必要な 3 つ（+0x11、+0x1f、+0x23）だけが載る。

## 2. 全体の流れ

IR や MIR には手を入れず、**命令を出力する直前（AsmPrinter）だけ**で判定する。

```
 C ソース
   │  clang -fmemlock-map[=all]
   │  （x86-64 Linux ELF・C 以外はエラー）
   ▼
 LLVM IR → 最適化 → ISel → MI パス        （すべて変更なし）
   │
   ▼
 X86AsmPrinter::emitInstruction(MI)
   │
   ├─ X86MemLockMap::handleInstruction(MI)
   │     MachineInstr とその MachineMemOperand (MMO) を見て理由コードを決め、保留する
   │
   ├─ MI → MCInst に変換（通常の変換 / TLS などの独自の展開）
   │
   └─ 各 MCInst を出力する直前に X86MemLockMap::handleMCInst(Inst)
         メモリアクセスなら、その場に一時ラベル .LmemlockN を置き、エントリを記録
   │
   ├─ インラインアセンブリ: 前後にラベルを置き、範囲として記録（0x05）
   ▼
 関数の終わり: X86MemLockMap::endFunction
   └─ 関数フラグメントごとに 1 レコードを .memlock_map に出力
```

ラベルを MachineInstr ではなく **MCInst ごと**に置くのが要点である。
1 つの MachineInstr が複数の機械語命令に展開される場合（例: `TLS_addr64` → `lea` + `call`）でも、
メモリに触る命令そのものの先頭を指せる。

## 3. 判定のしかた

### 3.1 ロック不要になる 4 つの規則

健全性が最優先なので、**以下の 4 つのどれかに当てはまると確実に言えるときだけ**ロック不要にし、
それ以外はすべてロック必要にする。

| 規則 | コード | 内容 | 判定方法 |
| --- | --- | --- | --- |
| A | 0x81 | 捕獲されないローカル変数 | MMO の IR 値を `getUnderlyingObjects` でさかのぼり、すべてその関数の `alloca` で、`PointerMayBeCaptured` が偽 |
| B | 0x82 | コンパイラのスタックスロット（spill など） | MMO が `Stack` / `FixedStack` の PseudoSourceValue |
| C | 0x83 | スタックポインタ経由の暗黙アクセス | `push`/`pop`/`call`/`ret`/`leave` などの許可リスト |
| D | 0x84 | GOT・定数プール・ジャンプテーブルの読み取り | 該当する PseudoSourceValue で、書き込みを含まない |

### 3.2 判定の流れ（MachineInstr から出る通常の命令）

```
 MI
  │
  ├ MachineOutliner が作った関数？ ─────────────────────▶ 0x09
  ├ 明示的メモリオペランドなし & 許可リスト（push など）─▶ 0x83 (C)
  ├ MMO がない／足りない ───────────────────────────────▶ 0x01
  │
  └ MMO ごとに判定 ──┬ Stack / FixedStack ──────────────▶ 0x82 (B)
                     ├ GOT / CP / JT の読み取り ────────▶ 0x84 (D)
                     ├ その他の PseudoSourceValue ──────▶ 0x06
                     ├ IR 値なし／基底が alloca 以外 ───▶ 0x02
                     ├ さかのぼりが途中で打ち切り ───────▶ 0x09
                     ├ alloca が捕獲されている ──────────▶ 0x03
                     └ すべて捕獲されない alloca ───────▶ 0x81 (A)

 複数の MMO があれば、ロック必要なコード（0x0?）を優先し、その中で最小のものを採る
```

### 3.3 それ以外の命令

| 出どころ | 扱い |
| --- | --- |
| AsmPrinter の独自の展開（TLS の `__tls_get_addr` 呼び出し、`__fentry__`、XRay、KCFI など） | `MCInstrDesc` でメモリに触るものは 0x07（TLSDESC は 0x09） |
| インラインアセンブリ | 中身は解析せず、範囲（kind = 1）として 0x05 |
| アセンブラが直前に NOP を入れうる関数（`-mbranches-within-32B-boundaries` など） | ラベルがずれるので、その関数のレコードを丸ごと出さない |

レコードがない関数は、利用側が「全部ロック必要」として扱う約束なので、どれも安全側に倒れる。

## 4. セクションのレイアウト

```
.memlock_map  （SHT_PROGBITS、SHF_LINK_ORDER、非ロード）
   sh_link → 対応するテキストセクション
   テキストセクションごとに 1 つ（COMDAT なら同じグループ）
   → --gc-sections で関数が消えると一緒に消える

 レコード（関数フラグメントごとに 1 つ）
 ┌─────────┬───────┬──────────────┬───────────────┬───────────────┬─────────────┬─────────┐
 │ version │ flags │ base_address │ fragment_size │ function_name │ num_entries │ entries │
 │   u8    │  u8   │ u64 (再配置) │    ULEB128    │ ULEB128 + 文字│   ULEB128   │   ...   │
 └─────────┴───────┴──────────────┴───────────────┴───────────────┴─────────────┴─────────┘
 エントリ: offset (ULEB128) │ kind (u8) │ [length (ULEB128), kind = 1 のときだけ] │ reason (u8)
```

上の例を通常モードでコンパイルしたときの実際のバイト列:

```
01                       version = 1
00                       flags = 0（通常モード。all なら 1）
00 00 00 00 00 00 00 00  base_address（R_X86_64_64 → .text+0 で埋まる）
32                       fragment_size = 0x32
01 66                    function_name = "f"（長さ 1）
03                       num_entries = 3
11 00 03                 +0x11  単一命令  0x03 captured
1f 00 03                 +0x1f  単一命令  0x03 captured
23 00 02                 +0x23  単一命令  0x02 not_alloca
```

- `offset` と `fragment_size` はラベルの差（`emitAbsoluteSymbolDiffAsULEB128`）として出すので、
  アセンブラが命令の長さを確定させた後の正しい値になる。
- `reason` の最上位ビットが 1（0x8?）ならロック不要、0 ならロック必要。利用側はそのビットだけ見ればよい。
- 作り方は既存の `SHT_LLVM_BB_ADDR_MAP`（`-fbasic-block-address-map`）と `.stack_sizes` を参考にしている。

## 5. 主なファイル

| ファイル | 役割 |
| --- | --- |
| `llvm/lib/Target/X86/X86MemLockMap.{h,cpp}` | 判定とセクション出力の本体 |
| `llvm/lib/Target/X86/X86MCInstLower.cpp` | MCInst を出力する箇所へのフック |
| `llvm/lib/Target/X86/X86AsmPrinter.{h,cpp}` | 関数終了時の出力、インラインアセンブリのフック |
| `llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp` | ターゲットのチェック、関数先頭ラベルの生成 |
| `read_memlock_map.py` / `verify_memlock_map.py` | セクションの表示 / 生成コード不変・網羅性などの検査 |

## 6. 制限（抜粋）

- `-flto`、`-fno-integrated-as` はまだエラーにならない（`-flto` ではセクションが出ないだけで、安全側）。
- `volatile` のローカル変数は捕獲扱い（0x03）になる。
- `ud2`、`lfence` など、`MCInstrDesc` 上メモリアクセス扱いの命令は過大に載る。

詳しくは [design.md](design.md) の 11 章を参照。
