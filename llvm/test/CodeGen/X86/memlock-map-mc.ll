;; Instructions emitted by custom lowering in the AsmPrinter, without a
;; MachineInstr of their own, need a lock (0x07). The TLSDESC call reads a
;; descriptor that may be resolved lazily (0x09).
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -relocation-model=pic \
; RUN:   -asm-verbose=false -memlock-map=lock | FileCheck %s --check-prefixes=CHECK,GD
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -relocation-model=pic \
; RUN:   -enable-tlsdesc -asm-verbose=false -memlock-map=lock \
; RUN:   | FileCheck %s --check-prefixes=CHECK,DESC

;; The offset includes the prefixes emitted as separate MCInsts: the call to
;; __tls_get_addr starts at 9 (pushq is 1 byte, data16 leaq is 8 bytes).
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -relocation-model=pic \
; RUN:   -memlock-map=lock -function-sections -filetype=obj -o %t.o
; RUN: llvm-readelf -x .memlock_map %t.o | FileCheck %s --check-prefix=HEX

;; Assembling the -S output gives the same bytes.
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -relocation-model=pic \
; RUN:   -memlock-map=lock -function-sections -o %t.s
; RUN: llvm-mc -triple=x86_64-unknown-linux-gnu -filetype=obj %t.s -o %t.s.o
; RUN: llvm-readelf -x .memlock_map -x .text.tls -x .text.fentry %t.o > %t.o.txt
; RUN: llvm-readelf -x .memlock_map -x .text.tls -x .text.fentry %t.s.o > %t.s.o.txt
; RUN: diff %t.o.txt %t.s.o.txt

@etg = external thread_local global i32

define i32 @tls() nounwind {
; CHECK-LABEL: tls:
; CHECK-NEXT:  .Lfunc_begin0:
; CHECK-NEXT:    pushq %rax
; GD-NEXT:     .Lmemlock0:
; GD-NEXT:       data16
; GD-NEXT:       leaq etg@TLSGD(%rip), %rdi
; GD-NEXT:     [[CALL:.Lmemlock1]]:
; GD-NEXT:       data16
; GD-NEXT:       data16
; GD-NEXT:       rex64
; GD-NEXT:       callq __tls_get_addr@PLT
; DESC-NEXT:     leaq etg@tlsdesc(%rip), %rax
; DESC-NEXT:   [[CALL:.Lmemlock0]]:
; DESC-NEXT:     callq *etg@tlscall(%rax)
; CHECK-NEXT:  [[LOAD:.Lmemlock[0-9]+]]:
; CHECK-NEXT:    movl
; CHECK:         .ascii "tls"
; CHECK-NEXT:    .byte 2
; CHECK-NEXT:    .uleb128 [[CALL]]-.Lfunc_begin0
; CHECK-NEXT:    .byte 0
; GD-NEXT:       .byte 7
; DESC-NEXT:     .byte 9
; CHECK-NEXT:    .uleb128 [[LOAD]]-.Lfunc_begin0
; CHECK-NEXT:    .byte 0
; CHECK-NEXT:    .byte 2
  %v = load i32, ptr @etg
  ret i32 %v
}

define void @fentry() nounwind "fentry-call"="true" {
; CHECK-LABEL: fentry:
; CHECK-NEXT:  .Lfunc_begin1:
; CHECK-NEXT:  [[FENTRY:.Lmemlock[0-9]+]]:
; CHECK-NEXT:    callq __fentry__
; CHECK:         .ascii "fentry"
; CHECK-NEXT:    .byte 1
; CHECK-NEXT:    .uleb128 [[FENTRY]]-.Lfunc_begin1
; CHECK-NEXT:    .byte 0
; CHECK-NEXT:    .byte 7
  ret void
}

; HEX:      Hex dump of section '.memlock_map':
; HEX-NEXT: 0x00000000 01000000 00000000 00001503 746c7302 ............tls.
; HEX-NEXT: 0x00000010 09000711 0002                       ......
; HEX:      Hex dump of section '.memlock_map':
; HEX-NEXT: 0x00000000 01000000 00000000 00000606 66656e74 ............fent
; HEX-NEXT: 0x00000010 72790100 0007                       ry....
