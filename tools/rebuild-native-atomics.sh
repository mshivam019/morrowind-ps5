#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$root/tools/env.sh"
# The PS5 compiler emits native x86-64 locked instructions for __sync builtins.
# Cross-run probes cannot execute PS5 binaries on the build host.
cmake -S "$root/deps/openscenegraph" -B "$root/build/openscenegraph" \
  -DOPENTHREADS_ATOMIC_USE_MUTEX=OFF \
  -D_OPENTHREADS_ATOMIC_USE_GCC_BUILTINS:BOOL=1 \
  -D_OPENTHREADS_ATOMIC_USE_MUTEX:BOOL=0
cmake --build "$root/build/openscenegraph" --target install -j "${PS5_BUILD_JOBS:-6}"
cmp "$root/build/openscenegraph/include/OpenThreads/Config" "$root/prefix/include/OpenThreads/Config"
# Atomic's layout changes. Rebuild every engine object, including plugins' users.
# Preserve the generated settings asset if a clean target knows about it.
cp "$root/build/openmw/defaults.bin" "$root/build/atomic-defaults.bin"
ninja -C "$root/build/openmw" -t clean
cp "$root/build/atomic-defaults.bin" "$root/build/openmw/defaults.bin"
cmake --build "$root/build/openmw" -j "${PS5_BUILD_JOBS:-6}" --target \
  components apps/openmw/libopenmw-lib.a \
  extern/osg-ffmpeg-videoplayer/libosg-ffmpeg-videoplayer.a \
  extern/oics/liboics.a extern/oics/liblocal_tinyxml.a \
  extern/smhasher/libsmhasher.a \
  apps/openmw/CMakeFiles/openmw.dir/main.cpp.o
