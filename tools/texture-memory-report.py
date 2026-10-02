#!/usr/bin/env python3
"""Report the current DDS fallback residency model offline; optionally record it in manifest."""
import argparse
import json
from pathlib import Path
from texture_pack import DEFAULT_STAGE, add_memory_accounting, load_manifest


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--stage',type=Path,default=DEFAULT_STAGE)
    p.add_argument('--write-manifest',action='store_true')
    p.add_argument('--budget-mib',type=int,default=2048,
                   help='comparison budget only; other engine resources also consume this memory')
    a=p.parse_args()
    if a.budget_mib<1:p.error('budget must be positive')
    m=add_memory_accounting(load_manifest(a.stage))
    if a.write_manifest:
        target=a.stage/'manifest.json';temporary=target.with_suffix('.json.next')
        temporary.write_text(json.dumps(m,indent=2)+'\n');temporary.replace(target)
    model=m['memory_model']
    largest=model['largest_texture']['modeled_resident_bytes']
    print(json.dumps(dict(pack=m['pack'],**model,comparison_budget_bytes=a.budget_mib*1048576,
                         largest_texture_copies_in_budget=(a.budget_mib*1048576)//largest,
                         budget_note='Arithmetic ceiling only, not a safe scene capacity; excludes other resources'),indent=2))


if __name__=='__main__':main()
