# Console setup

The beta is a native folder title, not a retail PS5 package. It requires a homebrew-capable console and a setup that registers and mounts folder titles.

The developer’s tested deployment uses an FTP server on port 2121, PS5Upload control on port 9114, and `/mnt/ext1/etaHEN/games/PPSA99630` on M.2. Firmware and loader combinations beyond this setup have not been validated for this beta. Do not infer compatibility from the Zelda ports’ test matrix.

The Windows/Linux install helpers only upload files. Register/mount the resulting title through your usual launcher. The development `tools/deploy.py` additionally uses PS5Upload control and expects the native-title services to be available; set `PS5_HOST` explicitly before using it.

## Game data

Copy the complete `Data Files` directory from your own PC installation of Morrowind plus Tribunal and Bloodmoon to `PPSA99630/assets/Data Files`. Preserve its directory structure. The engine configuration loads all three `.esm` files and `.bsa` archives. The release contains engine resources only; it cannot run without your game files.

Do not enable MET or other HD replacements for this beta. HD textures caused flickering in the console test and are disabled by default.

## Updating and user data

Close the running title before uploading. Keep your existing `assets/Data Files` and preserve the title’s writable sandbox. User configuration is under `/download0/config`, saves/user data under `/download0/user`, and cache under `/download0/cache` as seen by the application. These are application mount paths, not a promise of a particular host-visible directory on every loader.

The beta opens the normal main menu. New Game follows the original opening and character-creation scripts. Back up sandbox data before experimenting with configuration or changing loaders.

## Reporting problems

Include your firmware, loader/version, installation path, original or modified textures, location and reproduction steps. For performance issues, distinguish display/recording refresh rate from game frame rate. Inspect logs for account information before posting them.
