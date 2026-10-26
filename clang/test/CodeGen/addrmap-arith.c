// REQUIRES: x86-registered-target
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -target-cpu x86-64 -O0 \
// RUN:   -debug-info-kind=limited -mrelocation-model static -faddrmap -S -o - %s \
// RUN:   | FileCheck %s --check-prefixes=CHECK,O0
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -target-cpu x86-64 -O2 \
// RUN:   -debug-info-kind=limited -mrelocation-model static -faddrmap -S -o - %s \
// RUN:   | FileCheck %s --check-prefixes=CHECK,O2
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -target-cpu x86-64 -O2 \
// RUN:   -debug-info-kind=limited -mrelocation-model static -faddrmap \
// RUN:   -mllvm -addrmap-stats -S -o /dev/null %s 2>&1 \
// RUN:   | FileCheck %s --check-prefix=STATS

// Pointer arithmetic done by ADD/SUB/AND as well as LEA, described with the
// annotations from the IR.

typedef unsigned long uintptr_t;
struct Point { int x, y; };
struct Rect { struct Point a, b; };
struct Node { struct Node *next; int val; };
struct Rect g;
void sink(void *);

int *f1(void) { return &g.b.y; }
int *next_val(struct Node *p) { return &p->next->val; }
void loop(int *b, long n) {
  for (long i = 0; i < n; i++)
    sink(&b[i]);
}
void *align16(void *p) { return (void *)((uintptr_t)p & ~(uintptr_t)15); }
int *deref(int **q) { return *q + 2; }
char *bytes(char *p, long i) { return p + i; }
struct Item { int id; struct Point pos; };
struct World { long n; struct Item items[16]; };
struct Point *item_pos(struct World *p, long i) { return &p->items[i].pos; }

// -O0 computes &g.b.y step by step; each ADD derives from the previous one in
// place, so the earlier values are intermediate values of the last one.
// O0-LABEL: f1:
// O0:         .Laddrmap0:
// O0-NEXT:    movabsq $g, %rax
// O0-NEXT:    .Laddrmap1:
// O0-NEXT:    addq $8, %rax
// O0-NEXT:    .Laddrmap2:
// O0-NEXT:    addq $4, %rax
// O2-LABEL: f1:
// O2:         .Laddrmap0:
// O2-NEXT:    movl $g+12, %eax

// CHECK: .section .llvm_addrmap,"",@progbits

// O0:      \"function\":\"f1\",\"kind\":\"addr\"
// O0-SAME: \"path_str\":\"g\"
// O0-SAME: \"provenance\":[\"machine-operand\",\"debuginfo-type\",\"intermediate\"],\"intermediate_of\":2,\"reason\":\"intermediate value of g.b.y\"}
// O0-SAME: \"id\":1,\"function\":\"f1\",\"kind\":\"derive\",\"insn\":{\"mnemonic\":\"addq\"
// O0-SAME: \"src_ptr\":{\"expr\":\"g\",\"var\":null,\"type\":\"struct Rect *\",\"derived_from\":[0]}
// O0-SAME: \"path_str\":\"g.b\"
// O0-SAME: \"intermediate_of\":2,\"reason\":\"intermediate value of g.b.y\"}
// O0-SAME: \"id\":2,\"function\":\"f1\",\"kind\":\"derive\",\"insn\":{\"mnemonic\":\"addq\"
// O0-SAME: \"src_ptr\":{\"expr\":\"g.b\",\"var\":null,\"type\":\"struct Point *\",\"derived_from\":[1]}
// O0-SAME: \"path\":[{\"op\":\"field\",\"name\":\"b\",\"record\":\"struct Rect\",\"type\":\"struct Point\",\"offset_in_parent\":8,\"offset\":8,\"size\":8},{\"op\":\"field\",\"name\":\"y\",\"record\":\"struct Point\",\"type\":\"int\",\"offset_in_parent\":4,\"offset\":12,\"size\":4}],\"path_str\":\"g.b.y\"
// O0-SAME: \"provenance\":[\"machine-operand\",\"derived-from-record\",\"debuginfo-type\"],\"reason\":null}
// O2:      \"function\":\"f1\",\"kind\":\"addr\"
// O2-SAME: \"path_str\":\"g.b.y\"

// A pointer loaded from p->next. At -O0 p itself is reloaded from its stack
// slot first.
// CHECK-SAME: \"function\":\"next_val\",\"kind\":\"derive\",\"insn\":{\"mnemonic\":\"addq\"
// CHECK-SAME: \"src_ptr\":{\"expr\":\"p->next\",\"var\":null,\"type\":\"struct Node *\",\"derived_from\":[]}
// CHECK-SAME: \"path_str\":\"p->next->val\"
// CHECK-SAME: \"result_type\":\"int *\"
// CHECK-SAME: \"confidence\":\"derived\",\"provenance\":[\"machine-operand\",\"ir-annotation\",\"debuginfo-type\"]

