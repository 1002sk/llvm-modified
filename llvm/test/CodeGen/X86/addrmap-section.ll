; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -relocation-model=pic -addrmap \
; RUN:   | FileCheck %s --check-prefixes=CHECK,PIC
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -relocation-model=static -addrmap \
; RUN:   | FileCheck %s --check-prefixes=CHECK,STATIC
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -relocation-model=pic \
; RUN:   | FileCheck %s --check-prefix=NOMAP
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -relocation-model=pic -addrmap \
; RUN:   -filetype=obj -o %t.o
; RUN: llvm-readelf -S -r %t.o | FileCheck %s --check-prefix=OBJ
; RUN: llvm-readelf -x .llvm_addrmap %t.o | FileCheck %s --check-prefix=HEX

;; The addrmap is only emitted for x86-64 ELF.
; RUN: llc < %s -mtriple=i686-unknown-linux-gnu -addrmap | FileCheck %s --check-prefix=NOMAP

source_filename = "addrmap.c"

@g = dso_local global [4 x i32] zeroinitializer
@ext = external global i32

declare void @sink(ptr)

define ptr @addr_global() {
  ret ptr getelementptr inbounds (i8, ptr @g, i64 8)
}

define ptr @addr_got() {
  ret ptr @ext
}

define ptr @derive(ptr %p) {
  %q = getelementptr inbounds i8, ptr %p, i64 16
  ret ptr %q
}

define i64 @mul3(i64 %x) {
  %y = mul i64 %x, 3
  ret i64 %y
}

define void @stack() {
  %a = alloca [2 x i32]
  %b = getelementptr inbounds i8, ptr %a, i64 4
  call void @sink(ptr %b)
  ret void
}

;; Every target instruction gets a label right before it. Instructions that
;; only use an address (the call) or manipulate the frame (push/pop) do not.
; CHECK-LABEL: addr_global:
; CHECK:         [[L0:\.Laddrmap[0-9]+]]:
; PIC-NEXT:      leaq .Lg$local+8(%rip), %rax
; STATIC-NEXT:   movl $g+8, %eax
; CHECK-LABEL: addr_got:
; CHECK:         [[L1:\.Laddrmap[0-9]+]]:
; CHECK-NEXT:    movq ext@GOTPCREL(%rip), %rax
; CHECK-LABEL: derive:
; CHECK:         [[L2:\.Laddrmap[0-9]+]]:
; CHECK-NEXT:    leaq 16(%rdi), %rax
; CHECK-LABEL: mul3:
; CHECK:         [[L3:\.Laddrmap[0-9]+]]:
; CHECK-NEXT:    leaq (%rdi,%rdi,2), %rax
; CHECK-LABEL: stack:
; CHECK:         pushq %rax
; CHECK-NOT:     Laddrmap
; CHECK:         [[L4:\.Laddrmap[0-9]+]]:
; CHECK-NEXT:    leaq 4(%rsp), %rdi
; CHECK-NEXT:    callq sink@PLT
; CHECK-NEXT:    popq %rax

