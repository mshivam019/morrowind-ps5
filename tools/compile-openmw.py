#!/usr/bin/env python3
"""Build engine objects/static archives; native PS5 linking is a separate step."""
from pathlib import Path
import os, subprocess
root=Path(__file__).resolve().parents[1]
build=root/'build/openmw'
env=os.environ.copy()
env.setdefault('PS5_PAYLOAD_SDK',str(Path(os.environ.get('PS5SDK_ROOT', Path.home() / 'ps5sdk')) / 'native-app-boilerplate/.deps/native/ps5-payload-sdk'))
listing=subprocess.check_output(['ninja','-C',str(build),'-t','targets','all'],text=True)
targets=[]
for line in listing.splitlines():
    target, _, rule=line.rpartition(': ')
    if ('COMPILER__' in rule and target.endswith(('.o','.obj'))) or ('STATIC_LIBRARY_LINKER__' in rule):
        targets.append(target)
if not targets: raise SystemExit('No compilation targets; run tools/configure.sh first')
print(f'Building {len(targets)} PS5 object/archive targets',flush=True)
subprocess.run(['ninja','-C',str(build),'-j',os.environ.get('OPENMW_BUILD_JOBS','3'),*targets],env=env,check=True)
