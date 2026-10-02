#!/usr/bin/env python3
"""Safely extract MET textures into an isolated, ignored directory; never vanilla data."""
import argparse
from collections import Counter
import json
from pathlib import Path
import shutil
import stat
import zipfile
from texture_pack import ROOT, inspect_dds, safe_relative, sha256, load_manifest, add_memory_accounting


def stage_archive(archive, destination):
    archive, destination = Path(archive).resolve(), Path(destination).resolve()
    if not destination.is_relative_to(ROOT / 'game-mods') or destination == ROOT / 'game-mods':
        raise ValueError('Destination must be a separate directory under project game-mods/')
    if destination.exists():
        result = load_manifest(destination, verify=True)
        if result['archive_sha256'] != sha256(archive):
            raise ValueError('Existing stage belongs to a different archive; use a new destination')
        return result
    temporary = destination.with_name(destination.name + '.staging')
    if temporary.exists():
        raise ValueError(f'Interrupted staging directory exists; inspect/remove it first: {temporary}')
    entries = []
    seen = set()
    with zipfile.ZipFile(archive) as z:
        # Validate every member before writing any files; CRC checked on extraction.
        for member in z.infolist():
            name = member.filename.rstrip('/')
            if not name or not name.startswith('MET 6-1 main/') and name != 'MET 6-1 main':
                raise ValueError('Unexpected archive root: ' + name)
            if stat.S_ISLNK(member.external_attr >> 16):
                raise ValueError('Archive symlink is forbidden: ' + name)
            if member.is_dir():
                if '..' in name.split('/') or '\\' in name:
                    raise ValueError('Unsafe directory: ' + name)
                continue
            # Windows folder metadata is not a game asset. Exact known member only.
            if name == 'MET 6-1 main/textures/gherb/desktop.ini':
                continue
            relative = safe_relative(name[len('MET 6-1 main/'):])
            if relative.casefold() in seen:
                raise ValueError('Duplicate case-insensitive archive path: ' + relative)
            seen.add(relative.casefold())
            entries.append((member, relative))
        if not entries or sum(m.file_size for m, _ in entries) > 8 * 1024**3:
            raise ValueError('Empty or unexpectedly large texture archive')
        needed = sum(m.file_size for m, _ in entries)
        destination.parent.mkdir(parents=True, exist_ok=True)
        if shutil.disk_usage(destination.parent).free < needed + 256 * 1024**2:
            raise ValueError('Insufficient disk space for texture stage')
        temporary.mkdir()
        files = []
        try:
            for index, (member, relative) in enumerate(entries, 1):
                target = temporary / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                with z.open(member) as stream, target.open('xb') as output:
                    shutil.copyfileobj(stream, output, 1024 * 1024)
                with target.open('rb') as stream:
                    dds = inspect_dds(stream.read(128), target.stat().st_size)
                files.append(dict(path=relative, bytes=target.stat().st_size,
                                  sha256=sha256(target), dds=dds))
                if index % 500 == 0:
                    print(f'Staged and checked {index}/{len(entries)} DDS textures', flush=True)
            manifest = dict(schema=1, pack="MET6.1", archive_name=archive.name,
                            archive_sha256=sha256(archive), total_bytes=needed,
                            files=sorted(files, key=lambda item: item['path']))
            add_memory_accounting(manifest)
            (temporary / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
            temporary.rename(destination)
        except BaseException:
            shutil.rmtree(temporary)
            raise
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--out', type=Path, default=ROOT / "game-mods/MET6.1")
    args = parser.parse_args()
    manifest = stage_archive(args.archive, args.out)
    print(json.dumps(dict(files=len(manifest['files']), bytes=manifest['total_bytes'],
                          formats=dict(Counter(x['dds']['format'] for x in manifest['files'])),
                          manifest=str(args.out / 'manifest.json')), indent=2))


if __name__ == '__main__':
    main()
