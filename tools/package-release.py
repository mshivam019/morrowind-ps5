#!/usr/bin/env python3
"""Validate and assemble engine-only Windows/Linux native-title beta downloads."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TITLE = 'PPSA99630'
RUNTIME_SHA256 = 'e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def validate(title):
    required = ['eboot.bin', 'sce_sys/param.json', 'sce_sys/icon0.png',
                'sce_sys/pic0.dds', 'sce_sys/pic1.dds', 'sce_module/libc.prx',
                'assets/openmw.cfg', 'assets/settings.cfg', 'assets/defaults.bin',
                'assets/driver-build.json', 'assets/resources/version']
    for name in required:
        if not (title / name).is_file():
            raise ValueError(f'Missing required title file: {name}')
    if json.loads((title / 'sce_sys/param.json').read_text())['titleId'] != TITLE:
        raise ValueError('Unexpected title ID')
    if digest(title / 'sce_module/libc.prx') != RUNTIME_SHA256:
        raise ValueError('Runtime differs from the documented clean-room module')
    config = (title / 'assets/openmw.cfg').read_text()
    for value in ['content=Morrowind.esm', 'content=Tribunal.esm', 'content=Bloodmoon.esm',
                  'skip-menu=0', 'new-game=0',
                  'fallback=Movies_Morrowind_Logo,mw_logo.bik',
                  'fallback=Movies_New_Game,mw_intro.bik']:
        if value not in config.splitlines():
            raise ValueError(f'Unexpected beta configuration: missing {value}')
    if any(line.startswith('start=') for line in config.splitlines()):
        raise ValueError('Demo start cell override in beta configuration')
    if any('data=' in line and 'MET' in line for line in config.splitlines()):
        raise ValueError('HD texture root enabled')
    files = []
    for path in sorted(title.rglob('*')):
        relative = path.relative_to(title)
        if path.is_symlink():
            raise ValueError(f'Symbolic link in title: {relative}')
        if any(bit.lower() in {'data files', 'userdata', 'saves', '.git', '.deps'}
               for bit in relative.parts):
            raise ValueError(f'Private/game-data directory in title: {relative}')
        if not path.is_file():
            continue
        if path.suffix.lower() in {'.esm', '.esp', '.bsa', '.exe', '.dll', '.pkg', '.iso',
                                   '.key', '.pem', '.mkv', '.mp4', '.o', '.a', '.so'}:
            raise ValueError(f'Forbidden release file: {relative}')
        if relative.as_posix() == 'assets/hd-textures.json':
            raise ValueError('HD texture receipt in title')
        if relative.parts[0] not in {'assets', 'sce_sys', 'sce_module'} and relative.as_posix() != 'eboot.bin':
            raise ValueError(f'Unexpected title path: {relative}')
        if relative.parts[0] == 'sce_module' and relative.as_posix() != 'sce_module/libc.prx':
            raise ValueError(f'Unexpected runtime module: {relative}')
        files.append(path)
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--title', type=Path, required=True)
    parser.add_argument('--version', default='v0.1.0-beta.1')
    parser.add_argument('--out', type=Path, default=ROOT / 'build/releases')
    parser.add_argument('--check-only', action='store_true')
    args = parser.parse_args()
    if not re.fullmatch(r'v\d+\.\d+\.\d+(?:-beta\.\d+)?', args.version):
        parser.error('Version must be a semantic version or numbered beta')
    files = validate(args.title)
    manifest = {'title': TITLE, 'version': args.version,
                'files': [{'path': p.relative_to(args.title).as_posix(),
                           'bytes': p.stat().st_size, 'sha256': digest(p)} for p in files]}
    if args.check_only:
        print(f'Validated {len(files)} engine-only title files')
        return
    args.out.mkdir(parents=True, exist_ok=True)
    notices = [ROOT / 'LICENSE', ROOT / 'THIRD-PARTY-NOTICES.md']
    notices += sorted(p for p in (ROOT / 'LICENSES').rglob('*') if p.is_file())
    install = f'''Morrowind for PS5 — {args.version} (early beta)

1. Copy your own complete Morrowind + Tribunal + Bloodmoon Data Files folder to:
   output/PPSA99630/assets/Data Files/
2. Close the game. Upload output/PPSA99630 to your homebrew console, e.g.:
   /mnt/ext1/etaHEN/games/PPSA99630
3. Register/mount the folder with your native-title launcher and launch it.

Windows: powershell -ExecutionPolicy Bypass -File tools\\install.ps1 -Src .\\output\\PPSA99630 -Console <console-ip>
Linux/macOS: bash tools/install.sh --src output/PPSA99630 --console <console-ip>

The helpers upload only; registration/mounting is a separate step. Any FTP client works.
A homebrew-capable PS5 and compatible native-title loader are required.
Original game textures only. 1080p; approximately 30 FPS in the tested exterior scene.
Normal main menu and New Game are enabled. Full progression and save/load are unverified.
Preserve game files and /download0 sandbox data when updating; close the title first.

Source and corresponding-source archive:
https://github.com/mshivam019/morrowind-ps5/releases/tag/{args.version}
See README.md, docs/CONSOLE-SETUP.md, docs/VALIDATION.md and THIRD-PARTY-NOTICES.md.
No commercial game files, HD texture packs or proprietary SDK binaries are included.
'''
    for platform, helper in [('windows', 'install.ps1'), ('linux', 'install.sh')]:
        archive = args.out / f'morrowind-ps5-1080p-{args.version}-{platform}.zip'
        with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as z:
            for path in files:
                z.write(path, 'output/' + TITLE + '/' + path.relative_to(args.title).as_posix())
            for path in notices:
                z.write(path, path.relative_to(ROOT).as_posix())
            for name in ['README.md', 'docs/CONSOLE-SETUP.md', 'docs/VALIDATION.md']:
                z.write(ROOT / name, name)
            z.write(ROOT / 'tools' / helper, 'tools/' + helper)
            z.writestr('INSTALL.txt', install)
            z.writestr('MANIFEST.json', json.dumps(manifest, indent=2) + '\n')
        archive.with_suffix(archive.suffix + '.sha256').write_text(f'{digest(archive)}  {archive.name}\n')
        print(archive)


if __name__ == '__main__':
    main()
