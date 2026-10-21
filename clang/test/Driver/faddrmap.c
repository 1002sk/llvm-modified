// RUN: %clang -### --target=x86_64-unknown-linux-gnu -faddrmap %s 2>&1 | FileCheck %s --check-prefix=ON
// RUN: %clang -### --target=x86_64-unknown-linux-gnu -faddrmap -fno-addrmap %s 2>&1 | FileCheck %s --check-prefix=OFF
// RUN: %clang -### --target=x86_64-unknown-linux-gnu %s 2>&1 | FileCheck %s --check-prefix=OFF

// ON: "-cc1"{{.*}} "-faddrmap"
// OFF-NOT: "-faddrmap"
