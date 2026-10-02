/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5_AGC_COALESCE_H
#define PS5_AGC_COALESCE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PS5_AGC_COALESCE_MAX_WORDS 65536u
#define PS5_AGC_COALESCE_MAX_STREAMS 256u
struct ps5_agc_coalesce_stream {
   const uint32_t *words;
   uint32_t count;
   const void *owner;
   size_t owner_bytes;
   const volatile uint32_t *marker;
   uint32_t expected;
};
static inline bool
ps5_agc_coalesce_contains(const void *owner, size_t bytes,
                          const void *address, size_t span)
{
   uintptr_t base = (uintptr_t)owner, at = (uintptr_t)address;
   return owner && bytes && bytes <= UINTPTR_MAX - base && at >= base &&
      at - base <= bytes && span <= bytes - (at - base);
}
/* Only straight-line packets used by the native graphics path are accepted.
 * Indirect-buffer execution, conditional execution and unknown opcodes fall
 * back to separate submissions; no packet is rewritten or removed. */
static inline bool
ps5_agc_coalesce_stream_valid(const struct ps5_agc_coalesce_stream *stream)
{
   if (!stream || !stream->words || !stream->count ||
       stream->count > PS5_AGC_COALESCE_MAX_WORDS ||
       ((uintptr_t)stream->words & 3u) ||
       !ps5_agc_coalesce_contains(stream->owner, stream->owner_bytes,
          stream->words, (size_t)stream->count * sizeof(uint32_t)) ||
       !ps5_agc_coalesce_contains(stream->owner, stream->owner_bytes,
          (const void *)stream->marker, sizeof(*stream->marker)))
      return false;
   bool marker_seen = false;
   for (uint32_t at = 0; at < stream->count;) {
      uint32_t header = stream->words[at], type = header >> 30;
      uint32_t count;
      if (type == 2) {
         if (header != UINT32_C(0x80000000)) return false;
         count = 1;
      } else if (type == 0 || type == 3) {
         count = ((header >> 16) & 0x3fffu) + 2u;
      } else return false;
      if (count > stream->count - at) return false;
      if (type == 3) {
         unsigned opcode = (header >> 8) & 0xffu;
         if (header & 1u) return false; /* Predicated execution is unqualified. */
         if ((opcode == 0x27 && count != 6) ||
             (opcode == 0x2d && count != 3) ||
             (opcode == 0x35 && count != 5)) return false;
         if (opcode == 0x27 || opcode == 0x2d || opcode == 0x35)
            marker_seen = false; /* Completion must follow the final draw. */
         switch (opcode) {
         case 0x10: /* NOP */
         case 0x13: /* INDEX_BUFFER_SIZE */
         case 0x26: /* INDEX_BASE */
         case 0x27: /* DRAW_INDEX_2 */
         case 0x28: /* CONTEXT_CONTROL */
         case 0x2a: /* INDEX_TYPE */
         case 0x2d: /* DRAW_INDEX_AUTO */
         case 0x2f: /* NUM_INSTANCES */
         case 0x35: /* DRAW_INDEX_OFFSET_2 */
         case 0x46: /* EVENT_WRITE */
         case 0x51: /* CONTEXT_REG_RMW */
         case 0x58: /* ACQUIRE_MEM */
         case 0x68: /* SET_CONFIG_REG */
         case 0x69: /* SET_CONTEXT_REG */
         case 0x76: /* SET_SH_REG */
         case 0x79: /* SET_UCONFIG_REG */
         case 0x7a: /* SET_UCONFIG_REG_INDEX */
         case 0x9b: /* SET_SH_REG_INDEX */
            break;
         case 0x49: { /* RELEASE_MEM: standard GFX9+ eight-dword packet. */
            if (count != 8) return false;
            uint64_t address = (uint64_t)stream->words[at + 3] |
               ((uint64_t)stream->words[at + 4] << 32);
            if (address == (uint64_t)(uintptr_t)stream->marker &&
                stream->words[at + 5] == stream->expected &&
                (stream->words[at + 1] & 0xffu) == 0x28u &&
                ((stream->words[at + 2] >> 29) & 7u) == 1u &&
                ((stream->words[at + 2] >> 16) & 3u) <= 1u)
               marker_seen = true;
            break;
         }
         case 0x63: /* LOAD_SH_REG */
         case 0x64: /* LOAD_CONFIG_REG */
         case 0x9f: { /* LOAD_SH_REG_INDEX: existing native table validation. */
            if (count < 5) return false;
            uint64_t address = (uint64_t)stream->words[at + 1] |
               ((uint64_t)stream->words[at + 2] << 32);
            uint64_t bytes = (uint64_t)stream->words[at + 4] * 8u;
            if (bytes > SIZE_MAX || !ps5_agc_coalesce_contains(
                  stream->owner, stream->owner_bytes,
                  (const void *)(uintptr_t)address, (size_t)bytes))
               return false;
            break;
         }
         default: return false;
         }
      }
      at += count;
   }
   return marker_seen;
}
/* Validation completes before any output write. Original owners, tables and
 * markers must remain alive until the caller verifies every GPU marker. */
static inline bool
ps5_agc_coalesce(const struct ps5_agc_coalesce_stream *streams, unsigned count,
                 uint32_t *destination, size_t capacity, uint32_t *written)
{
   if (written) *written = 0;
   if (!streams || count < 2 || count > PS5_AGC_COALESCE_MAX_STREAMS || !destination || !written ||
       ((uintptr_t)destination & 255u) || capacity > PS5_AGC_COALESCE_MAX_WORDS)
      return false;
   size_t total = 0;
   for (unsigned i = 0; i < count; ++i) {
      if (!ps5_agc_coalesce_stream_valid(&streams[i]) ||
          streams[i].count > capacity - total) return false;
      total += streams[i].count;
   }
   uintptr_t begin = (uintptr_t)destination;
   size_t bytes = total * sizeof(uint32_t);
   if (bytes > UINTPTR_MAX - begin) return false;
   for (unsigned i = 0; i < count; ++i) {
      uintptr_t owner = (uintptr_t)streams[i].owner;
      if (begin < owner + streams[i].owner_bytes && owner < begin + bytes)
         return false;
   }
   size_t at = 0;
   for (unsigned i = 0; i < count; ++i) {
      memcpy(destination + at, streams[i].words,
             (size_t)streams[i].count * sizeof(uint32_t));
      at += streams[i].count;
   }
   *written = (uint32_t)total;
   return true;
}
#endif
