// RUN: %clang -### --target=x86_64-unknown-linux-gnu -fmemlock-map %s 2>&1 | FileCheck %s --check-prefix=LOCK
// RUN: %clang -### --target=x86_64-unknown-linux-gnu -fmemlock-map=all %s 2>&1 | FileCheck %s --check-prefix=ALL
// RUN: %clang -### --target=x86_64-unknown-linux-gnu -fmemlock-map=all -fmemlock-map %s 2>&1 | FileCheck %s --check-prefix=LOCK
// RUN: %clang -### --target=x86_64-unknown-linux-gnu -fmemlock-map -fno-memlock-map %s 2>&1 | FileCheck %s --check-prefix=OFF
// RUN: %clang -### --target=x86_64-unknown-linux-gnu %s 2>&1 | FileCheck %s --check-prefix=OFF
// RUN: not %clang -fsyntax-only --target=x86_64-unknown-linux-gnu -fmemlock-map=bogus %s 2>&1 | FileCheck %s --check-prefix=BAD

// Only C on x86-64 Linux ELF is supported.
// RUN: not %clang -fsyntax-only --target=i686-unknown-linux-gnu -fmemlock-map %s 2>&1 | FileCheck %s --check-prefix=TARGET -DTRIPLE=i686-unknown-linux-gnu
// RUN: not %clang -fsyntax-only --target=aarch64-unknown-linux-gnu -fmemlock-map %s 2>&1 | FileCheck %s --check-prefix=TARGET -DTRIPLE=aarch64-unknown-linux-gnu
// RUN: not %clang -fsyntax-only --target=x86_64-unknown-linux-gnux32 -fmemlock-map %s 2>&1 | FileCheck %s --check-prefix=TARGET -DTRIPLE=x86_64-unknown-linux-gnux32
// RUN: not %clang -fsyntax-only --target=x86_64-unknown-freebsd -fmemlock-map=all %s 2>&1 | FileCheck %s --check-prefix=TARGET -DTRIPLE=x86_64-unknown-freebsd
// RUN: not %clang -fsyntax-only --target=x86_64-apple-macosx10.15 -fmemlock-map %s 2>&1 | FileCheck %s --check-prefix=TARGET -DTRIPLE=x86_64-apple-macosx10.15.0
// RUN: not %clang -fsyntax-only --target=x86_64-unknown-linux-gnu -fmemlock-map -x c++ %s 2>&1 | FileCheck %s --check-prefix=LANG -DLANG=C++
// RUN: not %clang -fsyntax-only --target=x86_64-unknown-linux-gnu -fmemlock-map -x objective-c %s 2>&1 | FileCheck %s --check-prefix=LANG -DLANG=Objective-C
// RUN: %clang -fsyntax-only --target=x86_64-unknown-linux-gnu -fmemlock-map %s
// RUN: %clang -fsyntax-only --target=x86_64-unknown-linux-gnu -fmemlock-map -fno-memlock-map -x c++ %s

// LOCK: "-cc1"{{.*}} "-fmemlock-map=lock"
// ALL: "-cc1"{{.*}} "-fmemlock-map=all"
// OFF-NOT: "-fmemlock-map
// BAD: error: invalid value 'bogus' in '-fmemlock-map=bogus'
// TARGET: error: unsupported option '-fmemlock-map' for target '[[TRIPLE]]'
// LANG: error: invalid argument '-fmemlock-map' not allowed with '[[LANG]]'
