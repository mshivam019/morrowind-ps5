# Morrowind: PS5 native port

**Installed from ProsperoStore?** [Use the game-specific setup](INSTALL.md#already-installed-from-prosperostore): choose your game files and an output location; it prepares and checks a ready-to-copy folder automatically. Download **PS5-Install-Helper-v1.2.0.zip** from this game's release. Python 3.9+ is required; game conversion/module preparation requirements are listed in the guide.

**[Start here: installation, USB/offline copying, Wi-Fi FTP and game-file locations](INSTALL.md).**
M.2, Ethernet and PS5 Upload are optional. The guide also explains recovery after
store updates; older packaged instructions may describe the developer's M.2 setup.

A native PS5 port of [OpenMW 0.51.0](https://github.com/OpenMW/openmw), the open-source Morrowind engine, using the PS5 OpenGL runtime. Companion project to [Ocarina of Time](https://github.com/mshivam019/oot64-ps5) and [Majora’s Mask](https://github.com/mshivam019/tmm64-ps5) for PS5.

[Download v1.0.1](https://github.com/mshivam019/morrowind-ps5/releases/tag/v1.0.1) · [Console setup](docs/CONSOLE-SETUP.md) · [Build from source](docs/BUILDING.md)

## Features

- Native homebrew title **Morrowind - OpenMW** (`PPSA99630`), launched from the PS5 home screen.
- 1920 × 1080 rendering; approximately 30 FPS in the measured stationary exterior scene. Performance varies by location.
- DualSense movement, camera controls, interaction and gamepad menus.
- Sony native on-screen keyboard for character names.
- Controller appearance selection, inventory, audio, and basic save/load.
- Larger dialogs and text with a 150% interface scale.
- Native submission batching and asynchronous batch preparation through the patched PS5 OpenGL driver.
- Original game textures by default.

**v1.0.1 is the first stable release.** New Game uses the original opening and character-creation flow, with Sony's native keyboard for name entry. The interface is scaled to 150% for TV use. Tested on PS5 firmware 9.00 with kstuff and ShadowMount. See [validation and known limits](docs/VALIDATION.md).

## Requirements

- Homebrew-capable PS5 with native folder-title support, a compatible title registration/mounting setup and a file-copy method.
- Your own PC installation of **Morrowind, Tribunal and Bloodmoon**. The release configuration enables all three master files and archives.
- A loader-supported installation location and enough space for the engine plus your complete `Data Files` folder.
- A console file manager for USB copying, or any FTP client such as FileZilla or WinSCP (Wi-Fi or Ethernet). The optional common helpers use Python 3.9+ on Windows/Linux/macOS; manual copying needs no Python.

## Installation

Install `PPSA99630.zip` from ProsperoStore/the release, or unpack the complete
precompiled title folder. Add your complete **Data Files** folder under
`PPSA99630/assets/Data Files/`, including Morrowind, Tribunal and Bloodmoon master
files, archives and loose files. Close the game before updating its files.

Copy using USB plus your console file manager, Wi-Fi FTP, Ethernet FTP or your
preferred tool. Use the destination selected by your loader/store; M.2 and PS5
Upload are optional. A new manual title still needs your loader to mount/register it.

[INSTALL.md](INSTALL.md) gives exact layouts, the separate optional prepare and
transfer helpers, and recovery after store updates. The release's Windows/Linux
ZIPs contain the same PS5 app. No compilation or conversion is needed for Data Files.

## Validation and known limits

Console testing confirmed home-screen launch, the original opening, native keyboard name entry, appearance controls, movement, camera controls, inventory, interaction, audio, and basic save/load. The user also confirmed the larger dialogs and normal visuals with the optional Morrowind Optimization Patch.

The measured stationary outdoor rate remains approximately 30 FPS. Performance varies by location; a 60 FPS recording or engine cap does not establish 60 FPS gameplay. A complete playthrough and other firmware combinations have not been validated.

HD textures are disabled after a console test produced flickering. Use original textures. Performance and compatibility may vary across firmware, loaders and game locations.

Configuration, saves and cache use the title’s `/download0` sandbox. Preserve that data when updating and do not overwrite a running title. The release opens the normal main menu.

## Controller menus

The left stick moves the pointer. Cross on DualSense or A on Redgear selects; Circle or B goes back. L1/R1 switch the main Options tabs. Inside Controls, point at **Controller** with the left stick and press Cross/A to switch from Mouse and Keyboard.

## Optional optimization mod

[Morrowind Optimization Patch](https://www.nexusmods.com/morrowind/mods/45384) reduces mesh complexity. Its 1.18 core files were tested with this build. It is not included and does not guarantee 60 FPS. See [installation instructions](docs/CONSOLE-SETUP.md#optional-morrowind-optimization-patch).

## Building and layout

This repository contains the port’s patches, platform code, configuration, tests and build/installation scripts. Upstream engines and libraries are external dependencies. [Building](docs/BUILDING.md) describes the dependency layout and the release’s corresponding-source archive. [Releasing](docs/RELEASING.md) describes package validation and assembly.

```text
patches/          OpenMW, OSG, PSBC and graphics-driver changes
source-overlays/  PS5 paths and SDL integration
platform/        native runtime compatibility shims
cmake/, config/  toolchain and runtime settings
art/             launcher icon and selection/launch artwork
tools/, tests/   build, installation and regression checks
docs/, LICENSES/ guides, validation and upstream notices
```

## Credits and license

- [OpenMW](https://openmw.org/) and its contributors.
- [BlackBearReloaded](https://github.com/blackbearreloaded): [ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl), [native-app boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) and PS5 SDL integration.
- [ps5-payload-dev](https://github.com/ps5-payload-dev): public homebrew SDK and SDL port.
- Mesa, OpenGNM PSBC, OpenSceneGraph, MyGUI and the other engine dependencies listed in [third-party notices](THIRD-PARTY-NOTICES.md).
- [Mihawk](https://github.com/mihawk-99) for the attributed native shim contribution.

The port’s own code, scripts and patches use [GPL-3.0-or-later](LICENSE); upstream components retain their own licenses and notices. Game data, game installers, saves, texture packs and proprietary SDK binaries are not included.

This project is not affiliated with Bethesda or Sony.
