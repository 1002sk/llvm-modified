# memlock map 実装メモ（設計・判断・現状）

メモリアクセス・ロック要否マップ（`.memlock_map`）の実装内容をまとめる。要求は
`memaccess_lockmap.md`（以下、仕様書）に従う。本書は仕様書 9.1 の 5「設計メモ」にあたる。
仕様書が求める次の内容を含む。

- 第 4 節 C（スタックポインタ経由の暗黙アクセス）の許可リスト（5 章）
- 第 5 節 5（MC 層で生成される命令）の洗い出し（6 章）
- 参考にした既存実装（9 章）
- 既知の制限（11 章）

あわせて、仕様書の曖昧な点をどう解釈したかを「判断」として記録する（8 章）。

最終更新: 2026-10-24（分岐の整列への対応を追加）

## 1. 現状の要約

| 項目 | 状態 |
| --- | --- |
| セクション出力（6 節） | 完了。属性、COMDAT、リンク順序、レコード形式（バージョン 1） |
| 判定規則 A〜D（4 節） | 完了 |
| インラインアセンブリ（範囲、0x05） | 完了。出力 0 バイトの場合だけ制限あり（11 章） |
| MC 層の命令（0x07、TLSDESC は 0x09） | 完了 |
| オプション（7 節） | 完了。clang の `-fmemlock-map[=all]`・`-fno-memlock-map`、llc の `-memlock-map=` |
| エラーにする組み合わせ（7 節） | ターゲットと言語は完了。`-flto`、`-fno-integrated-as` は未実装 |
| 命令の直前にパディングを入れる機能（5.4） | 完了。保証できない関数はレコードを出さない（判断 D9） |
| 読み取りスクリプト・検証スクリプト（9.1 の 3、4） | 完了。`read_memlock_map.py`、`verify_memlock_map.py` |
| lit テスト（9.1 の 2） | 一部。リンク後の確認（9.2 の 14、15）は未実装 |
| 受け入れ用 C プログラム（9.4） | 未着手 |

コミット（ブランチ `additional-debuginfo`）:

| コミット | 内容 |
| --- | --- |
| `f38b04b2e9a6` | `-fmemlock-map` とセクション出力の骨格（理由コードは仮）、読み取りスクリプト、仕様書 |
| `32ce632b6ef4` | 判定規則 A〜D、インラインアセンブリの範囲、lit テスト |
| `046f35721974` | ラベルを MCInst 単位に変更し、MC 層の命令を登録 |
| `662ca8458bb0` | 仕様書 5.6・9.3-1 の改訂、検証スクリプト |
| `690ffbe470e4` | x86-64 Linux ELF 以外のターゲット、C 以外の言語をエラーにする |

## 2. 使い方

```sh
clang -O2 -fmemlock-map -c foo.c         # ロック必要な命令だけを載せる（通常モード）
clang -O2 -fmemlock-map=all -c foo.c     # 全メモリアクセス命令と理由コードを載せる
clang -O2 -fmemlock-map -S foo.c         # 同じ内容をディレクティブで出す
llc -memlock-map=lock|all|none foo.ll    # llc から直接使う場合

python3 read_memlock_map.py foo.o --disasm            # 関数ごとに表示（.o・リンク済み ELF）
python3 verify_memlock_map.py none.o lock.o all.o     # 9.3 の 1〜4 を検査
```

- cc1 は `-fmemlock-map=none|lock|all` を受け付ける。ドライバの `-fmemlock-map` と `-fno-memlock-map` は、それぞれ `=lock` と `=none` の別名。ドライバは `none` 以外のときだけ cc1 に渡す。
- 二つのスクリプトは pyelftools を使い、`verify_memlock_map.py` はさらに Capstone を使う。手元の環境では python3.10 のユーザー領域に入っているので、`PYTHONPATH=~/.local/lib/python3.10/site-packages` を付けて実行する。

## 3. 全体構成