// &b[i] in a loop: indexed at -O0, a strength-reduced induction pointer at -O2.
// CHECK-SAME: \"function\":\"loop\",\"kind\":\"derive\",\"insn\":{\"mnemonic\":\"addq\"
// CHECK-SAME: \"path_str\":\"b[?]\"
// O0-SAME:    \"confidence\":\"derived\"
// O2-SAME:    \"ptr_offset\":{\"const\":null,\"var\":[{\"reg\":null,\"scale\":4}]}
// O2-SAME:    \"confidence\":\"heuristic\",{{.*}}\"reason\":\"offset varies in a loop\"

// Alignment through an integer.
// O2-SAME: \"function\":\"align16\",\"kind\":\"mask\",\"insn\":{\"mnemonic\":\"andq\"
// O2-SAME: \"base\":{\"kind\":\"pointee\",\"name\":null,\"type\":\"void\"
// O2-SAME: \"reason\":\"bits of the pointer are masked\"

// A pointer loaded through a pointer.
// CHECK-SAME: \"function\":\"deref\",\"kind\":\"derive\"
// CHECK-SAME: \"src_ptr\":{\"expr\":\"(*q)\",\"var\":null,\"type\":\"int *\",\"derived_from\":[]}
// CHECK-SAME: \"path_str\":\"(*q)[2]\"

// Arithmetic on a char pointer is a byte offset only (R-PTR-6).
// CHECK-SAME: \"function\":\"bytes\",\"kind\":\"derive\"
// CHECK-SAME: \"path\":[],\"path_str\":\"\"
// CHECK-SAME: \"result_type\":\"char *\"
// CHECK-SAME: \"confidence\":\"derived\"

// &p->items[i].pos is p + 12*i + 12. At -O2, 12*i is (3*i)*4, and the slow
// three-operand LEA is split by X86FixupLEAs into
//   leaq (%rdi,%rax,4), %rax   ; p + 12*i, no C expression denotes it
//   addq $12, %rax             ; p + 12*i + 12, the annotated final value
// so the first one is recorded as an intermediate value of the second.
// O2-SAME: \"id\":6,\"function\":\"item_pos\",\"kind\":\"not_pointer\"
// O2-SAME: \"id\":7,\"function\":\"item_pos\",\"kind\":\"derive\",\"insn\":{\"mnemonic\":\"leaq\"
// O2-SAME: \"src_ptr\":{\"expr\":\"p\",\"var\":\"p\",\"type\":\"struct World *\",\"derived_from\":[]},\"path\":[],\"path_str\":\"\"
// O2-SAME: \"confidence\":\"derived\",\"provenance\":[\"machine-operand\",\"debuginfo-variable\",\"intermediate\"],\"intermediate_of\":8,\"reason\":\"intermediate value of p->items[?].pos\"}
// O2-SAME: \"id\":8,\"function\":\"item_pos\",\"kind\":\"derive\",\"insn\":{\"mnemonic\":\"addq\"
// O2-SAME: \"derived_from\":[7]
// O2-SAME: \"path_str\":\"p->items[?].pos\"
// At -O0 the steps p->items and p->items[?] are added in place.
// O0-SAME: \"id\":7,\"function\":\"item_pos\",\"kind\":\"derive\"
// O0-SAME: \"path_str\":\"p->items\"
// O0-SAME: \"intermediate_of\":9,\"reason\":\"intermediate value of p->items[?].pos\"}
// O0-SAME: \"id\":8,\"function\":\"item_pos\",\"kind\":\"derive\"
// O0-SAME: \"path_str\":\"p->items[?]\"
// O0-SAME: \"intermediate_of\":9,\"reason\":\"intermediate value of p->items[?].pos\"}
// O0-SAME: \"id\":9,\"function\":\"item_pos\",\"kind\":\"derive\"
// O0-SAME: \"path_str\":\"p->items[?].pos\"
// O0-SAME: \"provenance\":[\"machine-operand\",\"derived-from-record\",\"ir-annotation\",\"debuginfo-type\"],\"reason\":null}

// STATS: addrmap-stats: {{.*}}addrmap-arith.c: targets=9 addr=1 addr_got=0 derive=6 mask=1 not_pointer=1 unknown_base=0 annotated=6 ir_pointer_annotations={{[0-9]+}} derive_without_annotation=1 intermediate=1
