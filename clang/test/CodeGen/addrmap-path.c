// REQUIRES: x86-registered-target
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -O2 -debug-info-kind=limited \
// RUN:   -mrelocation-model static -faddrmap -S -o - %s \
// RUN:   | FileCheck %s --check-prefixes=CHECK,STATIC
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -O2 -debug-info-kind=limited \
// RUN:   -pic-level 2 -pic-is-pie -faddrmap -S -o - %s \
// RUN:   | FileCheck %s --check-prefixes=CHECK,PIE

// Access paths and ranges resolved from debug info types.

struct Vec2 { int x, y; };
struct Item { int id; struct Vec2 pos; };
struct World { long n; struct Item items[16]; };
struct Inner { int a; int arr[4]; };
struct Outer { char tag; struct Inner inner; };
struct Anon { int k; union { int i; float f; }; struct { short s1, s2; }; };
union U { int i; struct Vec2 v; };
struct World gw;
int grid[3][4];
struct Anon ga;
union U gu;
int arr[10];
void sink(void *);

int *deep(struct Outer *o) { return &o->inner.arr[2]; }
int *pidx(int *p, long i) { return p + i; }
int *pminus(int *p) { return p - 1; }
int *g2d(void) { return &grid[1][2]; }
int *past(void) { return &arr[10]; }
int *elem(long i) { return &arr[i]; }
short *anon(void) { return &ga.s2; }
float *anon_union(void) { return &ga.f; }
int *uni(void) { return &gu.v.y; }
char *mid(void) { return (char *)&gw.items[1].pos + 2; }
struct Vec2 *whole(void) { return &gw.items[3].pos; }
void locals(int n) {
  struct Outer o;
  int a[8];
  sink(&o.inner.arr[1]);
  sink(a);
  sink(&n);
}
long add(long a, long b) { return a + b; }

// CHECK: .section .llvm_addrmap,"",@progbits

// Pointer to a struct to the innermost field: base is the pointee of o, and
// every step carries its range relative to the value of o.
// CHECK:      \"function\":\"deep\",\"kind\":\"derive\"
// CHECK-SAME: \"base\":{\"kind\":\"pointee\",\"name\":null,\"type\":\"struct Outer\",\"type_size\":24,\"object\":{\"size\":null,\"size_unknown\":\"pointee\"},\"decl\":null}
// CHECK-SAME: \"src_ptr\":{\"expr\":\"o\",\"var\":\"o\",\"type\":\"struct Outer *\",\"derived_from\":[]}
// CHECK-SAME: \"path\":[{\"op\":\"field\",\"name\":\"inner\",\"record\":\"struct Outer\",\"type\":\"struct Inner\",\"offset_in_parent\":4,\"offset\":4,\"size\":20},{\"op\":\"field\",\"name\":\"arr\",\"record\":\"struct Inner\",\"type\":\"int[4]\",\"offset_in_parent\":4,\"offset\":8,\"size\":16,\"length\":4,\"stride\":4},{\"op\":\"index\",\"index\":2,\"index_reg\":null,\"type\":\"int\",\"offset_in_parent\":8,\"offset\":16,\"size\":4}]
// CHECK-SAME: \"path_str\":\"o->inner.arr[2]\",\"ptr_offset\":{\"const\":16,\"var\":[]},\"byte_offset_in_field\":0,\"result_type\":\"int *\"
// CHECK-SAME: \"confidence\":\"derived\",\"provenance\":[\"machine-operand\",\"debuginfo-variable\",\"debuginfo-type\"]

// p + i: a variable subscript of the pointer itself.
// CHECK-SAME: \"function\":\"pidx\",\"kind\":\"derive\"
// CHECK-SAME: \"path\":[{\"op\":\"index\",\"index\":null,\"index_reg\":\"rsi\",\"type\":\"int\",\"offset_in_parent\":null,\"offset\":null,\"size\":4}],\"path_str\":\"p[?]\",\"ptr_offset\":{\"const\":0,\"var\":[{\"reg\":\"rsi\",\"scale\":4}]}

// p - 1 is out of bounds.
// CHECK-SAME: \"function\":\"pminus\",\"kind\":\"derive\"
// CHECK-SAME: \"path_str\":\"p[-1]\"
// CHECK-SAME: \"out_of_bounds\":true

// Two-dimensional array.
// CHECK-SAME: \"function\":\"g2d\",\"kind\":\"addr\"
// CHECK-SAME: \"base\":{\"kind\":\"global\",\"name\":\"grid\",\"type\":\"int[3][4]\",\"type_size\":48,\"object\":{\"size\":48},\"decl\":{\"file\":\"{{.*}}addrmap-path.c\",\"line\":19}}
// CHECK-SAME: \"path\":[{\"op\":\"index\",\"index\":1,\"index_reg\":null,\"type\":\"int[4]\",\"offset_in_parent\":16,\"offset\":16,\"size\":16,\"length\":4,\"stride\":4},{\"op\":\"index\",\"index\":2,\"index_reg\":null,\"type\":\"int\",\"offset_in_parent\":8,\"offset\":24,\"size\":4}],\"path_str\":\"grid[1][2]\"

