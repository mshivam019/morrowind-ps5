# Morrowind: PS5 native port

A native PS5 port of [OpenMW 0.51.0](https://github.com/OpenMW/openmw), the open-source Morrowind engine, using the PS5 OpenGL runtime. Companion project to [Ocarina of Time](https://github.com/mshivam019/oot64-ps5) and [Majora’s Mask](https://github.com/mshivam019/tmm64-ps5) for PS5.

[Download the beta](https://github.com/mshivam019/morrowind-ps5/releases/tag/v0.1.0-beta.1) · [Console setup](docs/CONSOLE-SETUP.md) · [Build from source](docs/BUILDING.md)

## Features

- Native homebrew title **Morrowind - OpenMW** (`PPSA99630`), launched from the PS5 home screen.
- 1920 × 1080 rendering; approximately 30 FPS in the measured stationary exterior scene. Performance varies by location.
- DualSense movement, camera controls, interaction and gamepad menus.
- Inventory and audio work in the tested demo.
- Native submission batching and asynchronous batch preparation through the patched PS5 OpenGL driver.
- Original game textures by default.

**This is an early beta.** It opens the normal main menu; New Game uses Morrowind’s original opening and character-creation scripts. This is an early release for testing and contributions. Stable 60 FPS is not established. See [validation and known limits](docs/VALIDATION.md).

## Requirements

- Homebrew-capable PS5 with native folder-title support, an active FTP server and a compatible title registration/mounting setup.
- Your own PC installation of **Morrowind, Tribunal and Bloodmoon**. The beta configuration enables all three master files and archives.
- An M.2 installation path for the title and enough space for the engine plus your complete `Data Files` folder.
- Any FTP client, such as FileZilla or WinSCP. The optional helpers use PowerShell/curl.exe on Windows or Bash/curl on Linux and macOS.

## Installation

1. Download the Windows or Linux ZIP from the [beta release](https://github.com/mshivam019/morrowind-ps5/releases/tag/v0.1.0-beta.1) and extract it. Both contain the same PS5 executable; macOS users can use the Linux archive.
2. Copy your complete PC `Data Files` folder into `output/PPSA99630/assets/Data Files/`. Include `Morrowind.esm`, `Tribunal.esm`, `Bloodmoon.esm`, their `.bsa` archives and loose game files. Game data is not included in the download.
3. Close any running copy of the game. Upload `output/PPSA99630` to your console, for example `/mnt/ext1/etaHEN/games/PPSA99630`.
4. Register/mount that folder using your native-title launcher, then start **Morrowind - OpenMW** from the home screen. FTP upload alone does not register the title.

**Windows** (optional upload helper):

```powershell
powershell -ExecutionPolicy Bypass -File tools\install.ps1 -Src .\output\PPSA99630 -Console <console-ip>
```

**Linux/macOS** (optional upload helper):

```sh
bash tools/install.sh --src output/PPSA99630 --console <console-ip>
```

Use `-FtpPort` / `--ftp-port` or `-InstallRoot` / `--install-root` to match your setup. The folder name `etaHEN` alone does not establish a loader requirement. Read [console setup](docs/CONSOLE-SETUP.md) and the archive’s `INSTALL.txt` before updating.

## Validation and known limits

The packaged build booted on the developer’s PS5. Movement, turning, world rendering, water, characters and menus were visually checked; the latest video also shows home-screen launch, inventory and interaction. The measured stationary outdoor rate remains approximately 30 FPS. Earlier interior/exterior transition tests produced slower outdoor views. Recording at 60 FPS is not evidence of 60 FPS gameplay.

HD textures are disabled after a console test produced flickering. Use original textures for this beta. Performance and compatibility may vary across firmware, loaders and game locations.

Configuration, saves and cache use the title’s `/download0` sandbox. Preserve that data when updating and do not overwrite a running title. The beta opens the normal main menu.

## Building and layout

This repository contains the port’s patches, platform code, configuration, tests and build/installation scripts. Upstream engines and libraries are external dependencies. [Building](docs/BUILDING.md) describes the dependency layout and the release’s corresponding-source archive. [Releasing](docs/RELEASING.md) describes package validation and assembly.

```text
patches/          OpenMW, OSG, PSBC and graphics-driver changes
source-overlays/  PS5 paths and SDL integration
platform/        native runtime compatibility shims
cmake/, config/  toolchain and runtime settings
art/             original launcher icon
tools/, tests/   build, installation and regression checks
docs/, LICENSES/ guides, validation and upstream notices
```

## Credits and license

- [OpenMW](https://openmw.org/) and its contributors.
- [BlackBearReloaded](https://github.com/blackbearreloaded): [ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl), [native-app boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) and PS5 SDL integration.
- [ps5-payload-dev](https://github.com/ps5-payload-dev): public homebrew SDK and SDL port.
- Mesa, OpenGNM PSBC, OpenSceneGraph, MyGUI and the other engine dependencies listed in [third-party notices](THIRD-PARTY-NOTICES.md).
- Mihawk for the attributed native shim contribution.

The port’s own code, scripts and patches use [GPL-3.0-or-later](LICENSE); upstream components retain their own licenses and notices. Game data, game installers, saves, texture packs and proprietary SDK binaries are not included.

This project is not affiliated with Bethesda or Sony.
