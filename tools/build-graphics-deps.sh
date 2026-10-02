#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$root/tools/env.sh"
common=(-G Ninja -DCMAKE_TOOLCHAIN_FILE="$root/cmake/ps5-native.cmake" -DCMAKE_INSTALL_PREFIX="$root/prefix" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=OFF)
cmake -S "$root/deps/libpng" -B "$root/build/libpng" "${common[@]}" -DPNG_SHARED=OFF -DPNG_TESTS=OFF -DPNG_TOOLS=OFF
cmake --build "$root/build/libpng" --target install -j 6
cmake -S "$root/deps/libjpeg-turbo" -B "$root/build/libjpeg-turbo" "${common[@]}" -DENABLE_SHARED=OFF -DWITH_TURBOJPEG=OFF -DWITH_SIMD=OFF
cmake --build "$root/build/libjpeg-turbo" --target jpeg-static -j 6
cp "$root/build/libjpeg-turbo/libjpeg.a" "$root/prefix/lib/"
cp "$root/deps/libjpeg-turbo/jpeglib.h" "$root/deps/libjpeg-turbo/jmorecfg.h" "$root/deps/libjpeg-turbo/jerror.h" "$root/build/libjpeg-turbo/jconfig.h" "$root/prefix/include/"
cmake -S "$root/deps/freetype" -B "$root/build/freetype" "${common[@]}" -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_HARFBUZZ=ON
cmake --build "$root/build/freetype" --target install -j 6
cmake -S "$root/deps/openscenegraph" -B "$root/build/openscenegraph" "${common[@]}" -DPS5_OSG_DETAILED_PROFILE="${PS5_OSG_DETAILED_PROFILE:-OFF}" -DOPENTHREADS_ATOMIC_USE_MUTEX=OFF -D_OPENTHREADS_ATOMIC_USE_GCC_BUILTINS:BOOL=1 -D_OPENTHREADS_ATOMIC_USE_MUTEX:BOOL=0 -DDYNAMIC_OPENSCENEGRAPH=OFF -DDYNAMIC_OPENTHREADS=OFF -DBUILD_OSG_APPLICATIONS=OFF -DBUILD_OSG_EXAMPLES=OFF -DBUILD_OSG_PLUGINS_BY_DEFAULT=OFF -DBUILD_OSG_PLUGIN_BMP=ON -DBUILD_OSG_PLUGIN_DDS=ON -DBUILD_OSG_PLUGIN_FREETYPE=ON -DBUILD_OSG_PLUGIN_JPEG=ON -DBUILD_OSG_PLUGIN_OSG=ON -DBUILD_OSG_PLUGIN_PNG=ON -DBUILD_OSG_PLUGIN_TGA=ON -DOSG_WINDOWING_SYSTEM=None -DOPENGL_PROFILE=GL2 -DOSG_GL1_AVAILABLE=ON -DOSG_GL2_AVAILABLE=ON -DOSG_GL3_AVAILABLE=OFF -DOSG_GL_DISPLAYLISTS_AVAILABLE=ON -DOSG_GL_FIXED_FUNCTION_AVAILABLE=ON -DOSG_GL_MATRICES_AVAILABLE=ON -DOSG_GL_VERTEX_ARRAY_FUNCS_AVAILABLE=ON -DOSG_GL_VERTEX_FUNCS_AVAILABLE=ON -DOSG_GL_LIBRARY_STATIC=OFF -DMATH_LIBRARY= -DRT_LIBRARY= -DDL_LIBRARY= -DOSG_TEXT_USE_FONTCONFIG=OFF -DOPENGL_gl_LIBRARY="$PS5SDK_ROOT/extracted/ps5-opengl-sdk-0.3.0/sdk/lib/libPS5OpenGLCore33.a" -DOPENGL_INCLUDE_DIR="$PS5SDK_ROOT/extracted/ps5-opengl-sdk-0.3.0/sdk/include" -DCMAKE_DISABLE_FIND_PACKAGE_X11=ON -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON
cmake --build "$root/build/openscenegraph" --target install -j 6
cmake -S "$root/deps/mygui" -B "$root/build/mygui" "${common[@]}" -DMYGUI_STATIC=ON -DMYGUI_DONT_USE_OBSOLETE=ON -DMYGUI_RENDERSYSTEM=1 -DMYGUI_BUILD_DEMOS=OFF -DMYGUI_BUILD_TOOLS=OFF -DMYGUI_BUILD_PLUGINS=OFF -DMYGUI_BUILD_DOCS=OFF -DMYGUI_DISABLE_PLUGINS=ON
cmake --build "$root/build/mygui" --target install -j 6
