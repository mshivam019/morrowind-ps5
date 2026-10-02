# Third-party notices

The port’s own additions are Copyright (C) 2026 Shivam Mishra, GPL-3.0-or-later. Existing source headers and upstream copyrights remain intact. This statement does not relicense upstream components.

| Component | Source / version | Notice |
| --- | --- | --- |
| OpenMW | [OpenMW](https://github.com/OpenMW/openmw), 0.51.0 pinned in sources.lock.json | GPL-3.0; LICENSES/OpenMW.txt; per-file/extern notices in corresponding source |
| PS5 OpenGL, Mesa/ACO/NIR, OpenGNM PSBC | [BlackBearReloaded/ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl), SDK 0.3.0 and port modifications | GPL-3.0-or-later for project-owned code; Mesa/PSBC component licenses in LICENSES/ps5-opengl and source headers |
| Native app boilerplate and clean-room libc.prx | [BlackBearReloaded](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) | GPL-3.0-or-later; LICENSES/native-app-notices.txt and runtime-shim-notice.txt |
| PS5 SDL integration / SDL 2.30.12 | [ps5-payload-dev/SDL](https://github.com/ps5-payload-dev/SDL), BlackBearReloaded integration and port joystick changes | SDL zlib license; LICENSES/SDL2.txt; integration GPL-3.0-or-later headers |
| OpenSceneGraph / OpenThreads | Pinned in graphics-sources.lock.json | OpenSceneGraph Public License and per-file notices; LICENSES/OpenSceneGraph.txt |
| MyGUI | Pinned in graphics-sources.lock.json | MIT; LICENSES/MyGUI.txt |
| Bullet | Pinned in sources.lock.json | zlib; LICENSES/Bullet.txt |
| LZ4 | Pinned in sources.lock.json | BSD-2-Clause library; LICENSES/LZ4.txt |
| RecastNavigation | Pinned in sources.lock.json | zlib; LICENSES/RecastNavigation.txt |
| Boost 1.85.0 | [Boost](https://www.boost.org/) | Boost Software License 1.0; LICENSES/Boost.txt |
| Lua 5.4.7 | [Lua](https://www.lua.org/) | MIT; LICENSES/Lua.txt |
| SQLite 3.47.2 | [SQLite](https://sqlite.org/) | Public-domain dedication; LICENSES/SQLite.txt |
| ICU 76.1 | [Unicode ICU](https://github.com/unicode-org/icu) | Unicode and component notices; LICENSES/ICU.txt |
| yaml-cpp 0.8.0 | [yaml-cpp](https://github.com/jbeder/yaml-cpp) | MIT; LICENSES/yaml-cpp.txt |
| FreeType | Pinned in graphics-sources.lock.json | FreeType License; LICENSES/FreeType.txt and FreeType-FTL.txt |
| libpng | Pinned in graphics-sources.lock.json | libpng license; LICENSES/libpng.txt |
| libjpeg-turbo | Pinned in graphics-sources.lock.json | IJG/BSD/zlib as applicable; LICENSES/libjpeg-turbo.txt and libjpeg-turbo-IJG.txt |
| zlib 1.3.1 | [zlib](https://zlib.net/) | zlib license; LICENSES/zlib.txt |
| FFmpeg 6.1.3 | [FFmpeg](https://ffmpeg.org/) | LGPL configuration; LICENSES/FFmpeg.txt, FFmpeg-LGPL-2.1.txt and FFmpeg-LGPL-3.txt |
| OpenAL Soft 1.23.1 | [OpenAL Soft](https://github.com/kcat/openal-soft) | LGPL; LICENSES/OpenAL-Soft.txt |
| LLVM libc++, libc++abi, libunwind, compiler builtins | Public payload SDK / Clang toolchain | Apache-2.0 with LLVM exceptions and retained component notices; LICENSES/LLVM.txt |
| OpenMW bundled extern and engine resources | Included in the OpenMW corresponding-source tree | Retained per-file licenses and attribution in OpenMW source |

This software is based in part on the work of the Independent JPEG Group. Portions of this software are copyright © the FreeType Project. Original upstream notices are included in the corresponding-source archive; the root LICENSES directory is a convenient collection, not a replacement for per-file notices.

## PS5 runtime and artwork

The packaged libc.prx matches the native-app boilerplate’s published clean-room runtime hash: `e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036`. It is independently authored homebrew code, not a Sony firmware module. System import libraries are link-time inputs; Sony system implementations are not bundled.

The launcher icon is the port’s original text-based SVG/PNG. Backgrounds come from the native-app boilerplate’s original GPL-3.0-or-later presentation assets, Copyright (C) 2026 BlackBearReloaded. The template’s preview audio is omitted. Proprietary game data, commercial installers and third-party HD texture packs are not distributed.

Platform shim code retains Mihawk’s attributed GPL-3.0-or-later contribution. PS5 graphics and SDL source copies retain BlackBearReloaded’s original headers; port modifications are supplied in patches and overlays.

## Corresponding source

The beta release provides a source archive alongside the binary ZIPs, with the cleaned port, current engine/dependency source trees and graphics/native-app build sources. The public [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk/releases/tag/v0.42) and host compiler are separate prerequisites. Build instructions describe the source layout and the limits of fresh-clone reproduction. No proprietary SDK is distributed.