```
clang -fmemlock-map[=all]
  │  CodeGenOptions::MemLockMap → TargetOptions::MemLockMap（MemLockMapMode）
  │  CompilerInvocation: ターゲット・言語のチェック
  │
（IR・MIR は変更しない）
  │
AsmPrinter::doInitialization      ターゲットのチェック（全バックエンド共通）
AsmPrinter::SetupMachineFunction  有効時は関数先頭ラベル（func_begin）を必ず作る
  │
X86AsmPrinter::emitInstruction(MI)
  ├ X86MemLockMap::handleInstruction(MI)    MI を判定し、理由コードを保留する
  ├ （通常の変換）MCInstLowering.Lower(MI) → setDirect(true) → MCInst を出力
  └ （独自の展開）LowerTlsAddr、LowerFENTRY_CALL など → MCInst を出力
        │
        └ 各 MCInst の出力直前: X86MemLockMap::handleMCInst(Inst)
              必要ならラベルを打ち、エントリを記録する
emitInlineAsmStart / emitInlineAsmEnd       インラインアセンブリの前後にラベル（範囲）
  │
X86AsmPrinter::runOnMachineFunction の最後
  └ X86MemLockMap::endFunction             関数フラグメントごとにレコードを出力
```

主なファイル:

| ファイル | 役割 |
| --- | --- |
| `llvm/lib/Target/X86/X86MemLockMap.{h,cpp}` | 判定とセクション出力の本体 |
| `llvm/lib/Target/X86/X86MCInstLower.cpp` | MCInst 出力箇所へのフック、通常の変換の目印（`setDirect`） |
| `llvm/lib/Target/X86/X86AsmPrinter.{h,cpp}` | `X86MemLockMap` の保持、関数終了時の出力、インラインアセンブリのフック |
| `llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp` | ターゲットのチェック、関数先頭ラベルの生成 |
| `llvm/include/llvm/Target/TargetOptions.h` | `MemLockMapMode` |
| `llvm/lib/CodeGen/CommandFlags.cpp` | llc の `-memlock-map=` |
| `clang/include/clang/Options/Options.td` ほか | clang のオプションと cc1 への転送 |
| `clang/lib/Frontend/CompilerInvocation.cpp` | ターゲット・言語のエラー |

## 4. 判定の実装

### 4.1 判定の単位

判定は機械語命令（MCInst）ごとに行う。

- **通常の変換で出る MCInst:** MachineInstr を `X86MCInstLower::Lower` で 1 つの MCInst にする経路で出るもの。`emitInstruction` の最後で `setDirect(true)` を立てて区別する。これだけは MachineInstr の MMO を使って規則 A〜D で判定する。
- **それ以外でメモリに触る MCInst:** 独自の展開で出るものは 0x07 にする。メモリに触るかどうかは、`MCInstrDesc` の `mayLoad`・`mayStore` と、規則 C の許可リストで決める（判断 D1）。

### 4.2 MachineInstr の判定（`X86MemLockMap::classify`）

1. MachineOutliner が作った関数（`MF.isOutlined()`）の命令は 0x09 にする（仕様 4.3）。
2. 明示的なメモリオペランドがなく（`X86::getFirstAddrOperandIdx(MI) < 0`）、許可リスト（5 章）に入っている命令は 0x83 にする（規則 C）。
3. MMO が 1 つもなければ 0x01 にする。
4. 明示的なメモリオペランドがない命令（文字列命令など）は、`mayLoad` なら読みの MMO、`mayStore` なら書きの MMO がなければ 0x01 にする（判断 D3）。
5. 残りは MMO ごとに判定し（4.3）、ロック必要な理由のうち最小のコードを記録する。ロック必要な理由が一つもなければ、ロック不要の理由のうち最小のコードを記録する。

### 4.3 MMO の判定（`X86MemLockMap::classifyMemOperand`）

| 条件 | コード |
| --- | --- |
| PseudoSourceValue が `Stack`・`FixedStack` | 0x82（規則 B） |
| PseudoSourceValue が `GOT`・`ConstantPool`・`JumpTable` で、書き込みを含まない | 0x84（規則 D） |
| 同じ種類で書き込みを含む、またはそれ以外の PseudoSourceValue | 0x06 |
| IR の値がない | 0x02（判断 D2） |
| `getUnderlyingObjects` の結果が空 | 0x02 |
| 結果に、探索を打ち切った値（GEP、PHI、select、bitcast、addrspacecast）が残っている | 0x09（判断 D4） |
| 結果に `AllocaInst` 以外がある、または別の関数の `AllocaInst` | 0x02 |
| `PointerMayBeCaptured(AI, ReturnCaptures=true)` が真 | 0x03 |
| 上のどれにも当たらない（すべて捕獲されないローカル変数） | 0x81（規則 A） |

