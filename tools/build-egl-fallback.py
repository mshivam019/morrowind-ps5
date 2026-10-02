#!/usr/bin/env python3
"""Rebuild isolated EGL with the release core33 context and compression switches."""
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
project=Path(__file__).resolve().parent.parent
sdk=Path(os.environ.get('PS5SDK_ROOT',str(Path.home() / 'ps5sdk')))
gl=Path(os.environ.get('PS5_OPENGL_ROOT',sdk/'ps5-opengl-030/ps5-opengl'))
prefix=Path(os.environ.get('PS5_OPENGL_SDK',sdk/'extracted/ps5-opengl-sdk-0.3.0/sdk'))
config=Path(os.environ.get('PS5_RUNTIME_CONFIG',prefix.parent/'runtime-config.txt'))
lines=config.read_text().splitlines()
flags=[f.replace('/home/runner/work/ps5-opengl/ps5-opengl',str(gl)) for line in lines[1:] for f in shlex.split(line)]
flags += ['-DPS5_ENABLE_COMPRESSED_FALLBACK_CANDIDATE=1','-DPS5_ENABLE_CORE_CONTEXT_CANDIDATE=1',
          '-DHAVE_PTHREAD=1','-DHAVE_STRUCT_TIMESPEC=1','-Wno-error=unused-parameter',
          '-Wno-unreachable-code-generic-assoc','-I'+str(gl/'third_party/mesa-26.2.0/src/mesa')]
flags += ['-UPS5_DRAW_PROFILE'] if os.environ.get('PS5_PORT_PROFILE','0')=='0' else []
source=project/'patches/ps5-egl/ps5_egl.c'
output=project/'build/draw-batching-driver';output.mkdir(parents=True,exist_ok=True)
obj=output/'ps5_egl.o'
env=os.environ.copy();env['PS5_PAYLOAD_SDK']=str(sdk/'native-app-boilerplate/.deps/native/ps5-payload-sdk')
command=[str(sdk/'native-app-boilerplate/tooling/prospero-clang18'),*flags,'-c',str(source),'-o',str(obj)]
subprocess.run(command,env=env,check=True)
archive=project/'driver/lib/libps5_opengl_core33.a'
if not archive.exists():raise SystemExit('Build canonical screen/native archive first')
subprocess.run(['llvm-ar','r',str(archive),str(obj)],check=True)
subprocess.run(['llvm-ranlib',str(archive)],check=True)
(output/'egl-build-receipt.json').write_text(json.dumps({'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'flags':flags,'compressed_fallback':True,'core_context':True},indent=2)+'\n')
print(archive)