// One past the end.
// CHECK-SAME: \"function\":\"past\",\"kind\":\"addr\"
// CHECK-SAME: \"path_str\":\"arr[10]\"
// CHECK-SAME: \"out_of_bounds\":true

// &arr[i]: one instruction without PIC, a chain of two with PIE.
// STATIC-SAME: \"function\":\"elem\",\"kind\":\"addr\"
// STATIC-SAME: \"path_str\":\"arr[?]\"
// PIE-SAME:    \"id\":5,\"function\":\"elem\",\"kind\":\"addr\"
// PIE-SAME:    \"path_str\":\"arr\"
// PIE-SAME:    \"alternatives\":[{\"path_str\":\"arr[0]\",\"reason\":\"offset 0 of int[10]\"}]
// PIE-SAME:    \"id\":6,\"function\":\"elem\",\"kind\":\"derive\"
// PIE-SAME:    \"base\":{\"kind\":\"global\",\"name\":\"arr\",
// PIE-SAME:    \"src_ptr\":{\"expr\":\"arr\",\"var\":null,\"type\":\"int[10] *\",\"derived_from\":[5]}
// PIE-SAME:    \"path\":[{\"op\":\"index\",\"index\":null,\"index_reg\":\"rdi\",\"type\":\"int\",\"offset_in_parent\":null,\"offset\":null,\"size\":4}],\"path_str\":\"arr[?]\"
// PIE-SAME:    \"provenance\":[\"machine-operand\",\"derived-from-record\",\"debuginfo-type\"]

// Members of anonymous structs and unions are named directly.
// CHECK-SAME: \"function\":\"anon\",\"kind\":\"addr\"
// CHECK-SAME: \"path\":[{\"op\":\"field\",\"name\":null,\"record\":\"struct Anon\",\"type\":\"struct <anonymous>\",\"offset_in_parent\":8,\"offset\":8,\"size\":4},{\"op\":\"field\",\"name\":\"s2\",
// CHECK-SAME: \"path_str\":\"ga.s2\"

// A union lists all of its members.
// CHECK-SAME: \"function\":\"anon_union\",\"kind\":\"addr\"
// CHECK-SAME: \"alternatives\":[{\"path_str\":\"ga.i\",\"reason\":\"offset 0 of union <anonymous>\"},{\"path_str\":\"ga.f\",\"reason\":\"offset 0 of union <anonymous>\"}]

// The offset singles out one member of a union.
// CHECK-SAME: \"function\":\"uni\",\"kind\":\"addr\"
// CHECK-SAME: \"path_str\":\"gu.v.y\"

// Into the middle of a field.
// CHECK-SAME: \"function\":\"mid\",\"kind\":\"addr\"
// CHECK-SAME: \"path_str\":\"gw.items[1].pos.x\",\"ptr_offset\":{\"const\":26,\"var\":[]},\"byte_offset_in_field\":2,\"result_type\":\"\"

// Offset 0 of a struct also names its first field.
// CHECK-SAME: \"function\":\"whole\",\"kind\":\"addr\"
// CHECK-SAME: \"path_str\":\"gw.items[3].pos\",\"ptr_offset\":{\"const\":48,\"var\":[]},\"byte_offset_in_field\":0,\"result_type\":\"struct Vec2 *\"
// CHECK-SAME: \"alternatives\":[{\"path_str\":\"gw.items[3].pos.x\",\"reason\":\"offset 0 of struct Vec2\"}]

// Stack objects are matched to variables and parameters.
// CHECK-SAME: \"function\":\"locals\",\"kind\":\"addr\"
// CHECK-SAME: \"base\":{\"kind\":\"local\",\"name\":\"o\",\"type\":\"struct Outer\",\"type_size\":24,\"object\":{\"size\":24},\"decl\":{\"file\":\"{{.*}}addrmap-path.c\",\"line\":37}}
// CHECK-SAME: \"path_str\":\"o.inner.arr[1]\",\"ptr_offset\":{\"const\":12,\"var\":[]}
// CHECK-SAME: \"function\":\"locals\",\"kind\":\"addr\"
// CHECK-SAME: \"base\":{\"kind\":\"local\",\"name\":\"a\",\"type\":\"int[8]\"
// CHECK-SAME: \"path_str\":\"a\"
// CHECK-SAME: \"function\":\"locals\",\"kind\":\"addr\"
// CHECK-SAME: \"base\":{\"kind\":\"param\",\"name\":\"n\",\"type\":\"int\"
// CHECK-SAME: \"path_str\":\"n\"

// A LEA on integer variables.
// CHECK-SAME: \"function\":\"add\",\"kind\":\"not_pointer\"
// CHECK-SAME: \"reason\":\"base register holds integer variable a\"}\n"