- 一つの MMO に基底オブジェクトが複数ある場合も、最小のコードを採る。
- 捕獲判定は `AllocaInst` ごとに関数単位でキャッシュし、関数の終わりに捨てる（仕様 5.2）。
- `PointerMayBeCaptured` のブール版は、どの成分（アドレス・provenance）が捕獲されても真を返し、探索上限に達した場合も真になる（仕様 4.1 A の 4）。
- 規則 D の対象は GOT・定数プール・ジャンプテーブルの読み取りだけで、TLSDESC の記述子の読み取りは含めない（6 章で 0x09）。

## 5. 規則 C の許可リスト

明示的なメモリオペランドを持たない形のときだけ 0x83 になる。`X86MemLockMap::isImplicitStackAccess` に列挙している。

| オペコード | 命令 |
| --- | --- |
| `PUSH64r`、`PUSH64i8`、`PUSH64i32` | `push` |
| `PUSHP64r` | `pushp`（APX） |
| `PUSH2`、`PUSH2P` | `push2`、`push2p`（APX） |
| `PUSHF64` | `pushfq` |
| `POP64r` | `pop` |
| `POPP64r` | `popp`（APX） |
| `POP2`、`POP2P` | `pop2`、`pop2p`（APX） |
| `POPF64` | `popfq` |
| `CALL64pcrel32` | `call rel32` |
| `CALL64r`、`CALL64r_NT`、`CALL64r_ImpCall` | `call reg` |
| `RET64`、`RETI64` | `ret`、`ret imm16` |
| `LEAVE64` | `leave` |

入れなかったもの:

- **`ENTER`:** コンパイラが生成しない。
- **`TAILJMP*`、`JMP*`:** メモリに触らない。メモリ経由の形（`TAILJMPm64` など）は MMO で判定する。
- **`*_RVMARKER`:** Objective-C 専用で、AsmPrinter より前に展開される。
- **`PUSH64rmm`、`CALL64m` など明示的メモリオペランドを持つ形:** 仕様どおり、その MMO を A・B（・D）で判定する。

これらの命令は `mayLoadOrStore()` が偽でもメモリアクセス命令として扱う。仕様書 2 節の定義、および 9.2 の 11 で CALL・RET が 0x83 として載ることを満たすためである。

## 6. MC 層で生成される命令の洗い出し

X86 AsmPrinter のうち、MachineInstr を通常の変換以外で出力する箇所の一覧。いずれも `handleMCInst` を経由する（`EmitAndCountInstruction`、`emitCallInstruction`、直接 `OutStreamer->emitInstruction` を呼ぶ 5 箇所）。

