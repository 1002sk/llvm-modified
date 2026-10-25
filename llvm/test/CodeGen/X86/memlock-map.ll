; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -asm-verbose=false \
; RUN:   -memlock-map=all | FileCheck %s --check-prefixes=CHECK,ALL
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -asm-verbose=false \
; RUN:   -memlock-map=lock | FileCheck %s --check-prefixes=CHECK,LOCK
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu | FileCheck %s --check-prefix=NOMAP
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -memlock-map=none \
; RUN:   | FileCheck %s --check-prefix=NOMAP
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -memlock-map=all \
; RUN:   -function-sections -filetype=obj -o %t.o
; RUN: llvm-readelf -S -g %t.o | FileCheck %s --check-prefix=OBJ
; RUN: llvm-readelf -r %t.o | FileCheck %s --check-prefix=RELA

;; The map does not change the code.
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -filetype=obj -o %t.none.o
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -memlock-map=all \
; RUN:   -filetype=obj -o %t.all.o
; RUN: llvm-objcopy -O binary --only-section=.text %t.none.o %t.none.bin
; RUN: llvm-objcopy -O binary --only-section=.text %t.all.o %t.all.bin
; RUN: cmp %t.none.bin %t.all.bin

;; The map is only supported for x86-64 Linux ELF.
; RUN: not llc < %s -mtriple=i686-unknown-linux-gnu -memlock-map=all \
; RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=TARGET -DTRIPLE=i686-unknown-linux-gnu
; RUN: not llc < %s -mtriple=x86_64-unknown-linux-gnux32 -memlock-map=all \
; RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=TARGET -DTRIPLE=x86_64-unknown-linux-gnux32
; RUN: not llc < %s -mtriple=x86_64-unknown-freebsd -memlock-map=all \
; RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=TARGET -DTRIPLE=x86_64-unknown-freebsd
; RUN: not llc < %s -mtriple=x86_64-pc-windows-msvc -memlock-map=all \
; RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=TARGET -DTRIPLE=x86_64-pc-windows-msvc

; TARGET: error: -memlock-map is not supported for target '[[TRIPLE]]'

; NOMAP-NOT: memlock

@g = dso_local global i32 0

declare void @sink(ptr)

;; Rule A: a local variable whose address does not escape (0x81). Rule C: RET
;; (0x83). Neither needs a lock.
define i32 @local(i32 %x, i64 %i, i64 %j) nounwind {
; CHECK-LABEL: local:
; CHECK-NEXT:  .Lfunc_begin0:
; ALL-NEXT:    .Lmemlock0:
; CHECK-NEXT:    movl %edi, -16(%rsp,%rsi,4)
; ALL-NEXT:    .Lmemlock1:
; CHECK-NEXT:    movl -16(%rsp,%rdx,4), %eax
; ALL-NEXT:    .Lmemlock2:
; CHECK-NEXT:    retq
; CHECK-NEXT:  .Lfunc_end0:
; CHECK-NEXT:    .size local, .Lfunc_end0-local
; CHECK-EMPTY:
; CHECK-NEXT:    .section .memlock_map,"o",@progbits,.text
; CHECK-NEXT:    .byte 1
; ALL-NEXT:      .byte 1
; LOCK-NEXT:     .byte 0
; CHECK-NEXT:    .quad .Lfunc_begin0
; CHECK-NEXT:    .uleb128 .Lfunc_end0-.Lfunc_begin0
; CHECK-NEXT:    .byte 5
; CHECK-NEXT:    .ascii "local"
; ALL-NEXT:      .byte 3
; ALL-NEXT:      .uleb128 .Lmemlock0-.Lfunc_begin0
; ALL-NEXT:      .byte 0
; ALL-NEXT:      .byte 129
; ALL-NEXT:      .uleb128 .Lmemlock1-.Lfunc_begin0
; ALL-NEXT:      .byte 0
; ALL-NEXT:      .byte 129
; ALL-NEXT:      .uleb128 .Lmemlock2-.Lfunc_begin0
; ALL-NEXT:      .byte 0
; ALL-NEXT:      .byte 131
; LOCK-NEXT:     .byte 0
; CHECK-NEXT:    .text
  %a = alloca [4 x i32]
  %p = getelementptr i32, ptr %a, i64 %i
  store i32 %x, ptr %p
  %q = getelementptr i32, ptr %a, i64 %j
  %v = load i32, ptr %q
  ret i32 %v
}

;; A local variable passed to a call is captured (0x03). PUSH, CALL and POP
;; are rule C.
define i32 @captured(i32 %x) nounwind {
; CHECK-LABEL: captured:
; CHECK-NEXT:  .Lfunc_begin1:
; ALL-NEXT:    [[PUSH:.Lmemlock[0-9]+]]:
; CHECK-NEXT:    pushq %rax
; CHECK-NEXT:  [[STORE:.Lmemlock[0-9]+]]:
; CHECK-NEXT:    movl %edi, 4(%rsp)
; CHECK-NEXT:    leaq 4(%rsp), %rdi
; ALL-NEXT:    [[CALL:.Lmemlock[0-9]+]]:
; CHECK-NEXT:    callq sink@PLT
; CHECK-NEXT:  [[LOAD:.Lmemlock[0-9]+]]:
; CHECK-NEXT:    movl 4(%rsp), %eax
; ALL-NEXT:    [[POP:.Lmemlock[0-9]+]]:
; CHECK-NEXT:    popq %rcx
; ALL-NEXT:    [[RET:.Lmemlock[0-9]+]]:
; CHECK-NEXT:    retq
; CHECK:         .ascii "captured"
; ALL-NEXT:      .byte 6
; LOCK-NEXT:     .byte 2
; ALL-NEXT:      .uleb128 [[PUSH]]-.Lfunc_begin1
; ALL-NEXT:      .byte 0
; ALL-NEXT:      .byte 131
; CHECK-NEXT:    .uleb128 [[STORE]]-.Lfunc_begin1
; CHECK-NEXT:    .byte 0
; CHECK-NEXT:    .byte 3
; ALL-NEXT:      .uleb128 [[CALL]]-.Lfunc_begin1
; ALL-NEXT:      .byte 0
; ALL-NEXT:      .byte 131
; CHECK-NEXT:    .uleb128 [[LOAD]]-.Lfunc_begin1
; CHECK-NEXT:    .byte 0
; CHECK-NEXT:    .byte 3
; ALL-NEXT:      .uleb128 [[POP]]-.Lfunc_begin1
; ALL-NEXT:      .byte 0
; ALL-NEXT:      .byte 131
; ALL-NEXT:      .uleb128 [[RET]]-.Lfunc_begin1
; ALL-NEXT:      .byte 0
; ALL-NEXT:      .byte 131
; CHECK-NEXT:    .text
  %a = alloca i32
  store i32 %x, ptr %a
  call void @sink(ptr %a)
  %v = load i32, ptr %a
  ret i32 %v
}

