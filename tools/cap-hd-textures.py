#!/usr/bin/env python3
"""Stage separate 2K MET profile by preserving supplied DDS mip levels, without re-encoding."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
from texture_pack import ROOT, cap_dds, inspect_dds, load_manifest, add_memory_accounting


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT/'game-mods/MET6.1')
    parser.add_argument('--out', type=Path, default=ROOT/'game-mods/MET6.1-2K')
    args = parser.parse_args()
    source, out = args.source.resolve(), args.out.resolve()
    if not out.is_relative_to(ROOT/'game-mods') or out == source or out == ROOT/'game-mods':
        parser.error('output must be a separate directory under game-mods/')
    if out.exists():
        parser.error('output already exists; verify it with transfer-hd-textures.py --stage instead')
    manifest = load_manifest(source, verify=True)
    temporary = out.with_name(out.name+'.staging')
    if temporary.exists():
        parser.error('interrupted stage exists: '+str(temporary))
    temporary.mkdir(parents=True)
    files = []
    changed = 0
    try:
        for index,item in enumerate(manifest['files'],1):
            blob, skip = cap_dds((source/item['path']).read_bytes())
            path = temporary/item['path'];path.parent.mkdir(parents=True,exist_ok=True)
            path.write_bytes(blob)
            files.append(dict(path=item['path'],bytes=len(blob),sha256=hashlib.sha256(blob).hexdigest(),
                              dds=inspect_dds(blob[:128],len(blob)),source_sha256=item['sha256'],dropped_mips=skip))
            changed += skip > 0
            if index%500 == 0:print(f'Capped/checked {index}/{len(manifest["files"])}',flush=True)
        result=dict(schema=1,pack='MET6.1-2K',profile='maximum dimension 2048, supplied mip levels only',
                    archive_name=manifest['archive_name'],archive_sha256=manifest['archive_sha256'],
                    total_bytes=sum(x['bytes'] for x in files),files=files)
        add_memory_accounting(result)
        (temporary/'manifest.json').write_text(json.dumps(result,indent=2)+'\n');temporary.rename(out)
    except BaseException:
        shutil.rmtree(temporary);raise
    print(json.dumps(dict(files=len(files),capped=changed,bytes=result['total_bytes'],out=str(out)),indent=2))


if __name__ == '__main__':main()
