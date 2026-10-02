# Release packaging

The beta ships the console-tested native folder title, not the development package root. Never archive the whole `build/package` directory: it includes compiler/tooling and SDK material.

```sh
python3 tools/package-release.py --title build/beta-main-menu/dist/PPSA99630 --version v0.1.0-beta.1
```

The packager validates the title metadata, required files and normal-start configuration, rejects game-data files, texture-pack receipts, unexpected runtime modules and symbolic links, and assembles Windows/Linux ZIPs with install helpers, instructions, license texts, third-party notices and a SHA-256 file inventory. It does not install or launch anything.

Both platform downloads contain identical engine payloads. No game data is bundled. Add your own complete Morrowind/Tribunal/Bloodmoon `Data Files` before installation.

Provide the corresponding-source archive with the beta: the cleaned port repository, actual OpenMW/dependency source snapshots and the PS5 graphics/native-app source used to construct the binary. Preserve original upstream notices. Exclude source-control directories, private notes, account data, game data, compiler binaries and compiled libraries. The public payload SDK is a separate prerequisite linked from BUILDING.md.

```sh
python3 tools/package-source.py --sdk-root /path/to/ps5sdk --version v0.1.0-beta.1
```

Stage the cleaned port files in Git before assembling source; the source packager uses the current index file list and current working-tree contents.

Compare the release executable and runtime SHA-256 values with the tested title before uploading. Mark the GitHub release as a **prerelease** and use the explicit beta tag link; GitHub’s `/releases/latest` may skip prereleases.
