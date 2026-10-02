#!/usr/bin/env python3
"""Build the bounded initial marker-spin candidate without replacing the active driver."""
import os
from pathlib import Path
import shlex
import subprocess
import shutil

project = Path(__file__).resolve().parent.parent
sdk = Path(os.environ.get('PS5SDK_ROOT', str(Path.home() / 'ps5sdk')))
gl = Path(os.environ.get('PS5_OPENGL_ROOT', sdk / 'ps5-opengl-030/ps5-opengl'))
prefix = Path(os.environ.get('PS5_OPENGL_SDK', sdk / 'extracted/ps5-opengl-sdk-0.3.0/sdk'))
config = Path(os.environ.get('PS5_RUNTIME_CONFIG', prefix.parent / 'runtime-config.txt'))
output = project / 'build/poll-spin-candidate'
output.mkdir(parents=True, exist_ok=True)
platform = output / 'platform'
platform.mkdir(exist_ok=True)
shutil.copy2(project / 'patches/ps5-draw-batching/platform/ps5_agc_runtime_backend.c', platform)
for name in ('ps5_agc_native_runtime.c', 'ps5_agc_poll.h'):
    shutil.copy2(project / 'patches/ps5-poll-spin' / name, platform)
shutil.copy2(project / 'patches/ps5-draw-batching/platform/ps5_agc_profile.h', platform)

lines = config.read_text().splitlines()
flags = [flag.replace('/home/runner/work/ps5-opengl/ps5-opengl', str(gl))
         for line in [lines[1], lines[4]] for flag in shlex.split(line)]
flags = ['-O2' if flag == '-Os' else flag for flag in flags]
command = [str(sdk / 'native-app-boilerplate/tooling/prospero-clang18'), *flags,
           '-I' + str(gl / 'third_party/opengnm-psbc/libpsbc'),
           '-I' + str(gl / 'src/platform'), '-Wno-error=unused-function',
           '-DPS5_DRAW_PROFILE=1','-Dmain=ps5_agc_gate2_run','-DAGC_TRIANGLE_SUBMIT=1','-DAGC_RUNTIME_PACKAGES=1',
           '-c',str(platform / 'ps5_agc_runtime_backend.c'),
           '-o',str(output / 'ps5_agc_runtime_backend.o')]
env = os.environ.copy()
env['PS5_PAYLOAD_SDK'] = str(sdk / 'native-app-boilerplate/.deps/native/ps5-payload-sdk')
subprocess.run(command, env=env, check=True)
archive = output / 'libps5_opengl_core33.a'
shutil.copy2(project / 'driver/lib/libps5_opengl_core33.a', archive)
assert archive.exists(), 'Build draw-batching screen archive first'
subprocess.run(['llvm-ar','r',archive,output / 'ps5_agc_runtime_backend.o'],check=True)
subprocess.run(['llvm-ranlib',archive],check=True)
print(archive)
