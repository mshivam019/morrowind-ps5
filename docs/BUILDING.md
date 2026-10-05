# Building from source

Use Linux or WSL2, Clang/LLVM 18 wrappers, CMake, Ninja, Python 3, Bash, Git, patch, Make and the source-build prerequisites of Mesa and the engine dependencies. Python tools use only the standard library. The tested packaging step also linked Clang 22 compiler builtins; the current host compiler selects that archive unless `PS5_COMPILER_RT` is set.

## Dependency layout

The repository contains PS5 changes rather than vendored upstream engines. The release’s `morrowind-ps5-v1.0.0-source.tar.gz` includes actual source snapshots, already carrying the changes used for the build:

```text
morrowind-ps5-v1.0.0-source/
  port/                         this repository
    openmw/                     patched OpenMW 0.51.0
    deps/                       engine library source trees and SDL source/integration
  ps5sdk/
    native-app-boilerplate/     native title/container/runtime sources
    ps5-opengl-030/ps5-opengl/   graphics compiler/runtime sources and public dependencies
    deps/zlib/                  zlib 1.3.1 sources used for the engine
```

Compiled libraries, host executables, public SDK binaries, Git history, game data and private notes are excluded. The snapshots include upstream license texts and the port’s patches/overlays. The archive is source input, not a preconfigured build environment; a complete fresh-machine rebuild has not been validated.

Set `PS5SDK_ROOT` to the archive’s `ps5sdk` directory or your equivalent workspace. Obtain the public [PS5 payload SDK v0.42](https://github.com/ps5-payload-dev/sdk/releases/tag/v0.42) and place it under `native-app-boilerplate/.deps/native/ps5-payload-sdk`. The boilerplate’s setup scripts describe the SDK bootstrap. Its Clang wrappers and toolchain paths must match your host.

The graphics prerequisite is [ps5-opengl SDK 0.3.0](https://github.com/blackbearreloaded/ps5-opengl/releases/tag/v0.3.0). Place its extracted layout at `$PS5SDK_ROOT/extracted/ps5-opengl-sdk-0.3.0` and source at `$PS5SDK_ROOT/ps5-opengl-030/ps5-opengl`. Keep the package’s `runtime-config.txt`, `sdk` and notices together. The driver scripts use that configuration and replace selected runtime members locally; they do not change the installed SDK. The source archive includes the matching generated Mesa/PSBC headers and source inputs where needed, but no compiled archives.

Build or provide zlib under `$PS5SDK_ROOT/prefix`. The engine requires `prefix/lib/libz.a` and its headers. SDL sources live at `deps/sdl2-compat/SDL`; its integration overlay is under `deps/sdl2-compat/integration`.

## Fresh Git clone alternative

`sources.lock.json` and `graphics-sources.lock.json` pin OpenMW and the Git-based engine libraries. Clone those URLs to the named paths and check out the exact commits. Apply `patches/openmw-ps5.patch` to OpenMW and `patches/openscenegraph-ps5.patch` to OpenSceneGraph with `patch -p1`; then copy the `source-overlays` files into their matching relative paths. Do not reapply these patches to the source archive’s already-patched snapshots.

Other engine libraries are Boost 1.85.0, ICU 76.1, yaml-cpp 0.8.0, Lua 5.4.7 and SQLite amalgamation 3.47.2. Their archive checksums are in `dependency-archives.sha256`. FFmpeg 6.1.3 and OpenAL Soft 1.23.1 URLs/checksums are in `tools/build-media-deps.sh`. The release source archive provides these source directories without requiring you to reconstruct the development downloads. The SDL snapshot is included because a clean source-bootstrap script is not yet provided.

## Compile and package

From the `port` directory, after preparing the SDK/toolchain and source directories:

```sh
export PS5SDK_ROOT=/path/to/ps5sdk
source tools/env.sh
bash tools/build-deps.sh
bash tools/build-core-deps.sh
bash tools/build-sdl2-compat.sh
bash tools/build-graphics-deps.sh
bash tools/build-media-deps.sh
python3 tools/build-fixed-function-psbc.py
PS5_PORT_PROFILE=0 PS5_NATIVE_SUBMIT_COALESCE=1 PS5_ASYNC_BATCH=1 \
PS5_PRESENT_COMPLETION_WAIT=1 PS5_CLFLUSHOPT=1 PS5_LIGHT_DIAGNOSTICS=0 \
python3 tools/build-draw-batching-driver.py
PS5_SDL2_PREFIX="$PWD/prefix" bash tools/configure.sh
python3 tools/compile-openmw.py
python3 tools/pack-openmw.py --out build/package
```

Several build scripts expect the SDK’s default layout shown above. On a different layout, inspect their supported `PS5_OPENGL_ROOT`, `PS5_OPENGL_SDK`, `PS5_NATIVE_APP_TEMPLATE` and `PS5_SDL2_PREFIX` overrides. Do not assume that environment variables make every prerequisite relocatable.

The folder title is generated at `build/package/dist/PPSA99630`. Supply your own game data only for console testing; keep it outside source control and release packages. Detailed profiling and HD texture flags are off for the release.

## Host checks

```sh
python3 -m unittest discover -s tests -p 'test_*.py'
python3 tools/test-native-coalesce.py
python3 tools/test-async-screen-ownership.py
python3 tools/test-present-boundary-drain.py
```

The native keyboard test uses the host SDL2 and libdecor development headers:

```sh
cc -g -fsanitize=address,undefined -DSDL_VIDEO_DRIVER_PS5=1 -D__PROSPERO__ \
  -I/usr/include/SDL2 -I/usr/include/libdecor-0 -Ideps/sdl2-compat/SDL/src \
  tests/runtime/ps5-ime.c -lSDL2 -o /tmp/morrow-ime-test
/tmp/morrow-ime-test
```

Rendering fixtures need the graphics sources and, for some checks, the target compiler. Host tests do not replace console validation.
