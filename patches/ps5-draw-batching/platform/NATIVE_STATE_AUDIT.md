# Native cached-draw state audit

The cached draw path keeps its existing 16 KiB clear and flush of work-memory
`[0x4000,0x8000)`. The sparse-flush experiment was removed before packaging.
No reduced state clear/flush candidate is enabled or retained.

Confirmed local-source spans:

- Cached linkage copies 34 eight-byte CX records at 0x5000 and 3 UC records at 0x6000.
- `set_linkage_uc_state` can append NGG state to the UC table; tessellation has
  additional entries/rings and uses the uncached path.
- Native depth/stencil state starts at 0x4700, up to 4 eight-byte records.
- Graphics state starts at 0x6800. Live record count depends on viewports and
  enabled state; its reserved capacity ends at 0x7000.
- Completion marker is four bytes at 0x6ff0 and must be CPU-zeroed and made visible.
- Context records start at 0x7000; shader records start at 0x7800. Submission passes
  their live counts to the firmware indirect-register encoders.
- Command-up words and command-down auxiliary data have separate flushes.
- Diagnostic variants additionally use descriptor/vertex/index/uniform scratch
  near 0x4000..0x4700 and texture scratch at 0xc000.

`validate_indirect_register_tables` proves ownership inside the original work
allocation for recognized LOAD_SH/LOAD_CONFIG/LOAD_SH_INDEX packets. It does
not prove that those tables are the only state reads made by the GPU. Indirect
SH table values and direct SH user-data packets can encode pointers to further
shader-read data. A table-only PM4 scan therefore cannot safely narrow the state
flush. Numerical low-32-bit/shift-by-8 pointer-lookalike checks would be heuristics, not
proof for packed addresses or secondary pointer chains. Unknown packet forms
also require fallback. Complete firmware-generated draw captures were unavailable
offline, and the firmware linking/encoding APIs are opaque.

Clear and flush need separate proofs. Cached hits overwrite known linkage and
live register prefixes, but opaque link_shaders on first cached misses can depend
on output initialization. A smaller clear must prove every later CPU/API read is
initialized. A smaller flush must independently cover every GPU-visible read,
including indirect register tables, shader resource roots and completion storage.
Neither proof follows merely from the other.

Next validation should capture real cached-draw DCB words and retained indirect
tables, plus runtime shader metadata and user-data roots, across ordinary,
geometry, storage/image, indirect UBO, tessellation and diagnostic draws. A source
contract must describe permitted SH register/address forms and all secondary
shader-read spans. Unknown forms must preserve the full-state fallback. The lightweight diagnostic build now reports cached-path preparation counts
and state clear/flush bytes per 120 successful presents, without per-draw timer
calls. Cache hit/miss separation and command-up/down byte counts remain possible
future diagnostics.
