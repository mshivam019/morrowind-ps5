#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
source "$ROOT/tools/env.sh"
CC="$PS5SDK_ROOT/native-app-boilerplate/tooling/prospero-clang18"
CXX="$PS5SDK_ROOT/native-app-boilerplate/tooling/prospero-clang18++"
AR=$(command -v llvm-ar-18 || command -v llvm-ar)
RANLIB=$(command -v llvm-ranlib-18 || command -v llvm-ranlib)
JOBS=${JOBS:-3}
PREFIX="$ROOT/prefix"
mkdir -p "$PREFIX/include" "$PREFIX/lib" "$ROOT/build/core"
cmake -S "$ROOT/deps/yaml-cpp-0.8.0" -B "$ROOT/build/yaml" -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/ps5-native.cmake" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_INSTALL_PREFIX="$PREFIX" -DYAML_CPP_BUILD_TESTS=OFF -DYAML_CPP_BUILD_TOOLS=OFF -DYAML_CPP_BUILD_CONTRIB=OFF
cmake --build "$ROOT/build/yaml" -j"$JOBS"
cmake --install "$ROOT/build/yaml"
# Native libc exports ISO C setjmp/longjmp, but not POSIX underscore forms.
sed -i 's/^#elif defined(LUA_USE_POSIX)[[:space:]]*\/\* }{ \*\//#elif defined(LUA_USE_POSIX) \&\& !defined(__PROSPERO__) \/\* }{ \*\//' "$ROOT/deps/lua-5.4.7/src/ldo.c"
for src in "$ROOT"/deps/lua-5.4.7/src/*.c; do
 name=$(basename "$src" .c)
 case "$name" in lua|luac) continue;; esac
 "$CC" -O2 -fPIC -D__PROSPERO__ -DLUA_USE_POSIX -c "$src" -o "$ROOT/build/core/lua-$name.o"
done
"$AR" rcs "$PREFIX/lib/liblua.a" "$ROOT"/build/core/lua-*.o
cp "$ROOT"/deps/lua-5.4.7/src/{lua.h,luaconf.h,lauxlib.h,lualib.h,lua.hpp} "$PREFIX/include/"
SQLITE_SRC="$ROOT/deps/sqlite-amalgamation-3470200"
"$CC" -O2 -fPIC -D__PROSPERO__ -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -c "$SQLITE_SRC/sqlite3.c" -o "$ROOT/build/core/sqlite3.o"
"$AR" rcs "$PREFIX/lib/libsqlite3.a" "$ROOT/build/core/sqlite3.o"
cp "$SQLITE_SRC"/{sqlite3.h,sqlite3ext.h} "$PREFIX/include/"
# ICU host programs generate Unicode data for the target build.
mkdir -p "$ROOT/build/icu-host" "$ROOT/build/icu-ps5"
if [[ ! -f "$ROOT/build/icu-host/Makefile" ]]; then
 (cd "$ROOT/build/icu-host"; "$ROOT/deps/icu/source/runConfigureICU" Linux --disable-shared --enable-static --disable-tests --disable-samples)
fi
make -C "$ROOT/build/icu-host" -j"$JOBS"
(cd "$ROOT/build/icu-ps5"
 CC="$ROOT/tools/icu-ps5-cc.sh" CXX="$ROOT/tools/icu-ps5-cxx.sh" AR="$AR" RANLIB="$RANLIB" CFLAGS='-O2 -fPIC -D__PROSPERO__' CXXFLAGS='-O2 -fPIC -D__PROSPERO__ -DU_HAVE_NL_LANGINFO_CODESET=0 -fexceptions -frtti' "$ROOT/deps/icu/source/configure" --host=x86_64-unknown-freebsd --build=x86_64-pc-linux-gnu --prefix="$PREFIX" --with-cross-build="$ROOT/build/icu-host" --disable-shared --enable-static --disable-tests --disable-samples --disable-tools --disable-extras
 make -j"$JOBS" AR="$AR" RANLIB="$RANLIB"
 make AR="$AR" RANLIB="$RANLIB" install
)
BOOST_SRC="$ROOT/deps/boost_1_85_0"
if [[ ! -x "$BOOST_SRC/b2" ]]; then (cd "$BOOST_SRC"; ./bootstrap.sh --with-libraries=iostreams,program_options); fi
cat > "$ROOT/build/boost-user-config.jam" <<JAM
using clang : ps5 : $CXX : <compileflags>"-D__PROSPERO__ -fPIC -fexceptions -frtti" <archiver>$AR <ranlib>$RANLIB ;
JAM
(cd "$BOOST_SRC"; ./b2 --user-config="$ROOT/build/boost-user-config.jam" --with-program_options --with-iostreams toolset=clang-ps5 target-os=freebsd architecture=x86 address-model=64 variant=release link=static threading=multi -sNO_BZIP2=1 -sNO_LZMA=1 -sNO_ZSTD=1 -sZLIB_INCLUDE="$PS5SDK_ROOT/prefix/include" -sZLIB_LIBPATH="$PS5SDK_ROOT/prefix/lib" --prefix="$PREFIX" -j"$JOBS" install)
