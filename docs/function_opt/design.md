# function_opt 実装メモ
(ここは手書きです...)

関数単位で最適化を効かせる目的でLLVMを改変しています。現在制作途中なので随時更新されます/テスト用の出力があります

```
clang SOURCE_CODE.c -c -emit-llvm -o IR_NO_OPT.bc
opt IR_NO_OPT.bc -o IR_OPT.bc -passes='default<O2>,function-opt<"...">'
(or clang IR_NO_OPT.bc --mllvm -funcion-opt-pass-option="..." -o IR_OPT.bc)
clang IR_OPT -o a.out
```

効果を確かめたい時は`-O0 -Xclang -disable-O0-optnone`を使うとO0に指定した最適化のみを有効にできる

https://compilergym.com/llvm/index.html に記載されているパスのうち、function, loop-nest, loopのものに対応。

パスの内容はpass_data.csvに記載、表計算とかのソフトで見てください。LLMを使って情報をまとめているため、内容に誤りがある可能性があります。

## 実装方法
llvm/lib/Transforms/FunctionOpt にパスの形で実装
内部でFunctionPassManagerを生成して呼び出している
