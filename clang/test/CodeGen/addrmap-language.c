// REQUIRES: x86-registered-target

// The addrmap supports C only. C++ and Objective-C are rejected.
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -x c++ -faddrmap \
// RUN:   -S -o /dev/null %s 2>&1 | FileCheck %s --check-prefix=CXX
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -x objective-c \
// RUN:   -faddrmap -S -o /dev/null %s 2>&1 | FileCheck %s --check-prefix=OBJC
// CXX:  error: invalid argument '-faddrmap' not allowed with 'C++'
// OBJC: error: invalid argument '-faddrmap' not allowed with 'Objective-C'

// C, and LLVM IR (as compiled by -save-temps), are accepted.
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -x c -faddrmap -S -o - %s \
// RUN:   | FileCheck %s --check-prefix=MAP
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -x c -emit-llvm-bc \
// RUN:   -o %t.bc %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -x ir -faddrmap -S \
// RUN:   -o - %t.bc | FileCheck %s --check-prefix=MAP
// MAP: .section .llvm_addrmap,"",@progbits

// Without -faddrmap, C++ is not affected.
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -x c++ -S -o - %s \
// RUN:   | FileCheck %s --check-prefix=NOMAP
// NOMAP-NOT: llvm_addrmap

struct S { int a, b; };
struct S g;
int *f(void) { return &g.b; }
