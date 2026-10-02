#!/usr/bin/env python3
"""Compile real legacy and generic NIR through the patched host NIR/ACO backend."""
from pathlib import Path
import os
import subprocess

project = Path(__file__).resolve().parents[2]
source = Path(os.environ.get('PS5_OPENGL_ROOT', str(Path.home() / 'ps5sdk/ps5-opengl-030/ps5-opengl'))) / 'third_party/opengnm-psbc'
subprocess.run(['python3', project / 'tools/build-fixed-function-psbc.py', '--host'], check=True)
output = project / 'build/fixed-function-psbc'
flags = ['-std=gnu11', '-DHAVE_ENDIAN_H=1', '-DHAVE_FUNC_ATTRIBUTE_PACKED=1',
         '-DHAVE_PTHREAD=1', '-DHAVE_STRUCT_TIMESPEC=1', '-D_GNU_SOURCE']
for directory in ['include/mesa', 'include', 'src', 'libpsbc']:
    flags += ['-I', str(source / directory)]
subprocess.run(['cc', *flags, '-c', project / 'tests/driver/legacy-varying-nir.c',
                '-o', output / 'legacy-smoke.o'], check=True)
subprocess.run(['g++', '-o', output / 'legacy-smoke', output / 'legacy-smoke.o',
                output / 'libpsbc-host.a', '-pthread', '-lm'], check=True)
subprocess.run([output / 'legacy-smoke'], check=True, timeout=30)
