#!/usr/bin/env python3
"""Compile host packet/ownership checks plus isolated PS5 candidate objects."""
from pathlib import Path
import os, shlex, subprocess
p=Path(__file__).resolve().parent.parent
out=p/'build/native-coalesce-tests'; out.mkdir(parents=True,exist_ok=True)
subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(p/'patches/ps5-draw-batching/platform'),str(p/'tests/runtime/native-coalesce.c'),'-o',str(out/'host')],check=True)
subprocess.run([str(out/'host')],check=True)
source=(p/'patches/ps5-draw-batching/platform/ps5_agc_native_runtime.c').read_text()
a=source.index('static struct runtime_batch_entry {')
b=source.index('\n#endif\n\n#ifdef PS5_DRAW_PROFILE',a)
# Compile the actual native boundary guards, cutting only after the guard block;
# unreachable hardware operations are represented by a terminal success return.
boundaries = ''
for function in ('ps5_agc_gate2_shutdown_present', 'ps5_agc_gate2_present'):
 start=source.index('int '+function+'(')
 guard=source.index('        return -1; /* Pending GPU work',start)
 end=source.index('\n#endif\n#endif',guard)+len('\n#endif\n#endif')
 boundaries+=source[start:end]+'\n    return 0;\n}\n'
fixture=(p/'tests/runtime/native-submission-prefix.h').read_text()+source[a:b]+boundaries+(p/'tests/runtime/native-submission-main.h').read_text()
(out/'submission.c').write_text(fixture)
subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-variable','-Wno-unused-parameter','-I'+str(p/'patches/ps5-draw-batching/platform'),str(out/'submission.c'),'-o',str(out/'submission')],check=True)
subprocess.run([str(out/'submission')],check=True)
sdk=Path(os.environ.get('PS5SDK_ROOT',str(Path.home() / 'ps5sdk')))
gl=Path(os.environ.get('PS5_OPENGL_ROOT',sdk/'ps5-opengl-030/ps5-opengl'))
prefix=Path(os.environ.get('PS5_OPENGL_SDK',sdk/'extracted/ps5-opengl-sdk-0.3.0/sdk'))
config=Path(os.environ.get('PS5_RUNTIME_CONFIG',prefix.parent/'runtime-config.txt'))
lines=config.read_text().splitlines()
flags=[x.replace('/home/runner/work/ps5-opengl/ps5-opengl',str(gl)) for l in (lines[1],lines[4]) for x in shlex.split(l)]
env=dict(os.environ,PS5_PAYLOAD_SDK=str(sdk/'native-app-boilerplate/.deps/native/ps5-payload-sdk'))
for coalesce, asynchronous, profile, present, light in ((0,0,0,0,0),(1,1,0,1,1),(1,0,1,1,1),(0,1,0,0,0),(0,0,0,1,0),(0,0,0,0,1)):
 subprocess.run([str(sdk/'native-app-boilerplate/tooling/prospero-clang18'),*flags,*(['-DPS5_DRAW_PROFILE=1'] if profile else ['-UPS5_DRAW_PROFILE']),'-Wno-error=unused-function','-Wno-error=unused-variable','-I'+str(gl/'third_party/opengnm-psbc/libpsbc'),'-I'+str(gl/'src/platform'),'-Dmain=ps5_agc_gate2_run','-DAGC_TRIANGLE_SUBMIT=1','-DAGC_RUNTIME_PACKAGES=1',f'-DPS5_NATIVE_SUBMIT_COALESCE={coalesce}',f'-DPS5_ASYNC_BATCH={asynchronous}',f'-DPS5_PRESENT_COMPLETION_WAIT={present}',f'-DPS5_LIGHT_DIAGNOSTICS={light}','-c',str(p/'patches/ps5-draw-batching/platform/ps5_agc_runtime_backend.c'),'-o',str(out/f'backend-{coalesce}-{asynchronous}-{profile}-{present}-{light}.o')],env=env,check=True)
