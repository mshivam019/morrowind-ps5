#!/usr/bin/env python3
"""Assemble corresponding source without build products or private workspace data."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
EXCLUDED_DIRS = {'.git', '.svn', '__pycache__', 'CMakeFiles', 'downloads',
                 'autom4te.cache', '.deps', '.venv', 'node_modules'}
EXCLUDED_SUFFIXES = {'.o', '.obj', '.a', '.so', '.dll', '.exe', '.elf', '.prx',
                     '.pkg', '.iso', '.7z', '.zip', '.tar', '.gz', '.xz', '.bz2',
                     '.pyc', '.orig', '.rej', '.log', '.mkv', '.mp4', '.esm', '.esp', '.bsa', '.key', '.pem', '.bundle'}


def source_files(path):
    for current, dirs, files in os.walk(path):
        dirs[:] = sorted(d for d in dirs if d not in EXCLUDED_DIRS
                         and not (Path(current) / d).is_symlink())
        for name in sorted(files):
            file = Path(current) / name
            if file.is_symlink() or file.suffix.lower() in EXCLUDED_SUFFIXES:
                continue
            if name.startswith('.env') or name in {'.git', '.DS_Store', '.directory', 'CMakeCache.txt', 'build.ninja', '.ninja_log', '.ninja_deps'}:
                continue
            with file.open('rb') as stream:
                magic = stream.read(8)
            if magic.startswith((b'\x7fELF', b'!<arch>\n', b'MZ', b'\x4f\x15\x3d\x1d')):
                continue
            yield file


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk-root', type=Path, default=Path(os.environ.get('PS5SDK_ROOT', Path.home() / 'ps5sdk')))
    parser.add_argument('--version', default='v1.0.1')
    parser.add_argument('--out', type=Path, default=ROOT / 'build/releases')
    args = parser.parse_args()
    if not re.fullmatch(r'v\d+\.\d+\.\d+(?:-beta\.\d+)?', args.version):
        parser.error('Invalid version')
    prefix = f'morrowind-ps5-{args.version}-source'
    sdk = args.sdk_root
    gl = sdk / 'ps5-opengl-030/ps5-opengl'
    mappings = [(ROOT / 'openmw', 'port/openmw'), (ROOT / 'deps', 'port/deps'),
                (sdk / 'deps/zlib', 'ps5sdk/deps/zlib')]
    native = sdk / 'native-app-boilerplate'
    for name in ['tooling', 'tools', 'src', 'runtime', 'sce_sys', 'docs', 'tests']:
        mappings.append((native / name, f'ps5sdk/native-app-boilerplate/{name}'))
    for name in ['src', 'shaders', 'toolchain', 'tools', 'integration', 'native-app',
                 'third_party', 'LICENSES', 'docs']:
        mappings.append((gl / name, f'ps5sdk/ps5-opengl-030/ps5-opengl/{name}'))
    for source, _ in mappings:
        if not source.is_dir():
            raise ValueError(f'Missing corresponding-source tree: {source}')
    args.out.mkdir(parents=True, exist_ok=True)
    archive = args.out / (prefix + '.tar.gz')
    manifest = []
    with tarfile.open(archive, 'w:gz', compresslevel=6) as tar:
        def add(file, relative):
            tar.add(file, arcname=prefix + '/' + relative, recursive=False)
            with file.open('rb') as stream:
                digest = hashlib.file_digest(stream, 'sha256').hexdigest()
            manifest.append({'path': relative, 'bytes': file.stat().st_size, 'sha256': digest})
        tracked = subprocess.check_output(['git', '-C', str(ROOT), 'ls-files', '-z']).decode().split('\0')
        for name in tracked:
            if name and (ROOT / name).is_file():
                add(ROOT / name, 'port/' + name)
        for source, destination in mappings:
            for file in source_files(source):
                add(file, destination + '/' + file.relative_to(source).as_posix())
        for source, destination in [(native, 'ps5sdk/native-app-boilerplate'),
                                    (gl, 'ps5sdk/ps5-opengl-030/ps5-opengl')]:
            for file in source_files(source):
                if file.parent == source:
                    add(file, destination + '/' + file.name)
        # Required generated compiler/GL headers; compiled archives remain excluded.
        for generated in [gl / 'build/mesa-ps5-probe']:
            if generated.is_dir():
                for file in source_files(generated):
                    if file.suffix in {'.h', '.inc'}:
                        add(file, 'ps5sdk/ps5-opengl-030/ps5-opengl/' + file.relative_to(gl).as_posix())
    inventory = args.out / (prefix + '-manifest.json')
    inventory.write_text(json.dumps({'version': args.version, 'files': manifest}, indent=2) + '\n')
    with archive.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    archive.with_suffix(archive.suffix + '.sha256').write_text(f'{digest}  {archive.name}\n')
    print(f'{archive}: {len(manifest)} source files')


if __name__ == '__main__':
    main()
