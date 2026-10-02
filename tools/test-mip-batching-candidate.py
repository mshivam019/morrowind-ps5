#!/usr/bin/env python3
"""Compile and exercise the candidate's actual mip eligibility classifier."""
from pathlib import Path
import os
import subprocess
project = Path(__file__).resolve().parent.parent
sdk = Path(os.environ.get('PS5SDK_ROOT', str(Path.home() / 'ps5sdk')))
gl = Path(os.environ.get('PS5_OPENGL_ROOT', sdk / 'ps5-opengl-030/ps5-opengl'))
mesa = gl / 'third_party/mesa-26.2.0'
source = (project / 'patches/ps5-draw-batching/ps5_screen.c').read_text()
start = source.index('static bool\nps5_batch_mip_view_eligible')
end = source.index('\n/* Bounded category receipts:', start)
output = project / 'build/mip-batching-driver'
output.mkdir(parents=True, exist_ok=True)
test = output / 'mip-classifier-test.c'
test.write_text('#include <assert.h>\n#include <stdio.h>\n#include "pipe/p_state.h"\n' + source[start:end] + r'''
int main(void) {
 struct pipe_resource good = { .target=PIPE_TEXTURE_2D,
  .format=PIPE_FORMAT_R8G8B8A8_UNORM, .last_level=8, .array_size=1, .depth0=1 };
 struct pipe_sampler_view view = { .target=PIPE_TEXTURE_2D,
  .format=PIPE_FORMAT_R8G8B8A8_UNORM };
 view.u.tex.last_level=8;
 assert(ps5_batch_mip_view_eligible(&good,&view,true,false));
 view.u.tex.first_level=2;view.u.tex.last_level=5;
 assert(ps5_batch_mip_view_eligible(&good,&view,true,false));
 assert(!ps5_batch_mip_view_eligible(&good,&view,false,false));
 assert(!ps5_batch_mip_view_eligible(&good,&view,true,true));
 view.u.tex.first_level=6;
 assert(!ps5_batch_mip_view_eligible(&good,&view,true,false));
 view.u.tex.first_level=0;view.u.tex.last_level=9;
 assert(!ps5_batch_mip_view_eligible(&good,&view,true,false));
 view.u.tex.last_level=8;
 good.bind=PIPE_BIND_RENDER_TARGET|PIPE_BIND_SAMPLER_VIEW;
 assert(ps5_batch_mip_view_eligible(&good,&view,true,false));
 const unsigned binds[]={PIPE_BIND_DISPLAY_TARGET,PIPE_BIND_DEPTH_STENCIL};
 for(unsigned i=0;i<2;i++){good.bind=binds[i];
  assert(!ps5_batch_mip_view_eligible(&good,&view,true,false));}
 good.bind=0;good.nr_samples=4;
 assert(!ps5_batch_mip_view_eligible(&good,&view,true,false));
 good.nr_samples=0;good.array_size=2;
 assert(!ps5_batch_mip_view_eligible(&good,&view,true,false));
 good.array_size=1;view.u.tex.last_layer=1;
 assert(!ps5_batch_mip_view_eligible(&good,&view,true,false));
 view.u.tex.last_layer=0;good.format=PIPE_FORMAT_Z32_FLOAT;
 assert(!ps5_batch_mip_view_eligible(&good,&view,true,false));
 good.format=PIPE_FORMAT_R8G8B8A8_UNORM;good.last_level=16;
 assert(!ps5_batch_mip_view_eligible(&good,&view,true,false));
 good.last_level=0;view.u.tex.first_level=0;view.u.tex.last_level=0;
 assert(ps5_batch_mip_view_eligible(&good,&view,false,true));
 puts("Mip classifier: linear chain/subrange accepted; depth-staging/alias/tiled/MSAA/layers/depth/invalid LOD rejected; single-mip policy preserved.");
}
''')
command=['cc','-std=c11','-D_GNU_SOURCE','-DHAVE_ENDIAN_H','-I'+str(gl/'build/mesa-ps5-probe/src'),'-I'+str(mesa/'src/gallium/include'),'-I'+str(mesa/'src'),'-I'+str(mesa/'include'),str(test),'-o',str(output/'mip-classifier-test')]
subprocess.run(command,check=True)
subprocess.run([str(output/'mip-classifier-test')],check=True)
