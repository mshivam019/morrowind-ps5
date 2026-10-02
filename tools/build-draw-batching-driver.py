#!/usr/bin/env python3
"""Replace only ps5_screen.o in an isolated SDK runtime archive."""
import os
import hashlib
import json
from driver_features import candidate_flags
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

project = Path(__file__).resolve().parent.parent
sdk = Path(os.environ.get('PS5SDK_ROOT', str(Path.home() / 'ps5sdk')))
gl = Path(os.environ.get('PS5_OPENGL_ROOT', sdk / 'ps5-opengl-030/ps5-opengl'))
prefix = Path(os.environ.get('PS5_OPENGL_SDK', sdk / 'extracted/ps5-opengl-sdk-0.3.0/sdk'))
config = Path(os.environ.get('PS5_RUNTIME_CONFIG', prefix.parent / 'runtime-config.txt'))
output = project / 'build/draw-batching-driver'
output.mkdir(parents=True, exist_ok=True)
source = project / 'patches/ps5-draw-batching/ps5_screen.c'
lines = config.read_text().splitlines()
flags = [flag.replace('/home/runner/work/ps5-opengl/ps5-opengl', str(gl))
         for line in lines[1:] for flag in shlex.split(line)]
flags = ['-O2' if flag == '-Os' else flag for flag in flags]
# Override release SDK profiling after its flags, on both screen and backend.
flags += ['-UPS5_DRAW_PROFILE'] if os.environ.get('PS5_PORT_PROFILE', '0') == '0' else ['-DPS5_DRAW_PROFILE=1']
flags += candidate_flags()
flags += ['-Wno-error=unused-variable', '-Wno-error=unused-function']
command = [str(sdk / 'native-app-boilerplate/tooling/prospero-clang18'), *flags,
           '-DPS5_RUNTIME_QUIET=1', '-I' + str(gl / 'src/platform'),
           '-c', str(source), '-o', str(output / 'ps5_screen.o')]
env = os.environ.copy()
env['PS5_PAYLOAD_SDK'] = str(sdk / 'native-app-boilerplate/.deps/native/ps5-payload-sdk')
subprocess.run(command, env=env, check=True)
archive = project / 'driver/lib/libps5_opengl_core33.a'
archive.parent.mkdir(parents=True, exist_ok=True)
shutil.copy2(prefix / 'lib/libps5_opengl_core33.a', archive)
subprocess.run(['llvm-ar', 'r', archive, output / 'ps5_screen.o'], check=True)
subprocess.run(['llvm-ranlib', archive], check=True)
# The current screen uses the native runtime's scanout clean hints. Restore
# that runtime member after starting from the release SDK archive.
subprocess.run([sys.executable, str(project / 'tools/build-scanout-clean-runtime.py')],
               env=env, check=True)
subprocess.run([sys.executable, str(project / 'tools/build-egl-fallback.py')],
               env=env, check=True)
receipt = {
    'schema': 1,
    'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
    'candidate_flags': candidate_flags(),
    'detailed_profiling': os.environ.get('PS5_PORT_PROFILE', '0') != '0',
}
archive.with_suffix('.build.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(archive)
