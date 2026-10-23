// REQUIRES: x86-registered-target
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -O1 -fmemlock-map=all -S %s -o - | FileCheck %s --check-prefixes=CHECK,ALL
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -O1 -fmemlock-map=lock -S %s -o - | FileCheck %s --check-prefixes=CHECK,LOCK
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -O1 -S %s -o - | FileCheck %s --check-prefix=NOMAP

// NOMAP-NOT: {{\.memlock_map|\.Lmemlock}}

int g;

// The address of g is read from the GOT (rule D), g itself needs a lock.

// CHECK-LABEL: func:
// CHECK:         .section .memlock_map,"o",@progbits,.text
// CHECK-NEXT:    .byte 1
// ALL-NEXT:      .byte 1
// LOCK-NEXT:     .byte 0
// CHECK-NEXT:    .quad .Lfunc_begin0
// CHECK-NEXT:    .uleb128 .Lfunc_end0-.Lfunc_begin0
// CHECK-NEXT:    .byte 4
// CHECK-NEXT:    .ascii "func"
// ALL-NEXT:      .byte 3
// ALL-NEXT:      .uleb128 .Lmemlock0-.Lfunc_begin0
// ALL-NEXT:      .byte 0
// ALL-NEXT:      .byte 132
// ALL-NEXT:      .uleb128 .Lmemlock1-.Lfunc_begin0
// LOCK-NEXT:     .byte 1
// LOCK-NEXT:     .uleb128 .Lmemlock0-.Lfunc_begin0
// CHECK-NEXT:    .byte 0
// CHECK-NEXT:    .byte 2
int func(void) { return g; }