| 生成元 | 出す命令 | メモリへの触れ方 | 判定 | 確認 |
| --- | --- | --- | --- | --- |
| `TLS_addr64`、`TLS_base_addr64`（`LowerTlsAddr`） | `data16 lea`、`data16 data16 rex64 call __tls_get_addr`（GOT 経由なら `call *__tls_get_addr@GOTPCREL(%rip)`） | call（スタックと、GOT 経由なら GOT） | 0x07 | lit、手動 |
| `TLS_desc64`（`LowerTlsAddr`） | `lea x@tlsdesc(%rip)`、`call *x@tlscall(%rax)` | 記述子の読み取り（遅延解決されうる） | 0x09 | lit、手動 |
| `FENTRY_CALL`（`-pg -mfentry`） | `call __fentry__` | スタック | 0x07 | lit、手動 |
| `PATCHABLE_FUNCTION_ENTER`（XRay） | `jmp` と nop | なし | — | 手動 |
| `PATCHABLE_RET`（XRay） | `ret` と nop | スタック | 0x07 | 手動 |
| `PATCHABLE_TAIL_CALL`（XRay） | `jmp`（メモリ経由のこともある）と nop | メモリ経由の場合 | 0x07 | 未確認 |
| `PATCHABLE_EVENT_CALL`、`PATCHABLE_TYPED_EVENT_CALL`（XRay） | `push`、`mov`、`call`、`pop` | スタック | 0x07 | 未確認 |
| `KCFI_CHECK`（`-fsanitize=kcfi`） | `mov`、`add -4(%reg), %r10d`、`je`、`ud2` | 呼び出し先直前の型 ID の読み取り。`ud2` も `MCInstrDesc` 上はメモリアクセスなので載る | 0x07 | 手動 |
| `ASAN_CHECK_MEMACCESS` | `call` | スタック | 0x07 | 未確認 |
| `UBSAN_UD1` | `ud1`（メモリオペランドの形） | なし（トラップ）。`MCInstrDesc` 上は載る | 0x07 | 未確認 |
| `MORESTACK_RET`、`MORESTACK_RET_RESTORE_R10`（`-fsplit-stack`） | `ret`（後者は `mov %rax, %r10` も） | スタック | 0x07 | 手動 |
| `FAULTING_OP` | 実際のロード・ストア | 対象のメモリ | 0x07 | 未確認 |
| `STATEPOINT` | `call` | スタック | 0x07 | 未確認 |
| `PATCHPOINT`、`STACKMAP` | `mov`、`call *%r11`、nop | スタック | 0x07 | 未確認 |
| `PATCHABLE_OP` | nop、2 バイト `mov` | なし | — | 未確認 |
| `CALL64pcrel32` などの前の `cs` プレフィックス（`-mindirect-branch-cs-prefix`）、`JCC_1` の前の分岐ヒント | プレフィックス | — | 後続の命令のラベルを前に寄せる（7.3） | 手動 |
| `emitNop`、`emitX86Nops`、スタックマップのシャドウ | nop | なし | フックを通らない | — |
| `MOVPC32r`、`EH_RETURN`、`CLEANUPRET`、`CATCHRET`、SEH、Import Call Optimization | 32 ビットまたは Windows 専用 | — | 本機能では到達しない | — |
| ファイル末尾の ASan チェック関数、MachO のスタブ | 関数本体の外 | — | レコードなし（全部ロック必要、仕様 8.2） | — |

AsmPrinter が直接出す命令は、ほかに `-fpatchable-function-entry` の nop がある。これはメモリに触らない。

インラインアセンブリは命令を解析せず、範囲（kind = 1、0x05）で登録する。

- 範囲の前後には、`emitInlineAsmStart` と `emitInlineAsmEnd` でラベルを置く。
- 終わりのセクションが始まりと違う場合は、`reportError` でエラーにする。
- モジュールレベルのインラインアセンブリは、関数に属さないので登録しない。

## 7. セクション出力

### 7.1 セクション

`getMemLockMapSection` の作り方は、`MCObjectFileInfo::getBBAddrMapSection` と同じ。

- テキストセクションの開始シンボルと unique ID から、`SHT_PROGBITS`・`SHF_LINK_ORDER` の `.memlock_map` を作る。
- テキストセクションがグループに属するときは `SHF_GROUP` を付け、同じ COMDAT グループに入れる。
- `-S` では `.section .memlock_map,"o",@progbits,.text` や `"oG",@progbits,.text.inl,inl,comdat` の形になる。

### 7.2 レコード

- `AsmPrinter::MBBSectionRanges` の各要素を 1 つの関数フラグメントとし、レコードを 1 つずつ出す。エントリがなくても出す。
- エントリはフラグメントごとに分けて記録する。キーは MachineBasicBlock の `SectionID` で、MI のない MCInst は直前の MI のものを使う。
- 各フィールドの出し方:
  - `base_address`: `emitSymbolValue(Begin, 8)`（`R_X86_64_64`）
  - `fragment_size` とエントリの `offset`: `emitAbsoluteSymbolDiffAsULEB128`。アセンブラのレイアウト確定後に解決される。
  - 範囲の `length`: 範囲の終わりと始まりのラベル差

### 7.3 ラベルの位置

- ラベルは、MCInst を `OutStreamer->emitInstruction` に渡す直前に置く。コメント（`AddComment`）の後になるが、バイト列には影響しない。
- 独立した MCInst として出るプレフィックス（`X86II::isPrefix`）には、最初のものの前にラベルを置く。後続の命令がメモリアクセスなら、そのラベルを使う。これで `data16 data16 rex64 call` のオフセットが `data16` の先頭を指す。
- 一時シンボル（`.Lmemlock*`、`.Lmemlock_asm*`）はシンボル表に残らない。

## 8. 判断（解釈と選択）

**D1. ラベルを MachineInstr ではなく MCInst ごとに打つ**

