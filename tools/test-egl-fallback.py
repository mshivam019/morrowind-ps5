#!/usr/bin/env python3
"""Verify installed EGL options and upstream Mesa's existing DDS fallback path."""
import os
from pathlib import Path
import hashlib,json,os,re,subprocess
p=Path(__file__).resolve().parent.parent
source=p/'patches/ps5-egl/ps5_egl.c'
receipt=json.loads((p/'build/draw-batching-driver/egl-build-receipt.json').read_text())
assert hashlib.sha256(source.read_bytes()).hexdigest()==receipt['source_sha256']
for macro in ['PS5_ENABLE_COMPRESSED_FALLBACK_CANDIDATE','PS5_ENABLE_CORE_CONTEXT_CANDIDATE']:
 assert '-D'+macro+'=1' in receipt['flags']
obj=p/'build/draw-batching-driver/ps5_egl.o'
installed=subprocess.check_output(['llvm-ar','p',str(p/'driver/lib/libps5_opengl_core33.a'),'ps5_egl.o'])
assert installed==obj.read_bytes()
dwarf=subprocess.check_output(['llvm-dwarfdump',str(obj)],text=True)
assert re.search(r'DW_AT_name\s*\("allow_compressed_fallback"\).*?DW_AT_data_member_location\s*\(0x32\)',dwarf,re.S)
asm=subprocess.check_output(['llvm-objdump','-dr','--no-show-raw-insn',str(obj)],text=True)
create=asm.split('<eglCreateContext>:',1)[1].split('st_api_query_versions',1)[0]
assert re.search(r'movb\s+\$0x1, %al',create)
assert re.search(r'movb\s+%al, 0x32\(%rsi\)',create)
# Source evidence for native capability versus standard Mesa decompression.
sdk=Path(os.environ.get('PS5SDK_ROOT',str(Path.home() / 'ps5sdk')))
gl=Path(os.environ.get('PS5_OPENGL_ROOT',sdk/'ps5-opengl-030/ps5-opengl'))
mesa=gl/'third_party/mesa-26.2.0/src/mesa/state_tracker'
cb=(mesa/'st_cb_texture.c').read_text()
assert 'case MESA_FORMAT_LAYOUT_S3TC:\n      return !st->has_s3tc;' in cb
fmt=(mesa/'st_format.c').read_text()
assert '_mesa_is_format_s3tc(mesaFormat) && !st->has_s3tc' in fmt
assert 'PIPE_FORMAT_R8G8B8A8_SRGB :' in fmt and 'PIPE_FORMAT_R8G8B8A8_UNORM;' in fmt
assert 'handles decompression on upload (Unmap)' in cb
print('PASS installed EGL sets compressed fallback=true; core context flag preserved; source SHA/member match; Mesa maps unsupported S3TC to RGBA and decompresses upload.')
