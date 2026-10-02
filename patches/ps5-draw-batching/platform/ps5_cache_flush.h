/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5_CACHE_FLUSH_H
#define PS5_CACHE_FLUSH_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#if PS5_LIGHT_DIAGNOSTICS
#include <stdio.h>
#endif
#ifndef PS5_CLFLUSHOPT
#define PS5_CLFLUSHOPT 0
#endif
static inline bool
ps5_cache_flush_capable(uint32_t maximum_leaf, uint32_t leaf1_ebx,
                        uint32_t leaf1_edx, uint32_t leaf7_ebx)
{
   /* CPUID.01H:EBX reports the CLFLUSH line size in eight-byte units;
    * CPUID.07H.00H:EBX[23] advertises CLFLUSHOPT. Preserve the PS5's
    * existing 64-byte visibility granularity and fallback otherwise. */
   return maximum_leaf >= 7 && (leaf1_edx & (1u << 19)) &&
          ((leaf1_ebx >> 8) & 255u) == 8 && (leaf7_ebx & (1u << 23));
}
#if PS5_CLFLUSHOPT
static inline bool
ps5_cache_flush_detect(void)
{
   uint32_t a, b, c, d;
   __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                    : "a"(0u), "c"(0u));
   const uint32_t maximum_leaf = a;
   if (maximum_leaf < 7) return false;
   __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                    : "a"(1u), "c"(0u));
   const uint32_t leaf1_ebx = b, leaf1_edx = d;
   __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                    : "a"(7u), "c"(0u));
   return ps5_cache_flush_capable(maximum_leaf, leaf1_ebx, leaf1_edx, b);
}
#endif
static inline bool
ps5_cache_flush_uses_clflushopt(void)
{
#if PS5_CLFLUSHOPT
   /* Detection may harmlessly duplicate at concurrent first use. Atomic
    * publication avoids a data race; no CPUID remains in steady-state loops. */
   static unsigned capability;
   unsigned value = __atomic_load_n(&capability, __ATOMIC_RELAXED);
   if (!value) {
      value = ps5_cache_flush_detect() ? 2u : 1u;
      unsigned expected = 0;
      if (__atomic_compare_exchange_n(&capability, &expected, value, false,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
#if PS5_LIGHT_DIAGNOSTICS
         fprintf(stderr, "[ps5-cache-flush] cpuid_checked=1 selected=%s line_bytes=64\n",
                 value == 2 ? "clflushopt" : "clflush");
#endif
      } else {
         value = expected;
      }
   }
   return value == 2;
#else
   return false;
#endif
}
static inline void
ps5_cache_flush_range(const void *address, size_t bytes)
{
   const uintptr_t begin = (uintptr_t)address;
   const uintptr_t first = begin & ~(uintptr_t)63;
   const size_t prefix = begin - first;
   const size_t lines = bytes / 64 + (bytes % 64 + prefix + 63) / 64;
#if PS5_CLFLUSHOPT
   if (bytes && ps5_cache_flush_uses_clflushopt()) {
      /* Match Mesa util_flush_range: order preceding stores before issuing
       * weakly ordered flushes, and retain the existing full final fence. */
      __asm__ volatile("mfence" ::: "memory");
      for (size_t line = 0; line < lines; ++line)
         __asm__ volatile("clflushopt (%0)" : : "r"(first + line * 64) : "memory");
   } else
#endif
   {
      for (size_t line = 0; bytes && line < lines; ++line)
         __asm__ volatile("clflush (%0)" : : "r"(first + line * 64) : "memory");
   }
   __asm__ volatile("mfence" ::: "memory");
}
#endif
