#!/usr/bin/env python3
"""Build profiling-off screen/native candidate, preserving the canonical archive."""
import os,shlex,shutil,subprocess
from pathlib import Path
p=Path(__file__).resolve().parent.parent
sdk=Path(os.environ.get('PS5SDK_ROOT',str(Path.home() / 'ps5sdk')))
gl=sdk/'ps5-opengl-030/ps5-opengl'
prefix=sdk/'extracted/ps5-opengl-sdk-0.3.0/sdk'
lines=(prefix.parent/'runtime-config.txt').read_text().splitlines()
flags=[f.replace('/home/runner/work/ps5-opengl/ps5-opengl',str(gl)) for line in lines[1:] for f in shlex.split(line)]
flags=['-O2' if f=='-Os' else f for f in flags]
nativeflags=[f.replace('/home/runner/work/ps5-opengl/ps5-opengl',str(gl)) for i in (1,4) for f in shlex.split(lines[i])]
nativeflags=['-O2' if f=='-Os' else f for f in nativeflags]
out=p/'build/production-profile-candidate';out.mkdir(parents=True,exist_ok=True)
source=p/'patches/ps5-production-profile'
env=os.environ.copy();env['PS5_PAYLOAD_SDK']=str(sdk/'native-app-boilerplate/.deps/native/ps5-payload-sdk')
cc=str(sdk/'native-app-boilerplate/tooling/prospero-clang18')
# -U MUST follow all SDK flags: SDK defaults may explicitly enable profiling.
common=['-UPS5_DRAW_PROFILE','-DPS5_RUNTIME_QUIET=1','-I'+str(gl/'src/platform'),'-Wno-error=unused-function','-Wno-error=unused-variable']
subprocess.run([cc,*flags,*common,'-c',str(source/'ps5_screen.c'),'-o',str(out/'ps5_screen.o')],env=env,check=True)
subprocess.run([cc,*nativeflags,*common,'-I'+str(gl/'third_party/opengnm-psbc/libpsbc'),'-Dmain=ps5_agc_gate2_run','-DAGC_TRIANGLE_SUBMIT=1','-DAGC_RUNTIME_PACKAGES=1','-c',str(source/'platform/ps5_agc_runtime_backend.c'),'-o',str(out/'ps5_agc_runtime_backend.o')],env=env,check=True)
archive=out/'libps5_opengl_core33.a';shutil.copy2(p/'driver/lib/libps5_opengl_core33.a',archive)
subprocess.run(['llvm-ar','r',str(archive),str(out/'ps5_screen.o'),str(out/'ps5_agc_runtime_backend.o')],check=True)
subprocess.run(['llvm-ranlib',str(archive)],check=True)
print(archive)
