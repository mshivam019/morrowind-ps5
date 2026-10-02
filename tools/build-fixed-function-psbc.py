#!/usr/bin/env python3
"""Rebuild three PSBC objects in an isolated compiler archive; leave SDK intact."""
from pathlib import Path
import os
import argparse
import shlex
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--host', action='store_true', help='build host compiler archive for CPU regression checks')
args = parser.parse_args()
project = Path(__file__).resolve().parent.parent
source = Path(os.environ.get('PS5_OPENGL_ROOT', str(Path.home() / 'ps5sdk/ps5-opengl-030/ps5-opengl'))) / 'third_party/opengnm-psbc'
sdk_root = Path(os.environ.get('PS5SDK_ROOT', str(Path.home() / 'ps5sdk')))
env = os.environ.copy()
env['PS5_PAYLOAD_SDK'] = str(sdk_root / 'native-app-boilerplate/.deps/native/ps5-payload-sdk')
output = project / ('build/fixed-function-psbc' if args.host else 'driver/lib')
objects = project / 'build/fixed-function-psbc'
output.mkdir(parents=True, exist_ok=True)
objects.mkdir(parents=True, exist_ok=True)
for relative in ['src/amd/vulkan/nir/radv_nir_lower_io.c', 'libpsbc/psbc_compile.c', 'src/amd/vulkan/radv_shader_info.c']:
    target = relative[:-2] + ('.o' if args.host else '.ps5.o')
    make_config = ['CONFIG=../../toolchain/opengnm-psbc-host.mak'] if args.host else ['-f', '../../toolchain/Makefile.opengnm-psbc-ps5']
    result = subprocess.run(['make', '-n', *make_config, '-W', relative, target], cwd=source, env=env, check=True, capture_output=True, text=True)
    commands = [line for line in result.stdout.splitlines() if ' -c ' in line and ' -o ' in line]
    if len(commands) != 1:
        raise RuntimeError(f'Expected one compile command for {relative}: {result.stdout}')
    command = shlex.split(commands[0])
    if not args.host:
        command[0] = str(sdk_root / 'native-app-boilerplate/tooling/prospero-clang18')
    command[command.index('-c') + 1] = str(project / 'patches/psbc-fixed-function' / relative)
    command[command.index('-o') + 1] = str(objects / Path(target).name)
    subprocess.run(command, cwd=source, env=env, check=True)
# The source tree archive shares the NIR/ACO ABI of these patched sources.
archive = output / ('libpsbc-host.a' if args.host else 'libpsbc.ps5.a')
shutil.copy2(source / ('libpsbc.a' if args.host else 'libpsbc.ps5.a'), archive)
suffix = '.o' if args.host else '.ps5.o'
subprocess.run(['llvm-ar', 'r', archive, *[objects / (name + suffix) for name in ['radv_nir_lower_io', 'psbc_compile', 'radv_shader_info']]], check=True)
subprocess.run(['llvm-ranlib', archive], check=True)
print(archive)