; CHECK:       .section .llvm_addrmap,"",@progbits
; CHECK-NEXT:  .p2align 3
; CHECK-NEXT:  .ascii "AMAP"
; CHECK-NEXT:  .short 1
; CHECK-NEXT:  .short 48
; CHECK-NEXT:  .long [[#]]
; CHECK-NEXT:  .long 5
; CHECK-NEXT:  .long 128
; CHECK-NEXT:  .long [[#]]
; CHECK-NEXT:  .quad 0
; CHECK-NEXT:  .ascii
; CHECK-NEXT:  .quad [[L0]]
; CHECK-NEXT:  .long 0
; CHECK-NEXT:  .long 0
; CHECK-NEXT:  .quad [[L1]]
; CHECK-NEXT:  .long 1
; CHECK-NEXT:  .long 0
; CHECK-NEXT:  .quad [[L2]]
; CHECK-NEXT:  .long 2
; CHECK-NEXT:  .long 0
; CHECK-NEXT:  .quad [[L3]]
; CHECK-NEXT:  .long 3
; CHECK-NEXT:  .long 0
; CHECK-NEXT:  .quad [[L4]]
; CHECK-NEXT:  .long 4
; CHECK-NEXT:  .long 0

;; The JSON Lines body is a single .ascii directive.
; CHECK-NEXT:  .ascii "{\"type\":\"header\",\"version\":1,\"tu_id\":\"{{[0-9a-f]+}}\",\"source\":\"addrmap.c\",\"target\":\"x86_64-unknown-linux-gnu\",\"opt_level\":\"O2\",\"producer\":\"LLVM {{.*}} addrmap-patch\"}\n

;; leaq g+8(%rip) / movl $g+8: address of a global with a constant offset.
; CHECK-SAME:  {\"type\":\"record\",\"id\":0,\"function\":\"addr_global\",\"kind\":\"addr\",
; PIC-SAME:    \"insn\":{\"mnemonic\":\"leaq\",\"dest_reg\":\"rax\",\"addr\":{\"segment\":null,\"base_reg\":\"rip\",\"index_reg\":null,\"scale\":1,\"disp\":8,\"symbol\":\"g\",\"rip_relative\":true}},
; STATIC-SAME: \"insn\":{\"mnemonic\":\"movl\",\"dest_reg\":\"eax\",\"addr\":{\"segment\":null,\"base_reg\":null,\"index_reg\":null,\"scale\":1,\"disp\":8,\"symbol\":\"g\",\"rip_relative\":false}},
; CHECK-SAME:  \"base\":{\"kind\":\"global\",\"name\":\"g\",\"type\":null,\"type_size\":16,\"object\":{\"size\":16},\"decl\":null},
; CHECK-SAME:  \"ptr_offset\":{\"const\":8,\"var\":[]},
; CHECK-SAME:  \"out_of_bounds\":false,\"confidence\":\"derived\",\"provenance\":[\"machine-operand\"],\"reason\":null}\n

;; movq ext@GOTPCREL(%rip): address loaded from the GOT.
; CHECK-SAME:  {\"type\":\"record\",\"id\":1,\"function\":\"addr_got\",\"kind\":\"addr_got\",
; CHECK-SAME:  \"insn\":{\"mnemonic\":\"movq\",\"dest_reg\":\"rax\",\"addr\":{{{.*}}\"symbol\":\"ext\",\"rip_relative\":true}},
; CHECK-SAME:  \"base\":{\"kind\":\"global\",\"name\":\"ext\",
; CHECK-SAME:  \"path_str\":\"ext\",\"ptr_offset\":{\"const\":0,\"var\":[]},

;; leaq 16(%rdi): pointer derived from another pointer. Without debug info the
;; source pointer is not known, but the offset from it is.
; CHECK-SAME:  {\"type\":\"record\",\"id\":2,\"function\":\"derive\",\"kind\":\"derive\",
; CHECK-SAME:  \"addr\":{\"segment\":null,\"base_reg\":\"rdi\",\"index_reg\":null,\"scale\":1,\"disp\":16,\"symbol\":null,\"rip_relative\":false}},
; CHECK-SAME:  \"base\":{\"kind\":\"pointee\",\"name\":null,\"type\":null,\"type_size\":null,\"object\":{\"size\":null,\"size_unknown\":\"pointee\"},\"decl\":null},
; CHECK-SAME:  \"src_ptr\":{\"expr\":null,\"var\":null,\"type\":null,\"derived_from\":[]},
; CHECK-SAME:  \"ptr_offset\":{\"const\":16,\"var\":[]},
; CHECK-SAME:  \"confidence\":\"heuristic\",{{.*}}\"reason\":\"source pointer not known\"}\n

;; leaq (%rdi,%rdi,2): integer arithmetic. Only insn and source are recorded.
; CHECK-SAME:  {\"type\":\"record\",\"id\":3,\"function\":\"mul3\",\"kind\":\"not_pointer\",
; CHECK-SAME:  \"addr\":{\"segment\":null,\"base_reg\":\"rdi\",\"index_reg\":\"rdi\",\"scale\":2,\"disp\":0,\"symbol\":null,\"rip_relative\":false}},\"source\":null,\"confidence\":\"heuristic\",\"reason\":\"integer arithmetic shape (no base, or base == index)\"}\n

;; leaq 4(%rsp): address of a stack object. Without debug info there is no
;; variable to name it.
; CHECK-SAME:  {\"type\":\"record\",\"id\":4,\"function\":\"stack\",\"kind\":\"addr\",
; CHECK-SAME:  \"base_reg\":\"rsp\",
; CHECK-SAME:  \"base\":{\"kind\":\"unknown\",
; CHECK-SAME:  \"ptr_offset\":{\"const\":null,\"var\":[]},
; CHECK-SAME:  \"reason\":\"stack object without a variable\"}\n"

; NOMAP-NOT: {{Laddrmap|llvm_addrmap}}

; OBJ:      .llvm_addrmap PROGBITS 0000000000000000 {{[0-9a-f]+}} {{[0-9a-f]+}} 00 0 0 8
; OBJ:      Relocation section '.rela.llvm_addrmap'
; OBJ-NEXT: Offset
; OBJ-NEXT: 0000000000000030 {{.*}} R_X86_64_64 {{.*}} .text + 0
; OBJ-NEXT: 0000000000000040 {{.*}} R_X86_64_64 {{.*}} .text + 10
; OBJ-NEXT: 0000000000000050 {{.*}} R_X86_64_64 {{.*}} .text + 20
; OBJ-NEXT: 0000000000000060 {{.*}} R_X86_64_64 {{.*}} .text + 30
; OBJ-NEXT: 0000000000000070 {{.*}} R_X86_64_64 {{.*}} .text + 41

; HEX: 0x00000000 414d4150 01003000
