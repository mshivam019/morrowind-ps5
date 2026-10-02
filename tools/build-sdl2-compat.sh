#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$root/tools/env.sh"
cmake -S "$root/deps/sdl2-compat/integration" -B "$root/build/sdl2-compat" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$root/cmake/ps5-native.cmake" -DCMAKE_INSTALL_PREFIX="$root/prefix" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DSDL_SOURCE="$root/deps/sdl2-compat/SDL" -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON
cmake --build "$root/build/sdl2-compat" --target install -j 3