- 判断: MCInst の出力直前に判定・ラベル付けする。通常の変換の MCInst だけ MachineInstr で判定し、ほかは 0x07 にする。
- 理由: 独自の展開（例: TLS_addr64 は lea の後に call を出す）では、メモリアクセスが先頭命令とは限らない。MachineInstr の直前に打つと、オフセットがずれたり列挙漏れが起きたりする（仕様 3.4、5.5）。
- 代替案:
  - 展開ごとに個別にラベル処理を書く案。漏れやすいので不採用。
  - MCStreamer を差し替える案。インラインアセンブリの命令まで拾ってしまい、文脈も分からないので不採用。

**D2. IR の値も PseudoSourceValue もない MMO は 0x02**

- 理由: 仕様 4.2 は「MMO がない」を 0x01、「基底オブジェクトが不明」を 0x02 としている。MMO はあるが値がない場合は後者と解釈した。

**D3. 明示的メモリオペランドのない命令は、読み・書きそれぞれの MMO を要求する**

- 理由: 文字列命令（`rep movs` など）は読みと書きの二つのアクセスを持つ。片方の MMO しかなければ「MMO が足りない」（0x01）にあたる。
- 明示的メモリオペランドを持つ命令（`PUSH64rmm`、`CALL64m`）は、仕様 4.1 C に従い、MMO が 1 つあればそれで判定する。暗黙のスタックアクセス側は問わない。

**D4. `getUnderlyingObjects` が探索を打ち切った場合は 0x09**

- 理由: 上限に達すると、たどり切れなかった値（GEP など）をそのまま結果に入れる。仕様 4.2 の「探索上限到達」は 0x09 なので、0x02 と区別した。

**D5. 入力が LLVM IR の場合はエラーにしない**

- 理由: `-save-temps` は C → IR → 機械語の順にコンパイルするため、IR を受け付けないと C でも使えなくなる。addrmap と同じ扱い。
- 影響: C++ から作った `.ll` を直接渡すと通ってしまう（11 章）。

**D6. ターゲット条件は「x86-64 かつ Linux かつ ELF」、x32 は除く**

- 理由: 仕様 1 節の前提が x86-64・Linux・ELF。x32（`gnux32`）は ILP32 で、仕様の想定外とした。
- エラーの場所: clang は `CompilerInvocation`（cc1 で判定するので、ドライバと cc1 の両方を覆う）、llc は全バックエンド共通の `AsmPrinter::doInitialization`。

**D7. セクションシンボルの追加を許し、9.3-1 の比較方法を改訂した（仕様書を改訂済み）**

- 事象: `base_address` の再配置は一時ラベル基準なので、アセンブラは対象テキストセクションのセクションシンボルを使う。そのセクションをほかに参照する再配置がない場合（retpoline のサンク、`-fno-asynchronous-unwind-tables` など）、セクションシンボルが増えてシンボル番号がずれる。clang は既定で `-faddrsig` を使うので、`.llvm_addrsig` のバイト列も変わる。
- 判断: ユーザーと相談のうえ、仕様書 5.6 と 9.3-1 を次のように改訂した。
  - セクションシンボルの追加を許す。
  - シンボル番号を含むセクション（`.symtab`、再配置、`.group`、`.llvm_addrsig`）は、シンボル名で比較する。
- 代替案:
  - weak でない関数だけ関数シンボル基準の再配置にする案。weak・COMDAT 関数では回避できず、MC 層（ELFObjectWriter）の変更も必要なので不採用。
  - 起きる構成をエラーにする案。網羅が難しいので不採用。

**D8. 関数先頭ラベルを必ず作る**

- `CurrentFnBegin` は、通常はデバッグ情報や例外処理がある場合しか作られない。`MemLockMap != None` のときも作るように条件を足した。一時シンボルなので出力のバイト列は変わらない。

**D9. アセンブラが分岐の前に NOP を入れうる関数は、レコードを出さない（ユーザーの指示）**

