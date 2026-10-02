#!/usr/bin/env python3
"""Exercise canonical flush loop and descriptor range updates with a cache oracle."""
from pathlib import Path
import subprocess
import tempfile
from cache_flush_test_fixture import intercepted_header

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'patches/ps5-draw-batching/ps5_screen.c').read_text()

def function(name):
    start = source.index(name + '(')
    begin = source.index('{', start)
    depth = 1
    end = begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

flush = function('ps5_flush_gpu_data_group')
constant = function('ps5_prepare_constant')
texture = function('ps5_prepare_texture')
copied_record_start = constant.index('ps5_descriptor_flush_record(descriptor_flush, storage->data,')
copied_record_end = constant.index(';', copied_record_start) + 1
copied_record = constant[copied_record_start:copied_record_end]
assert 'copied_offset, copied_offset + state->size' in copied_record
copied_offset_function = function('ps5_copied_constant_offset')
# Extract actual bookkeeping statements rather than duplicating their formulas.
updates = '\n'.join(line.strip() for line in constant.splitlines()
                    if 'descriptor_offset =' in line or 'descriptor_first = MIN2' in line or 'descriptor_end = MAX2' in line)
assert constant.index('descriptor_end = MAX2') < constant.index('if (!state->valid)')
assert 'descriptor_first = 0;' in constant and 'descriptor_end = PS5_CONSTANT_DATA_OFFSET;' in constant
assert texture.count('flush_first = MIN2(flush_first, binding->offset);') == 2
assert 'descriptor_flush, table->data,' in texture
assert 'flush_first, flush_size, PS5_FLUSH_DESCRIPTOR' in texture
texture_updates = '\n'.join(line.strip() for line in texture.splitlines()
                           if 'flush_first = MIN2' in line or 'flush_size = MAX2' in line)
