// REQUIRES: x86-registered-target
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -O2 -debug-info-kind=limited \
// RUN:   -mrelocation-model static -faddrmap -S -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -O2 -S -o - %s \
// RUN:   | FileCheck %s --check-prefix=NOMAP
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -O2 -faddrmap -emit-llvm \
// RUN:   -o - %s | FileCheck %s --check-prefix=NOMAP

struct Point { int x, y; };
struct Point g;
static int s[4];

int *get_y(void) { return &g.y; }
int *get_s(void) { static int t; return &t; }
int *field(struct Point *p) { return &p->y; }

// CHECK-LABEL: get_y:
// CHECK:         .Laddrmap0:
// CHECK-NEXT:    movl $g+4, %eax
// CHECK-LABEL: get_s:
// CHECK:         .Laddrmap1:
// CHECK-NEXT:    movl $get_s.t, %eax
// CHECK-LABEL: field:
// CHECK:         .Laddrmap2:
// CHECK-NEXT:    leaq 4(%rdi), %rax

// CHECK:      .section .llvm_addrmap,"",@progbits
// CHECK-NEXT: .p2align 3
// CHECK-NEXT: .ascii "AMAP"
// CHECK:      \"function\":\"get_y\",\"kind\":\"addr\"
// CHECK-SAME: \"source\":{\"file\":\"{{.*}}addrmap-section.c\",\"line\":13,\"column\":20,\"inlined_at\":[]}
// CHECK-SAME: \"base\":{\"kind\":\"global\",\"name\":\"g\",\"type\":\"struct Point\",\"type_size\":8,\"object\":{\"size\":8},\"decl\":{\"file\":\"{{.*}}addrmap-section.c\",\"line\":10}}
// CHECK-SAME: \"path_str\":\"g.y\",\"ptr_offset\":{\"const\":4,\"var\":[]}
// CHECK-SAME: \"function\":\"get_s\",\"kind\":\"addr\"
// CHECK-SAME: \"base\":{\"kind\":\"static_local\",\"name\":\"t\",
// CHECK-SAME: \"function\":\"field\",\"kind\":\"derive\"
// CHECK-SAME: \"base\":{\"kind\":\"pointee\",
// CHECK-SAME: \"path_str\":\"p->y\"

// NOMAP-NOT: {{Laddrmap|llvm_addrmap}}