;; A constant pool read is rule D (0x84). A global variable needs a lock
;; (0x02).
define double @global(double %x) nounwind {
; CHECK-LABEL: global:
; CHECK-NEXT:  .Lfunc_begin2:
; ALL-NEXT:    [[CP:.Lmemlock[0-9]+]]:
; CHECK-NEXT:    mulsd .LCPI2_0(%rip), %xmm0
; CHECK-NEXT:  [[G:.Lmemlock[0-9]+]]:
; CHECK-NEXT:    incl g(%rip)
; CHECK:         .ascii "global"
; ALL-NEXT:      .byte 3
; LOCK-NEXT:     .byte 1
; ALL-NEXT:      .uleb128 [[CP]]-.Lfunc_begin2
; ALL-NEXT:      .byte 0
; ALL-NEXT:      .byte 132
; CHECK-NEXT:    .uleb128 [[G]]-.Lfunc_begin2
; CHECK-NEXT:    .byte 0
; CHECK-NEXT:    .byte 2
  %m = fmul double %x, 3.14
  %v = load i32, ptr @g
  %i = add i32 %v, 1
  store i32 %i, ptr @g
  ret double %m
}

;; Inline assembly is a range (0x05), whether or not it accesses memory.
define i32 @asm(i32 %x) nounwind {
; CHECK-LABEL: asm:
; CHECK-NEXT:  .Lfunc_begin3:
; CHECK-NEXT:    #APP
; CHECK-NEXT:  [[BEGIN:.Lmemlock_asm[0-9]+]]:
; CHECK-NEXT:    movl %edi, %eax
; CHECK-NEXT:  [[END:.Lmemlock_asm_end[0-9]+]]:
; CHECK-NEXT:    #NO_APP
; CHECK:         .ascii "asm"
; ALL-NEXT:      .byte 2
; LOCK-NEXT:     .byte 1
; CHECK-NEXT:    .uleb128 [[BEGIN]]-.Lfunc_begin3
; CHECK-NEXT:    .byte 1
; CHECK-NEXT:    .uleb128 [[END]]-[[BEGIN]]
; CHECK-NEXT:    .byte 5
  %r = call i32 asm "movl $1, $0", "=r,r"(i32 %x)
  ret i32 %r
}

;; A function without memory accesses still has a record.
define i32 @nomem(i32 %x) nounwind {
; CHECK-LABEL: nomem:
; CHECK:         .ascii "nomem"
; ALL-NEXT:      .byte 1
; LOCK-NEXT:     .byte 0
  %r = mul i32 %x, 3
  ret i32 %r
}

;; One section per text section, linked to it, in the COMDAT group of a
;; COMDAT function.
$inl = comdat any

define linkonce_odr i32 @inl() nounwind comdat {
; CHECK-LABEL: inl:
; CHECK:         .section .memlock_map,"oG",@progbits,.text.inl,inl,comdat
  ret i32 1
}

; OBJ:      [{{ *}}[[#%d,LOCAL:]]] .text.local       PROGBITS
; OBJ-NEXT: [{{ *}}[[#%d,MAP:]]] .memlock_map      PROGBITS {{[0-9a-f]+}} {{[0-9a-f]+}} {{[0-9a-f]+}} 00 L [[#LOCAL]] 0 1
; OBJ-NEXT: [{{ *}}[[#MAP+1]]] .rela.memlock_map RELA     {{.*}} [[#MAP]]  8
; OBJ:      [{{ *}}[[#%d,INL:]]] .text.inl         PROGBITS {{.*}} AXG
; OBJ-NEXT: [{{ *}}[[#%d,INLMAP:]]] .memlock_map      PROGBITS {{[0-9a-f]+}} {{[0-9a-f]+}} {{[0-9a-f]+}} 00 LG [[#INL]] 0 1
; OBJ:      COMDAT group section [{{.*}}] `.group' [inl] contains 3 sections:
; OBJ-NEXT:   [Index]    Name
; OBJ-NEXT:   [{{ *}}[[#INL]]]   .text.inl
; OBJ-NEXT:   [{{ *}}[[#INLMAP]]]   .memlock_map
; OBJ-NEXT:   [{{ *}}[[#INLMAP+1]]]   .rela.memlock_map
; RELA:      Relocation section '.rela.memlock_map'
; RELA-NEXT:   Offset
; RELA-NEXT:   0000000000000002 {{.*}} R_X86_64_64 {{.*}} .text.local + 0
