// RUN: %clang -### --target=x86_64-unknown-linux-gnu -fmemlock-map %s 2>&1 | FileCheck %s --check-prefix=LOCK
// RUN: %clang -### --target=x86_64-unknown-linux-gnu -fmemlock-map=all %s 2>&1 | FileCheck %s --check-prefix=ALL
// RUN: %clang -### --target=x86_64-unknown-linux-gnu -fmemlock-map=all -fmemlock-map %s 2>&1 | FileCheck %s --check-prefix=LOCK
// RUN: %clang -### --target=x86_64-unknown-linux-gnu -fmemlock-map -fno-memlock-map %s 2>&1 | FileCheck %s --check-prefix=OFF
// RUN: %clang -### --target=x86_64-unknown-linux-gnu %s 2>&1 | FileCheck %s --check-prefix=OFF
// RUN: not %clang -fsyntax-only --target=x86_64-unknown-linux-gnu -fmemlock-map=bogus %s 2>&1 | FileCheck %s --check-prefix=BAD

// LOCK: "-cc1"{{.*}} "-fmemlock-map=lock"
// ALL: "-cc1"{{.*}} "-fmemlock-map=all"
// OFF-NOT: "-fmemlock-map
// BAD: error: invalid value 'bogus' in '-fmemlock-map=bogus'
