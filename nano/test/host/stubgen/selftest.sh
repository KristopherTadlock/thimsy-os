#!/bin/sh
# Copyright (C) 2026 The Thimsy OS Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# gen.py end to end with a real compiler: compile testdata/core.cpp (which calls what
# testdata/dropped.cpp would define), take nm's undefined symbols, generate the stubs, compile and
# link them without dropped.cpp, and run the program: every call must return a zero value and each
# stub must log exactly once. Needs g++, nm and python3 (3.8+); builds under $1 (default
# ~/.cache/thimsy/nano-host/stubgen-selftest). The callers (stubs.mk) add nice and prlimit.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
nano=$(cd "$here/../../.." && pwd)
out=${1:-$HOME/.cache/thimsy/nano-host/stubgen-selftest}
CXX=${CXX:-g++}
NM=${NM:-nm}
PYTHON=${PYTHON:-python3}
flags="-std=gnu++17 -Wall -Wextra -Werror -O0 -g"

mkdir -p "$out"
$CXX $flags -I"$here/testdata" -c "$here/testdata/core.cpp" -o "$out/core.o"
$NM -C "$out/core.o" >"$out/core.nm"
$PYTHON "$here/gen.py" --nm "$out/core.nm" --from "$here/testdata/dropped.cpp" --decls "$here/testdata/fake.h" \
    --include-root "$here/testdata" --out "$out/stubs.cpp"
$CXX $flags -I"$here/testdata" -I"$nano/compat/stubs/include" -c "$out/stubs.cpp" -o "$out/stubs.o"
printf '%s\n' '#include <cstdio>' \
    'extern "C" void nanostub_log(const char* what) { std::fprintf(stderr, "nanostub: %s\n", what); }' \
    >"$out/log.cpp"
$CXX $flags -c "$out/log.cpp" -o "$out/log.o"
$CXX "$out/core.o" "$out/stubs.o" "$out/log.o" -o "$out/core"
"$out/core" 2>"$out/core.log"
n=$(grep -c 'nanoRgbFollowSample' "$out/core.log" || true)
if [ "$n" != 1 ]; then
    echo "stubgen selftest: nanoRgbFollowSample logged $n times, want 1" >&2
    cat "$out/core.log" >&2
    exit 1
fi
stubs=$(grep -c '^// android::' "$out/stubs.cpp" || true)
echo "stubgen selftest: $stubs stubs generated, linked and run"