- 事象: `-mbranches-within-32B-boundaries` や `-x86-align-branch*` を使うと、X86 のアセンブラは整列対象の命令の直前に NOP を入れる（`X86AsmBackend::emitInstructionBegin` の `MCBoundaryAlignFragment`）。対象は分岐・call・ret、およびマクロ融合ペアの先頭命令。ラベルは AsmPrinter が命令を渡す前に置くので NOP の前に来てしまい、エントリが NOP を指して本当の命令が漏れる。
- 判断: 仕様 5.4 はこの場合エラーとしていたが、ユーザーの指示でエラーにはしない。そのようなメモリアクセス命令を含む関数は、レコードを丸ごと出さない。レコードのない範囲は利用側がすべてロック必要として扱う（仕様 8.2）ので、安全側に倒れる。
- 実装:
  - 新設した `X86_MC::getAlignedBranchKinds()` で、コマンドラインオプションから整列対象の種類を求める。中身は X86AsmBackend のコンストラクタと同じ計算。
  - メモリアクセスの MCInst ごとに `X86MemLockMap::mayBePadded` で判定する。融合の判定は、アセンブラと共用にした `X86_MC::isFirstMacroFusibleInst` を使う。該当すれば、関数のレコードを落とす。
  - 判定はモードに関係なく、エントリにならないロック不要の命令も含めて行う。そのため、通常モードと all モードで同じ関数が落ちる（9.3-4）。
  - オプションの値で決めるので、`-S` でも同じ判定になる。
- 対象外: プレフィックスによるパディングは命令自身の先頭に付くので、ラベルはずれない。分岐が条件分岐（jcc）だけでメモリに触らない場合も、ずれるエントリがないので落とさない。
- 精度: 部分的にパディングを止める区間（`NoAutoPaddingScope`、TLS の呼び出し列など）は考慮せず、安全側に倒している。

## 9. 参考にした既存実装

| 既存実装 | 参考にした点 |
| --- | --- |
| `SHT_LLVM_BB_ADDR_MAP`（`-fbasic-block-address-map`、`AsmPrinter::emitBBAddrMapSection`、`MCObjectFileInfo::getBBAddrMapSection`） | テキストセクションごとのリンク順序セクション、COMDAT、ラベル差の ULEB128（`emitAbsoluteSymbolDiffAsULEB128`）、`MBBSectionRanges` による関数フラグメント |
| `.stack_sizes`（`-fstack-size-section`、`getStackSizesSection`） | `SHF_LINK_ORDER` セクションの作り方、関数先頭ラベルの生成条件 |
| `CaptureTracking`（`PointerMayBeCaptured`）、`ValueTracking`（`getUnderlyingObjects`） | 規則 A の判定 |
| addrmap（`X86AddrMap`、本ブランチの先行実装） | AsmPrinter のフックの置き方、オプションの通し方、C 以外の言語のエラー |

## 10. テストと検証

lit テスト:

| テスト | 内容 |
| --- | --- |
| `llvm/test/CodeGen/X86/memlock-map.ll` | 規則 A・C・D と 0x02・0x03、インラインアセンブリの範囲、エントリなしの関数、all・lock・none の各モード、COMDAT・リンク順序・再配置、生成コード不変、対象外ターゲットのエラー |
| `llvm/test/CodeGen/X86/memlock-map-padding.ll` | 分岐の整列（call、ret、融合ペア、jcc）を有効にしたとき、対象の関数だけレコードが落ちること。RIP 相対の cmp は融合しないので落ちないこと |
| `llvm/test/CodeGen/X86/memlock-map-mc.ll` | TLS の `__tls_get_addr` 呼び出し（0x07、オフセットがプレフィックス込み）、TLSDESC（0x09）、`__fentry__`（0x07）、`-S` 経由のアセンブル結果の一致 |
| `clang/test/Driver/fmemlock-map.c` | ドライバからの転送、不正な値、ターゲット・言語のエラー |
| `clang/test/CodeGen/memlock-map-section.c` | cc1 での出力 |

検証スクリプト（`verify_memlock_map.py`）による組み合わせ検証は、216 通りすべて通った。

- 入力: テスト用の C ファイル 3 つ。9.2 の 1〜13、18〜20、22、23 のケース、TLS、間接呼び出しを含む。
- 最適化レベル: -O0、-O1、-O2、-O3、-Os、-Oz
- -g: あり・なし
- オプション群: 指定なし / `-fPIC -fno-plt -mtls-dialect=gnu2` / `-ffunction-sections -fno-asynchronous-unwind-tables` / `-fPIC` と XRay / `-fsanitize=kcfi -pg -mfentry -fstack-protector-all` / `-mretpoline -mindirect-branch-cs-prefix -fsplit-stack`