# The two branches contain identical tracking; execute each separately.
texture_updates = texture_updates.splitlines()
assert texture_updates[:2] == texture_updates[2:]
accumulator_start = source.index('struct ps5_descriptor_flush_range {')
accumulator_end = source.index('static void\nps5_flush_gpu_data(', accumulator_start)
accumulator = source[accumulator_start:accumulator_end]
draw = function('ps5_draw_vbo_locked_inner')
assert draw.count('ps5_descriptor_flush_finish(&descriptor_flush[') == 2
boundary = draw.index('ps5_descriptor_flush_finish(&descriptor_flush[0])')
assert boundary > draw.index('!ps5_prepare_texture(context, context->fs')
assert boundary < draw.index('if (!ps5_agc_gate2_run')
assert boundary < draw.index('ps5_agc_gate2_run()')
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#define MIN2(a,b) ((a)<(b)?(a):(b))
#define MAX2(a,b) ((a)>(b)?(a):(b))
#define PS5_CPU_PROFILE_CLOCK() 0
#define PS5_CPU_PROFILE_ADD(field,value) ((void)0)
enum ps5_flush_group { PS5_FLUSH_DESCRIPTOR, PS5_FLUSH_UNIFORM };
#include <string.h>
#include <stdbool.h>
unsigned detect_calls; bool test_support;
static uintptr_t recorded[1024];
static unsigned count, fences;
static void record_line(uintptr_t at, bool optimized) { (void)optimized; assert(count<1024); recorded[count++]=at; }
static void record_fence(void) { ++fences; }
''' + intercepted_header() + '\nstatic void\n' + flush + '\n' + accumulator + '\n#define PS5_CONSTANT_DATA_OFFSET 4096u\n#define PS5_MAX_CONSTANT_BUFFER_SIZE 16384u\n#define PS5_GEOMETRY_CONSTANT_SLOT 2u\nstatic size_t\n' + copied_offset_function + r'''
int main(void) {
  _Alignas(64) uint8_t memory[32768];
  for(unsigned offset=0;offset<128;++offset)
    for(unsigned bytes=0;bytes<258;++bytes) {
      count=fences=0;
      ps5_flush_gpu_data_group(memory+offset,bytes,PS5_FLUSH_DESCRIPTOR);
      unsigned expected=0;
      for(unsigned line=0;line<512;line+=64)
        if(bytes && line<offset+bytes && line+64>offset) {
          assert(recorded[expected++]==(uintptr_t)memory+line);
        }
      assert(count==expected && fences==1);
    }
  struct { size_t offset, stride; } entry, *binding=&entry;
  size_t descriptor_first=SIZE_MAX,descriptor_end=0;
  unsigned array_index;
  entry.offset=1024;
  for(array_index=0;array_index<4;++array_index) {
''' + updates + r'''
  }
  assert(descriptor_first==1024 && descriptor_end==1088);
  // Invalid/unbound entries are covered because bookkeeping precedes continue.
  descriptor_first=0;descriptor_end=4096;
  entry.offset=2048;array_index=3;
  {
''' + updates + r'''
  }
  assert(descriptor_first==0 && descriptor_end==4096);
  for(unsigned branch=0;branch<2;++branch) {
    size_t flush_first=SIZE_MAX,flush_size=0;
    entry.offset=768;entry.stride=48;
''' + '\n'.join(texture_updates[:2]) + r'''
    entry.offset=96;entry.stride=48;
''' + '\n'.join(texture_updates[2:]) + r'''
    assert(flush_first==96 && flush_size==816);
    count=fences=0;
    ps5_flush_gpu_data_group(memory+flush_first,flush_size-flush_first,PS5_FLUSH_DESCRIPTOR);
    assert(recorded[0]==(uintptr_t)memory+64);
    assert(recorded[count-1]==(uintptr_t)memory+768);
  }
  // Actual accumulator: UBO header + later texture writes share ONE fence.
  struct ps5_descriptor_flush_range range={0};
  count=fences=0;
  ps5_descriptor_flush_record(&range,memory,0,1024,PS5_FLUSH_UNIFORM);
  ps5_descriptor_flush_record(&range,memory,768,816,PS5_FLUSH_DESCRIPTOR);
  assert(count==0 && fences==0);
  ps5_descriptor_flush_finish(&range);
  assert(count==16 && fences==1 && range.data==NULL);
  // A subsequent draw/bank never inherits the prior accumulator.
  ps5_descriptor_flush_record(&range,memory,128,144,PS5_FLUSH_DESCRIPTOR);
  ps5_descriptor_flush_finish(&range);
  assert(count==17 && fences==2);
  // No UBO and no textures has no flush; complex helpers remain immediate.
  ps5_descriptor_flush_finish(&range);
  assert(fences==2);
  ps5_descriptor_flush_record(NULL,memory,63,65,PS5_FLUSH_DESCRIPTOR);
  assert(count==19 && fences==3);
  // Changing backing flushes previous range before recording new bank.
  ps5_descriptor_flush_record(&range,memory,0,16,PS5_FLUSH_UNIFORM);
  ps5_descriptor_flush_record(&range,memory+512,0,16,PS5_FLUSH_DESCRIPTOR);
  assert(fences==4 && range.data==memory+512);
  ps5_descriptor_flush_finish(&range);
  assert(fences==5);
  // Replay the actual copied-uniform call with VS/FS owned arenas. Headers
  // and data are contiguous; union flush adds no gap bytes or extra fences.
  struct { uint8_t *data; } backing={memory}, *storage=&backing;
  struct { size_t size; } uniform_state={0}, *state=&uniform_state;
  struct ps5_descriptor_flush_range *descriptor_flush=&range;
  size_t sizes[]={1,63,64,65,128,4096,4224,16384};
  for(unsigned slot=0;slot<2;++slot)for(unsigned i=0;i<8;++i) {
    const size_t copied_offset=ps5_copied_constant_offset(slot);
    assert(copied_offset==4096);state->size=sizes[i];count=fences=0;
''' + copied_record + r'''
    ps5_descriptor_flush_record(&range,memory,0,4096,PS5_FLUSH_UNIFORM);
    ps5_descriptor_flush_record(&range,memory,0,48,PS5_FLUSH_DESCRIPTOR);
    assert(!count && !fences && range.first==0 && range.end==4096+state->size);
    ps5_descriptor_flush_finish(&range);
    assert(fences==1 && count==(4096+state->size+63)/64);
    assert(recorded[0]==(uintptr_t)memory);
    assert(recorded[count-1]==(uintptr_t)memory+((4096+state->size-1)/64)*64);
  }
  // Complex geometry paths pass NULL: copied data is still immediately
  // visible, at the original stage-specific offset, before header handling.
  descriptor_flush=NULL;state->size=65;count=fences=0;
  {
    const size_t copied_offset=ps5_copied_constant_offset(2);
''' + copied_record + r'''
    assert(count==2 && fences==1 && recorded[0]==(uintptr_t)memory+copied_offset);
  }
  assert(!range.data);
  // Typical one texture at offset 768: flush48 bytes instead of816 bytes.
  puts("PASS: 33024 cache-range cases; sparse/unbound UBO zero spans; both texture branches; 816->48 logical descriptor bytes; copied VS/FS header+data one contiguous fence, complex stages immediate");
}
'''
with tempfile.TemporaryDirectory() as temp:
    c = Path(temp)/'test.c'
    c.write_text(program)
    binary=Path(temp)/'test'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Wno-unused-variable','-Wno-unused-parameter',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
