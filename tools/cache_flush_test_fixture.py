"""Intercept only hardware side effects in the production cache helper."""
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent

def intercepted_header(record='record_line', fence='record_fence'):
    source=(ROOT/'patches/ps5-draw-batching/platform/ps5_cache_flush.h').read_text()
    start=source.index('static inline bool\nps5_cache_flush_detect(void)\n{')
    end=source.index('\n}',start)+2
    source=source[:start]+'''static inline bool
ps5_cache_flush_detect(void)
{ ++detect_calls; return test_support; }'''+source[end:]
    for instruction, optimized in (('clflushopt','true'),('clflush','false')):
        asm=f'__asm__ volatile("{instruction} (%0)" : : "r"(first + line * 64) : "memory");'
        assert source.count(asm)==1
        source=source.replace(asm,f'{record}(first + line * 64, {optimized});')
    source=source.replace('__asm__ volatile("mfence" ::: "memory");',f'{fence}();')
    assert '__asm__' not in source
    return source
