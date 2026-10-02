#!/usr/bin/env python3
"""Compile the actual native depth-plane bulk-copy eligibility predicate."""
from pathlib import Path
import os
import subprocess
project=Path(__file__).resolve().parent.parent
sdk=Path(os.environ.get('PS5SDK_ROOT',str(Path.home() / 'ps5sdk')))
gl=Path(os.environ.get('PS5_OPENGL_ROOT',sdk/'ps5-opengl-030/ps5-opengl'))
mesa=gl/'third_party/mesa-26.2.0'
s=(project/'patches/ps5-draw-batching/ps5_screen.c').read_text()
a=s.index('static bool\nps5_depth_bulk_copy_eligible');b=s.index('\n/* Shared bounded',a)
out=project/'build/draw-batching-driver';out.mkdir(parents=True,exist_ok=True)
c=out/'depth-bulk-test.c'
c.write_text('#include <assert.h>\n#include <stdio.h>\n#include "pipe/p_state.h"\n'+s[a:b]+r'''
int main(void) {
 struct pipe_resource src={.target=PIPE_TEXTURE_2D,.format=PIPE_FORMAT_Z32_FLOAT_S8X24_UINT,
 .width0=1920,.height0=1080,.depth0=1,.array_size=1,.bind=PIPE_BIND_DEPTH_STENCIL};
 struct pipe_resource dst=src;
 struct pipe_blit_info good={0};
 good.src.resource=&src;good.dst.resource=&dst;
 good.src.format=good.dst.format=src.format;good.mask=PIPE_MASK_Z;
 good.filter=PIPE_TEX_FILTER_NEAREST;
 good.src.box=good.dst.box=(struct pipe_box){.width=1920,.height=1080,.depth=1};
 assert(ps5_depth_bulk_copy_eligible(&good));
 struct pipe_blit_info bad;
#define REJECT(change) bad=good; change; assert(!ps5_depth_bulk_copy_eligible(&bad))
 REJECT(bad.mask=PIPE_MASK_ZS);REJECT(bad.mask=PIPE_MASK_S);
 REJECT(bad.src.box.height=-1080);REJECT(bad.src.box.x=1);
 REJECT(bad.dst.box.width=1919);REJECT(bad.scissor_enable=true);
 REJECT(bad.src.level=1);REJECT(bad.src.box.z=1);
 REJECT(bad.src.resource=&dst);REJECT(bad.filter=PIPE_TEX_FILTER_LINEAR);
 REJECT(bad.swizzle_enable=true);REJECT(bad.alpha_blend=true);
 dst.nr_samples=4;assert(!ps5_depth_bulk_copy_eligible(&good));dst.nr_samples=0;
 dst.nr_storage_samples=4;assert(!ps5_depth_bulk_copy_eligible(&good));dst.nr_storage_samples=0;
 dst.last_level=1;assert(!ps5_depth_bulk_copy_eligible(&good));dst.last_level=0;
 dst.array_size=2;assert(!ps5_depth_bulk_copy_eligible(&good));dst.array_size=1;
 dst.format=PIPE_FORMAT_Z32_FLOAT;assert(!ps5_depth_bulk_copy_eligible(&good));
 src.format=PIPE_FORMAT_Z32_FLOAT;good.src.format=good.dst.format=src.format;
 assert(ps5_depth_bulk_copy_eligible(&good));
 puts("Native depth bulk classifier: full depth plane accepted, stencil masks/flip/subregion/mip/layer/MSAA/alias rejected.");
}
''')
subprocess.run(['cc','-std=c11','-D_GNU_SOURCE','-DHAVE_ENDIAN_H','-I'+str(gl/'build/mesa-ps5-probe/src'),'-I'+str(mesa/'src/gallium/include'),'-I'+str(mesa/'src'),'-I'+str(mesa/'include'),str(c),'-o',str(out/'depth-bulk-test')],check=True)
subprocess.run([str(out/'depth-bulk-test')],check=True)
