; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -relocation-model=pic -addrmap \
; RUN:   | FileCheck %s --check-prefix=GD
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -relocation-model=static -addrmap \
; RUN:   | FileCheck %s --check-prefix=LE

;; The general dynamic sequence comes from a pseudo instruction. The label has
;; to be placed before the data16 prefix, which is part of the LEA.

@tv = thread_local global [4 x i32] zeroinitializer
@tl = dso_local thread_local global [4 x i32] zeroinitializer

define ptr @tls() {
  ret ptr getelementptr inbounds (i8, ptr @tv, i64 8)
}

define ptr @tls_local() {
  ret ptr getelementptr inbounds (i8, ptr @tl, i64 8)
}

; GD-LABEL: tls:
; GD:         [[L:\.Laddrmap[0-9]+]]:
; GD-NEXT:    data16
; GD-NEXT:    leaq tv@TLSGD(%rip), %rdi
; GD:         .quad [[L]]
; GD:         \"kind\":\"addr\",\"insn\":{\"mnemonic\":\"leaq\",\"dest_reg\":\"rdi\",\"addr\":{\"segment\":null,\"base_reg\":\"rip\",\"index_reg\":null,\"scale\":1,\"disp\":0,\"symbol\":\"tv\",\"rip_relative\":true}}
; GD-SAME:    \"base\":{\"kind\":\"tls\",\"name\":\"tv\",\"type\":null,\"type_size\":16,\"object\":{\"size\":16},\"decl\":null}

;; Local exec: the base register holds the thread pointer, not an offset.
; LE-LABEL: tls_local:
; LE:         movq %fs:0, %rax
; LE-NEXT:    [[L:\.Laddrmap[0-9]+]]:
; LE-NEXT:    leaq tl@TPOFF+8(%rax), %rax
; LE:         .quad [[L]]
; LE:         \"function\":\"tls_local\",\"kind\":\"addr\"
; LE-SAME:    \"base\":{\"kind\":\"tls\",\"name\":\"tl\",
; LE-SAME:    \"ptr_offset\":{\"const\":8,\"var\":[]}