検査内容は 9.3 の 1〜4 で、それぞれ次のとおり。

- 9.3-1: 改訂後の比較方法で、生成コードが変わらないこと。
- 9.3-2: オフセットが `llvm-objdump` 上の命令の先頭に一致し、昇順で重複がないこと。
- 9.3-3: Capstone で逆アセンブルした命令のうち、メモリに触るものがすべて載っていること。対象は、メモリオペランドを持つもの（`lea`・nop を除く）、push・pop・call・ret・leave・enter、文字列命令。
- 9.3-4: 通常モードのエントリが、all モードのロック必要エントリと一致すること。

`llvm/test/CodeGen/X86` の既存テスト（5379 件）は、改造前と同じくすべて通る。

9.2 の必須テストケースの状況:

| # | 状況 |
| --- | --- |
| 1、2、3、4、5、6、7、8、9、10、11、13、22 | 期待どおり（手動確認と組み合わせ検証） |
| 12 | 期待どおり（ロック必要）。メモリ経由の末尾呼び出しは MMO がなく 0x01 |
| 18 | 期待どおり。GOT の読み取りは 0x84、その先は 0x02 |
| 19 | 末尾呼び出しにならない `call *foo@GOTPCREL(%rip)` は 0x84。末尾呼び出し（`jmp *...`）は MMO がなく 0x01 |
| 20 | 期待どおり（0x09） |
| 21 | 期待どおり（エラー） |
| 23 | ジャンプテーブルの読み取りは 0x84。switch によっては定数表（グローバル）に変換され、0x02 になる |
| 14、15 | 未確認（リンク後の `--gc-sections`、PIE、共有ライブラリ） |
| 16 | パディングが入りうる関数はレコードを出さない（判断 D9）。call・ret まで整列させて実際に NOP が入る入力で、検証スクリプトが通ることを確認 |
| 17 | 上の組み合わせ検証で確認 |

## 11. 既知の制限と未実装

**未実装（仕様書の要求）**

- **`-flto` のエラー:** 今は黙ってセクションが出ない。`-flto` では `clang -c` がビットコードを出し、リンク時のコード生成に `-fmemlock-map` が渡らないため。レコードがないので全部ロック必要として扱われ、健全性は保たれるが、仕様 7 節はエラーを求めている。
- **`-fno-integrated-as` のエラー。**
- **リンク後の確認（9.2 の 14、15）の lit テスト。**
- **受け入れ用 C プログラム（9.4）。**

**制限（健全性は保たれるもの）**

- **分岐の整列を有効にした場合:** 該当する関数のレコードがなくなり、関数全体でロックを取る（判断 D9）。規定の `-mbranches-within-32B-boundaries` では、メモリオペランドを持つ融合可能な比較（`cmp mem, reg` など）やメモリ経由のジャンプを含む関数が対象になる。なお、このオプションでは本機能がなくても、`-S` 経由でアセンブルした `.text` が直接出力と一致しない（LLVM 既存の挙動）。

- **出力 0 バイトのインラインアセンブリ:** ディレクティブだけのものは長さ 0 の範囲になり、直後の命令とオフセットが重なる。仕様 6.2 の「同一オフセットの重複なし」に反する。空文字列のインラインアセンブリは AsmPrinter が出力しないので、問題にならない。
- **`volatile` 付きのローカル変数:** CaptureTracking が volatile アクセスを捕獲として扱うので 0x03 になる。
- **メモリ経由の末尾呼び出し:** `TAILJMPm64` など、MMO が失われた命令は 0x01 になる。
- **`MCInstrDesc` で過大に判定される命令:** `ud2`、`ud1`、`pause`、`lfence` などはメモリアクセスとして載る（0x07 または 0x01）。
- **C++ などから作った LLVM IR を直接入力した場合:** エラーにならない（判断 D5）。
- **MachineOutliner:** X86 では既定で無効。`MF.isOutlined()` の扱いは実装したが、テストしていない。
- **基本ブロックセクション・関数分割（`-fbasic-block-sections`、`-fsplit-machine-functions`）:** 関数フラグメントが複数になる場合の実装（`MBBSectionRanges` ごとのレコード）はしたが、テストしていない。
