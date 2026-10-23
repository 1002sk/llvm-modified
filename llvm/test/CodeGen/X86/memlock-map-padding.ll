;; When the assembler aligns branches with nops, the nops go between the label
;; and the instruction. A function with a memory access that may be padded
;; gets no record, so that a lock is taken for all of it. The decision does not
;; depend on the mode.
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -asm-verbose=false \
; RUN:   -memlock-map=all -x86-align-branch-boundary=32 -x86-align-branch=call \
; RUN:   | FileCheck %s --check-prefix=CALL
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -asm-verbose=false \
; RUN:   -memlock-map=lock -x86-align-branch-boundary=32 -x86-align-branch=call \
; RUN:   | FileCheck %s --check-prefix=CALL
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -asm-verbose=false \
; RUN:   -memlock-map=all -x86-align-branch-boundary=32 -x86-align-branch=ret \
; RUN:   | FileCheck %s --check-prefix=RET
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -asm-verbose=false \
; RUN:   -memlock-map=all -x86-branches-within-32B-boundaries \
; RUN:   | FileCheck %s --check-prefix=FUSED
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -asm-verbose=false \
; RUN:   -memlock-map=all -x86-align-branch-boundary=32 -x86-align-branch=jcc \
; RUN:   | FileCheck %s --check-prefix=JCC

; CALL:      .ascii "nocall"
; CALL-NOT:  .ascii "withcall"
; CALL:      .ascii "fused"
; CALL:      .ascii "rip"

; RET-NOT:   .ascii

;; The cmp in @fused may be fused with the jcc and padded. The RIP-relative
;; one in @rip is not fusible, and the jcc does not access memory.
; FUSED:     .ascii "nocall"
; FUSED:     .ascii "withcall"
; FUSED-NOT: .ascii "fused"
; FUSED:     .ascii "rip"

;; Padding in front of a jcc, which does not access memory, does not matter.
; JCC:       .ascii "nocall"
; JCC:       .ascii "withcall"
; JCC:       .ascii "fused"
; JCC:       .ascii "rip"

@g = dso_local global i32 0
declare void @ext()

define i32 @nocall(i32 %x) nounwind {
  %v = load i32, ptr @g
  %r = add i32 %v, %x
  ret i32 %r
}

define void @withcall() nounwind {
  call void @ext()
  store i32 1, ptr @g
  ret void
}

define i32 @fused(ptr %p, i32 %x) nounwind {
  %v = load i32, ptr %p
  %c = icmp eq i32 %v, %x
  br i1 %c, label %t, label %f
t:
  ret i32 1
f:
  ret i32 2
}

define i32 @rip(i32 %x) nounwind {
  %v = load i32, ptr @g
  %c = icmp eq i32 %v, %x
  br i1 %c, label %t, label %f
t:
  ret i32 1
f:
  ret i32 2
}
