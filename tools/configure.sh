#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$root/tools/env.sh"
args=()
for target in LAUNCHER WIZARD OPENCS MWINIIMPORTER ESSIMPORTER BSATOOL ESMTOOL NIFTEST NAVMESHTOOL BULLETOBJECTTOOL; do
    args+=("-DBUILD_${target}=OFF")
done
cmake -S "$root/openmw" -B "$root/build/openmw" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$root/cmake/ps5-native.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DBullet_DIR="$root/prefix/lib/cmake/bullet" \
    -DLUA_MATH_LIBRARY="$PS5_PAYLOAD_SDK/target/lib/libSceLibcInternal.so" \
    -DOSG_STATIC=ON -DMYGUI_STATIC=ON -DBULLET_STATIC=ON -DUSE_LUAJIT=OFF \
    -DOPENMW_USE_SYSTEM_RECASTNAVIGATION=ON -DOPENMW_USE_SYSTEM_SQLITE3=ON \
    -DOPENGL_gl_LIBRARY="$PS5SDK_ROOT/extracted/ps5-opengl-sdk-0.3.0/sdk/lib/libPS5OpenGL.a" \
    -DOPENGL_INCLUDE_DIR="$PS5SDK_ROOT/extracted/ps5-opengl-sdk-0.3.0/sdk/include" \
    "${args[@]}" "$@"
