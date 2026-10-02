#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$root/tools/env.sh"
common=(-G Ninja -DCMAKE_TOOLCHAIN_FILE="$root/cmake/ps5-native.cmake" -DCMAKE_INSTALL_PREFIX="$root/prefix" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=OFF)
cmake -S "$root/deps/lz4/build/cmake" -B "$root/build/lz4" "${common[@]}" -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF
cmake --build "$root/build/lz4" --target install -j 6
cmake -S "$root/deps/bullet" -B "$root/build/bullet" "${common[@]}" -DUSE_DOUBLE_PRECISION=ON -DBUILD_BULLET3=OFF -DBUILD_EXTRAS=OFF -DBUILD_OPENGL3_DEMOS=OFF -DBUILD_UNIT_TESTS=OFF -DBUILD_BULLET2_DEMOS=OFF -DBUILD_CPU_DEMOS=OFF -DBUILD_ENET=OFF -DBUILD_CLSOCKET=OFF -DBUILD_EGL=OFF
cmake --build "$root/build/bullet" --target BulletCollision LinearMath -j 6
mkdir -p "$root/prefix/lib" "$root/prefix/include/bullet" "$root/prefix/lib/cmake/bullet"
cp "$root/cmake/BulletConfig-float64.cmake" "$root/prefix/lib/cmake/bullet/"
cp "$root/build/bullet/src/BulletCollision/libBulletCollision.a" "$root/build/bullet/src/LinearMath/libLinearMath.a" "$root/prefix/lib/"
cp -R "$root/deps/bullet/src/BulletCollision" "$root/deps/bullet/src/LinearMath" "$root/deps/bullet/src/btBulletCollisionCommon.h" "$root/prefix/include/bullet/"
cmake -S "$root/deps/recastnavigation" -B "$root/build/recast" "${common[@]}" -DRECASTNAVIGATION_ENABLE_ASSERTS=0 -DRECASTNAVIGATION_DEMO=OFF -DRECASTNAVIGATION_TESTS=OFF -DRECASTNAVIGATION_EXAMPLES=OFF
cmake --build "$root/build/recast" --target install -j 6
