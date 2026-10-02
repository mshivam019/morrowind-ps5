#!/usr/bin/env python3
"""Run actual element lifecycle, format conversion and layout-cache source."""
from pathlib import Path
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'patches/ps5-draw-batching/ps5_screen.c').read_text()
def function(name):
 start=s.rindex('\n'+name+'(')+1
 begin=s.index('{',start);end=begin+1;depth=1
 while depth:
  depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[start:end]
fmt=function('ps5_vertex_format')
fmt=fmt.replace('{\n   switch','{\n   ++format_calls;\n   switch',1)
layout=function('ps5_vertex_layout_from_state')
create=function('ps5_create_vertex_elements_state')
bind=function('ps5_bind_vertex_elements_state')
delete=function('ps5_delete_vertex_elements_state')
a=s.index('#define PS5_VERTEX_LAYOUT_CACHE_SLOTS');b=s.index('\nstruct ps5_fence',a)
state=s[a:b]
formats=sorted(set(re.findall(r'PIPE_FORMAT_[A-Z0-9_]+',fmt+layout+create)))
outputs=sorted(set(re.findall(r'PSBC_VERTEX_FORMAT_[A-Z0-9_]+',fmt)))
flags=sorted(set(re.findall(r'PS5_ENABLE_[A-Z0-9_]+',fmt+create)))
head=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PIPE_MAX_ATTRIBS 32
#define PSBC_MAX_VERTEX_ATTRIBUTES 32
#define VERT_ATTRIB_GENERIC0 16
#define BITFIELD64_BIT(n) (UINT64_C(1)<<(n))
'''
head+='enum pipe_format {'+','.join(formats)+'};\n'
head+='typedef enum {'+','.join(outputs)+'} PsbcVertexFormat;\n'
head+='\n'.join('#define '+flag+' 1' for flag in flags)+'\n'
head+=r'''
typedef struct { unsigned location,binding,offset,stride,alignment,instance_divisor; PsbcVertexFormat format; } PsbcVertexAttribute;
struct ps5_vertex_layout { uint32_t count; PsbcVertexAttribute attributes[PSBC_MAX_VERTEX_ATTRIBUTES]; };
struct pipe_vertex_element { enum pipe_format src_format; unsigned vertex_buffer_index,src_offset,src_stride,instance_divisor; bool dual_slot; };
struct pipe_context { int unused; };
struct nir { struct { uint64_t inputs_read; } info; };
struct ps5_shader { struct nir *nir; };
static unsigned format_calls;
'''+state+r'''
struct ps5_context { struct pipe_context base; struct ps5_vertex_elements *vertex_elements; };
static bool
'''+fmt+'\nstatic bool\n'+layout+'\nstatic void *\n'+create+'\nstatic void\n'+bind+'\nstatic void\n'+delete+r'''
static void inputs(struct nir *nir,uint64_t mask) { nir->info.inputs_read=mask<<VERT_ATTRIB_GENERIC0; }
static void zero_tail(const struct ps5_vertex_layout *layout) {
 for(unsigned i=layout->count;i<PSBC_MAX_VERTEX_ATTRIBUTES;++i) {
  const unsigned char *bytes=(const unsigned char*)&layout->attributes[i];
  for(unsigned j=0;j<sizeof(layout->attributes[i]);++j)assert(bytes[j]==0);
 }
}
int main(void) {
 struct ps5_context context={0};struct nir nir={0};struct ps5_shader shader={&nir};
 struct pipe_vertex_element element[3]={
  {PIPE_FORMAT_R32G32B32_FLOAT,2,16,48,0,false},
  {PIPE_FORMAT_R64G64_FLOAT,1,32,64,7,false},
  {PIPE_FORMAT_R32_FLOAT,0,4,16,3,false}
 };
 struct ps5_vertex_elements *a=ps5_create_vertex_elements_state(&context.base,3,element);
 assert(a);ps5_bind_vertex_elements_state(&context.base,a);
 struct ps5_vertex_layout got,reference;
 inputs(&nir,BITFIELD64_BIT(1)|BITFIELD64_BIT(5));
 memset(&got,0xa5,sizeof(got));assert(ps5_vertex_layout_from_state(&shader,a,&got));
 assert(got.count==2 && got.attributes[0].location==1 && got.attributes[1].location==5);
 assert(got.attributes[0].format==PSBC_VERTEX_FORMAT_R32G32B32_FLOAT);
 assert(got.attributes[0].binding==2 && got.attributes[0].offset==16 && got.attributes[0].stride==48);
 assert(got.attributes[1].alignment==8 && got.attributes[1].instance_divisor==7);
 zero_tail(&got);reference=got;unsigned before=format_calls;
 for(unsigned i=0;i<600;++i) {
  memset(&got,0xff,sizeof(got));assert(ps5_vertex_layout_from_state(&shader,a,&got));
  assert(!memcmp(&got,&reference,sizeof(got)));zero_tail(&got);
 }
 assert(format_calls==before);
 // Shader object identity is irrelevant; exact input mask is the only key.
 struct ps5_shader other_shader={&nir};assert(ps5_vertex_layout_from_state(&other_shader,a,&got));assert(format_calls==before);
 inputs(&nir,0);assert(ps5_vertex_layout_from_state(&shader,a,&got));assert(!got.count);zero_tail(&got);
 // More masks than slots evict safely; the old mask recomputes equivalentbytes.
 for(unsigned i=0;i<5;++i){inputs(&nir,BITFIELD64_BIT(i));assert(ps5_vertex_layout_from_state(&shader,a,&got));}
 inputs(&nir,BITFIELD64_BIT(1)|BITFIELD64_BIT(5));before=format_calls;
 assert(ps5_vertex_layout_from_state(&shader,a,&got));assert(format_calls==before+2);assert(!memcmp(&got,&reference,sizeof(got)));
 // An independentlycreated state with identical mask but different format/
 // stride/divisor cannot borrow another state's cached layout.
 element[0].src_format=PIPE_FORMAT_R32_FLOAT;element[0].src_stride=20;element[0].instance_divisor=9;
 struct ps5_vertex_elements *b=ps5_create_vertex_elements_state(&context.base,3,element);
 assert(ps5_vertex_layout_from_state(&shader,b,&got));
 assert(got.attributes[0].format==PSBC_VERTEX_FORMAT_R32_FLOAT && got.attributes[0].stride==20 && got.attributes[0].instance_divisor==9);
 // Insufficient elements and unsupported formats never install a successcache.
 inputs(&nir,15);before=format_calls;
 assert(!ps5_vertex_layout_from_state(&shader,b,&got));assert(format_calls==before+3);
 assert(!ps5_vertex_layout_from_state(&shader,b,&got));assert(format_calls==before+6);
 element[0].src_format=(enum pipe_format)9999;
 struct ps5_vertex_elements *bad=ps5_create_vertex_elements_state(&context.base,1,element);
 inputs(&nir,1);before=format_calls;
 assert(!ps5_vertex_layout_from_state(&shader,bad,&got));assert(!ps5_vertex_layout_from_state(&shader,bad,&got));assert(format_calls==before+2);
 assert(!ps5_vertex_layout_from_state(&shader,NULL,&got));
 inputs(&nir,0);assert(ps5_vertex_layout_from_state(&shader,NULL,&got));zero_tail(&got);
 // Actual destroy clears binding; calloc of a new state provides no stalekey,
 // even if allocator reuses a former element-state address.
 ps5_delete_vertex_elements_state(&context.base,a);assert(!context.vertex_elements);
 element[0].src_format=PIPE_FORMAT_R32G32_FLOAT;
 struct ps5_vertex_elements *fresh=ps5_create_vertex_elements_state(&context.base,1,element);
 for(unsigned i=0;i<PS5_VERTEX_LAYOUT_CACHE_SLOTS;++i)assert(!fresh->layout_cache[i].valid);
 inputs(&nir,1);before=format_calls;assert(ps5_vertex_layout_from_state(&shader,fresh,&got));assert(format_calls==before+1);
 ps5_delete_vertex_elements_state(&context.base,b);ps5_delete_vertex_elements_state(&context.base,bad);ps5_delete_vertex_elements_state(&context.base,fresh);
 puts("PASS actual vertex-layout cache: 600 hits avoid format conversions; input masks/eviction/state lifecycle/errors/strides/divisors/FP64 alignment/zero tails preserved.");
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'test.c').write_text(head)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
