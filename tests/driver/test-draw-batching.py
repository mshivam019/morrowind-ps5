#!/usr/bin/env python3
"""Exercise the actual driver's classifier using smooth and flat NIR inputs."""
from pathlib import Path
import os
import subprocess

project = Path(__file__).resolve().parents[2]
source = Path(os.environ.get('PS5_OPENGL_ROOT', str(Path.home() / 'ps5sdk/ps5-opengl-030/ps5-opengl'))) / 'third_party/opengnm-psbc'
output = project / 'build/draw-batching-driver'
text = (project / 'patches/ps5-draw-batching/ps5_screen.c').read_text()
start = text.index('static bool\nps5_fragment_has_flat_inputs(')
end = text.index('\nstatic void *', start)
helper = text[start:end]
code = '''#include <assert.h>
#include <stdio.h>
#include "compiler/nir/nir_builder.h"
#include "psbc_compile.h"
'''+helper+'''
static int type_size(const struct glsl_type* type, bool bindless) {
 return glsl_count_attribute_slots(type, false);
}
int main(void) {
 psbc_init();
 for (int flat=0; flat<2; flat++) {
  nir_builder b=nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
   psbc_get_nir_options(PSBC_STAGE_FRAGMENT),"flat-classifier");
  nir_variable* input=nir_variable_create(b.shader,nir_var_shader_in,glsl_vec4_type(),"input");
  input->data.location=VARYING_SLOT_VAR0;
  input->data.interpolation=flat?INTERP_MODE_FLAT:INTERP_MODE_SMOOTH;
  nir_variable* color=nir_variable_create(b.shader,nir_var_shader_out,glsl_vec4_type(),"color");
  color->data.location=FRAG_RESULT_DATA0;
  nir_store_var(&b,color,nir_load_var(&b,input),15);
  assert(ps5_fragment_has_flat_inputs(b.shader)==flat);
  nir_lower_io(b.shader,nir_var_shader_in|nir_var_shader_out,type_size,nir_lower_io_use_interpolated_input_intrinsics);
  nir_remove_dead_variables(b.shader,nir_var_shader_in|nir_var_shader_out,NULL);
  assert(ps5_fragment_has_flat_inputs(b.shader)==flat);
  printf("%s classification passed before/after lowered IO\\n",flat?"flat":"smooth");
  ralloc_free(b.shader);
 }
 psbc_shutdown();
}
'''
(output / 'draw-batching-test.c').write_text(code)
flags = ['-std=gnu11','-DHAVE_ENDIAN_H=1','-DHAVE_FUNC_ATTRIBUTE_PACKED=1',
         '-DHAVE_PTHREAD=1','-DHAVE_STRUCT_TIMESPEC=1','-D_GNU_SOURCE']
for directory in ['include/mesa','include','src','libpsbc']:
    flags += ['-I',str(source / directory)]
subprocess.run(['cc',*flags,'-c',output / 'draw-batching-test.c','-o',output / 'draw-batching-test.o'],check=True)
subprocess.run(['g++','-o',output / 'draw-batching-test',output / 'draw-batching-test.o',
                project / 'build/fixed-function-psbc/libpsbc-host.a','-pthread','-lm'],check=True)
subprocess.run([output / 'draw-batching-test'],check=True,timeout=30)
