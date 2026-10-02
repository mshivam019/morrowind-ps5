#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$root/tools/env.sh"
# Pin source archives and verify before extracting.
mkdir -p "$root/deps/downloads"
fetch_source() {
    local archive="$1" url="$2" digest="$3" directory="$4"
    if [[ ! -f "$root/deps/downloads/$archive" ]]; then
        curl -L --fail "$url" -o "$root/deps/downloads/$archive"
    fi
    printf '%s  %s\n' "$digest" "$root/deps/downloads/$archive" | sha256sum --check --status
    if [[ ! -d "$root/deps/$directory" ]]; then
        tar -xf "$root/deps/downloads/$archive" -C "$root/deps"
    fi
}
fetch_source ffmpeg-6.1.3.tar.xz https://ffmpeg.org/releases/ffmpeg-6.1.3.tar.xz bc5f1e4a4d283a6492354684ee1124129c52293bcfc6a9169193539fbece3487 ffmpeg-6.1.3
fetch_source openal-soft-1.23.1.tar.gz https://github.com/kcat/openal-soft/archive/refs/tags/1.23.1.tar.gz dfddf3a1f61059853c625b7bb03de8433b455f2f79f89548cbcbd5edca3d4a4a openal-soft-1.23.1
if ! rg -q 'static const PathNamePair procbin\{"/app0", "eboot.bin"\}' "$root/deps/openal-soft-1.23.1/core/helpers.cpp"; then
    patch -d "$root/deps/openal-soft-1.23.1" -p1 < "$root/tools/patches/openal-ps5-helpers.patch"
fi
cc="$PS5SDK_ROOT/native-app-boilerplate/tooling/prospero-clang18"
mkdir -p "$root/build/ffmpeg"
cd "$root/build/ffmpeg"
"$root/deps/ffmpeg-6.1.3/configure" --prefix="$root/prefix" --target-os=freebsd --arch=x86_64 --enable-cross-compile --cc="$cc" --ar=llvm-ar --ranlib=llvm-ranlib --ld=ld.lld --extra-ldflags="-L$PS5_PAYLOAD_SDK/target/lib -lSceLibcInternal -lkernel -lpthread" --enable-static --disable-shared --disable-programs --disable-doc --disable-network --disable-avdevice --disable-avfilter --disable-postproc --disable-encoders --disable-muxers --disable-hwaccels --disable-x86asm --disable-autodetect --disable-symver --disable-everything --enable-avcodec --enable-avformat --enable-swscale --enable-swresample --enable-decoder=bink,binkaudio_dct,binkaudio_rdft,mp3,vorbis,flac,pcm_s16le,pcm_u8,adpcm_ima_wav --enable-demuxer=bink,mp3,ogg,flac,wav --enable-parser=mpegaudio,vorbis,flac --enable-protocol=file --extra-cflags='-D__PROSPERO__ -fPIC'
# The native application provides mkstemp; configure probes cannot link its shim.
sed -i 's/^#define HAVE_MKSTEMP 0$/#define HAVE_MKSTEMP 1/; s/^#define HAVE_SYSCTL 1$/#define HAVE_SYSCTL 0/' config.h
make -j 3
make install
cmake -S "$root/deps/openal-soft-1.23.1" -B "$root/build/openal" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$root/cmake/ps5-native.cmake" -DCMAKE_INSTALL_PREFIX="$root/prefix" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DLIBTYPE=STATIC -DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF -DALSOFT_TESTS=OFF -DALSOFT_BACKEND_SDL2=ON -DALSOFT_REQUIRE_SDL2=ON -DALSOFT_BACKEND_OSS=OFF -DALSOFT_BACKEND_WAVE=OFF -DALSOFT_BACKEND_PIPEWIRE=OFF -DALSOFT_BACKEND_PULSEAUDIO=OFF -DALSOFT_BACKEND_JACK=OFF -DALSOFT_BACKEND_PORTAUDIO=OFF -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON -DHAVE_LIBATOMIC=OFF -DHAVE_PTHREAD_SETSCHEDPARAM=OFF -DHAVE_PTHREAD_SET_NAME_NP=OFF -DHAVE_PTHREAD_SETNAME_NP=OFF -DHAVE_LIBM=OFF -DHAVE_LIBRT=OFF -DALSOFT_RTKIT=OFF -DALSOFT_BACKEND_ALSA=OFF -DALSOFT_BACKEND_SNDIO=OFF
cmake --build "$root/build/openal" --target install -j 3
