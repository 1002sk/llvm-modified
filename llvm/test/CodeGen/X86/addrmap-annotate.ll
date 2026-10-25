; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -addrmap \
; RUN:   -stop-after=addrmap-annotate | FileCheck %s --check-prefix=IR
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -addrmap -O2 \
; RUN:   | FileCheck %s --check-prefixes=ASM,O2
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -addrmap -O0 \
; RUN:   | FileCheck %s --check-prefixes=ASM,O0

;; The AddrMapAnnotate pass describes each pointer computation relative to a
;; root found from debug info, and marks integer arithmetic. Instruction
;; selection (SelectionDAG at -O2, FastISel at -O0) carries the annotation to
;; the instruction defining the value.

%struct.Node = type { ptr, i32 }

; int *next_val(struct Node *p) { return &p->next->val; }
define ptr @next_val(ptr %p) !dbg !10 {
entry:
    #dbg_value(ptr %p, !20, !DIExpression(), !21)
  %0 = load ptr, ptr %p, align 8, !dbg !22
  %val = getelementptr inbounds i8, ptr %0, i64 8, !dbg !22
  ret ptr %val, !dbg !22
}

; long mul3(long x) { return x * 3; }
define i64 @mul3(i64 %x) !dbg !30 {
entry:
  %mul = mul nsw i64 %x, 3, !dbg !31
  ret i64 %mul, !dbg !31
}

; IR-LABEL: define ptr @next_val(
; IR:         getelementptr inbounds i8, ptr %0, i64 8, !dbg !{{[0-9]+}}, !addrmap ![[PTR:[0-9]+]]
; IR-LABEL: define i64 @mul3(
; IR:         mul nsw i64 %x, 3, !dbg !{{[0-9]+}}, !addrmap ![[INT:[0-9]+]]
; IR-DAG:   ![[PTR]] = !{!"ptr", !"pointee", ![[NODE:[0-9]+]], i64 8, ![[EMPTY:[0-9]+]], !"p->next", ![[NODEPTR:[0-9]+]], i1 false}
; IR-DAG:   ![[NODE]] = distinct !DICompositeType(tag: DW_TAG_structure_type, name: "Node"
; IR-DAG:   ![[NODEPTR]] = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: ![[NODE]]
; IR-DAG:   ![[EMPTY]] = !{}
; IR-DAG:   ![[INT]] = !{!"int"}

;; The pointer loaded from p->next is described by that expression, so the
;; derived pointer resolves to p->next->val.
; O2-LABEL: next_val:
; O2:         [[L0:\.Laddrmap[0-9]+]]:
; O2-NEXT:    addq $8, %rax
; O0-LABEL: next_val:
; O0:         [[L0:\.Laddrmap[0-9]+]]:
; O0-NEXT:    addq $8, %rax
; O2-LABEL: mul3:
; O2:          [[L1:\.Laddrmap[0-9]+]]:
; O2-NEXT:     leaq (%rdi,%rdi,2), %rax
; ASM:       .quad [[L0]]
; O2:        .quad [[L1]]
; ASM:       \"function\":\"next_val\",\"kind\":\"derive\",\"insn\":{\"mnemonic\":\"addq\",\"dest_reg\":\"rax\",\"addr\":{\"segment\":null,\"base_reg\":\"rax\",\"index_reg\":null,\"scale\":1,\"disp\":8,
; ASM-SAME:  \"base\":{\"kind\":\"pointee\",\"name\":null,\"type\":\"struct Node\",\"type_size\":16,
; ASM-SAME:  \"src_ptr\":{\"expr\":\"p->next\",\"var\":null,\"type\":\"struct Node *\",\"derived_from\":[]}
; ASM-SAME:  \"path\":[{\"op\":\"field\",\"name\":\"val\",\"record\":\"struct Node\",\"type\":\"int\",\"offset_in_parent\":8,\"offset\":8,\"size\":4}],\"path_str\":\"p->next->val\"
; ASM-SAME:  \"confidence\":\"derived\",\"provenance\":[\"machine-operand\",\"ir-annotation\",\"debuginfo-type\"]
;; Integer arithmetic is not a pointer, for sure. At -O0 it is an imul.
; O2-SAME:   \"function\":\"mul3\",\"kind\":\"not_pointer\"
; O2-SAME:   \"confidence\":\"exact\",\"reason\":\"integer arithmetic in the IR\"
; O0-NOT:    \"function\":\"mul3\"

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3}

!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, producer: "clang", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug)
!1 = !DIFile(filename: "addrmap.c", directory: "/")
!2 = !{i32 7, !"Dwarf Version", i32 5}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!5 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !4, size: 64)
!6 = distinct !DICompositeType(tag: DW_TAG_structure_type, name: "Node", file: !1, line: 1, size: 128, elements: !7)
!7 = !{!8, !9}
!8 = !DIDerivedType(tag: DW_TAG_member, name: "next", scope: !6, file: !1, line: 1, baseType: !11, size: 64)
!9 = !DIDerivedType(tag: DW_TAG_member, name: "val", scope: !6, file: !1, line: 1, baseType: !4, size: 32, offset: 64)
!10 = distinct !DISubprogram(name: "next_val", scope: !1, file: !1, line: 2, type: !12, scopeLine: 2, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0)
!11 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !6, size: 64)
!12 = !DISubroutineType(types: !13)
!13 = !{!5, !11}
!20 = !DILocalVariable(name: "p", arg: 1, scope: !10, file: !1, line: 2, type: !11)
!21 = !DILocation(line: 0, scope: !10)
!22 = !DILocation(line: 2, column: 44, scope: !10)
!30 = distinct !DISubprogram(name: "mul3", scope: !1, file: !1, line: 3, type: !12, scopeLine: 3, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0)
!31 = !DILocation(line: 3, column: 30, scope: !30)
