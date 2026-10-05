# Release validation

## v1.0.0

Tested on PS5 firmware 9.00 with kstuff and ShadowMount, installed on M.2 as `PPSA99630`.

The user confirmed:

- Normal New Game opening and Sony native keyboard name entry.
- The keyboard stays open, accepts the name, and allows continuing character creation.
- Race, face, hair and other appearance controls work with the controller layout.
- Basic save/load and normal visuals after loading the save.
- Larger dialogs at 150% interface scale.
- Normal visuals with the separately installed Morrowind Optimization Patch 1.18 core.

Earlier console tests confirmed movement, camera controls, world rendering, inventory, interaction and audio. A complete playthrough, all quest/combat combinations, and other firmware/loader combinations have not been tested.

The executable SHA-256 is `e9daec8edc9402bd72c1df73e64e9358130ecc121b2293f086ede7f3c2d748c2`, matching the console-tested build. The release uses the same GUI settings, with original game data and no enabled optional-mod path in the default configuration.

## Automated checks

Native packaging and SELF integrity checks passed. The IME regression test runs with AddressSanitizer and UndefinedBehaviorSanitizer and SDL's real text conversion. It covers initialization/open failures, duplicate requests, Unicode across event boundaries, cancel/result failures, temporary focus loss and shutdown. These tests supplement console testing.

## Rendering and performance

Original textures, 1080p, 4096-unit view distance, VSync and a 60 FPS engine cap are the defaults. The graphics driver is unchanged from beta.1: SHA-256 `be0d0c3a033043453ad1429d0baee174ebcd275024b863ddbee5097091f17dbb`.

Earlier measurements were approximately 30 FPS in a stationary exterior scene. A previous indoor/outdoor transition ran at about 20 FPS after returning outside; lighter interior views reached about 60 FPS. These measurements do not establish a locked 60 FPS result or a performance improvement from the optional mod.

## Known limits

- HD texture replacements produced flickering and remain disabled by default.
- Native block-compressed texture sampling remains unresolved; expanded texture data increases memory use.
- The known pre-context `glGetString` warning remains.
- Game data and optional mods must be supplied separately.
