#!/usr/bin/env python3
"""Build the linear mip batching candidate without changing the active driver."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess

project = Path(__file__).resolve().parent.parent
sdk = Path(os.environ.get('PS5SDK_ROOT', str(Path.home() / 'ps5sdk')))
gl = Path(os.environ.get('PS5_OPENGL_ROOT', sdk / 'ps5-opengl-030/ps5-opengl'))
prefix = Path(os.environ.get('PS5_OPENGL_SDK', sdk / 'extracted/ps5-opengl-sdk-0.3.0/sdk'))
config = Path(os.environ.get('PS5_RUNTIME_CONFIG', prefix.parent / 'runtime-config.txt'))
output = project / 'build/mip-batching-driver'
output.mkdir(parents=True, exist_ok=True)
source = project / 'patches/ps5-mip-batching/ps5_screen.c'
lines = config.read_text().splitlines()
flags = [flag.replace('/home/runner/work/ps5-opengl/ps5-opengl', str(gl))
         for line in lines[1:] for flag in shlex.split(line)]
flags = ['-O2' if flag == '-Os' else flag for flag in flags]
command = [str(sdk / 'native-app-boilerplate/tooling/prospero-clang18'), *flags,
           '-DPS5_RUNTIME_QUIET=1', '-I' + str(gl / 'src/platform'),
           '-c', str(source), '-o', str(output / 'ps5_screen.o')]
env = os.environ.copy()
env['PS5_PAYLOAD_SDK'] = str(sdk / 'native-app-boilerplate/.deps/native/ps5-payload-sdk')
subprocess.run(command, env=env, check=True)
archive = output / 'libps5_opengl_core33.a'
archive.parent.mkdir(parents=True, exist_ok=True)
shutil.copy2(project / 'driver/lib/libps5_opengl_core33.a', archive)
subprocess.run(['llvm-ar', 'r', archive, output / 'ps5_screen.o'], check=True)
subprocess.run(['llvm-ranlib', archive], check=True)
print(archive)
