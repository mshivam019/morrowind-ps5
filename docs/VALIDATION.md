# Beta validation

## Included build

The v0.1.0-beta.1 executable and runtime are the console-tested `build/final-performance/dist/PPSA99630` binaries. The release configuration restores the normal main menu (`skip-menu=0`) and removes the Seyda Neen demo override. New Game follows the original game scripts. New Game invokes the original OpenMW startup path rather than the development demo shortcut.

- Original textures, 1080p, 4096-unit view distance, VSync and a 60 FPS engine cap.
- Native submission coalescing, asynchronous batch overlap, marker completion and CPUID-gated CLFLUSHOPT enabled.
- Detailed profiling, light diagnostic counters and on-screen FPS overlay disabled.
- Final driver archive SHA-256: `be0d0c3a033043453ad1429d0baee174ebcd275024b863ddbee5097091f17dbb`.

## Console evidence

The final build launched successfully and completed a 40-second capture without new presentation/assertion failures. The measured stationary outdoor cadence remained approximately 30 FPS. Controller movement/turning, characters, water, textures and menus were visually confirmed during the test round.

Comparisons used 780 completed frames after startup. The baseline and several renderer candidates averaged approximately 29.97 FPS in the stationary exterior scene. Submission changes improved qualification/correctness but did not establish a 60 FPS result.

An earlier indoor/outdoor transition completed, but the outdoor view after returning ran at approximately 20 FPS. That route has not been revalidated on the final build. Lighter interior views reached approximately 60 FPS in earlier development tests; this is not a full-game performance claim.

The latest recording shows launch from the home screen, movement, inventory and interaction. Its 1080p60 encoding describes the recording, not engine frame rate.

## Known limits

- Normal main menu and New Game restored; the release no longer forces the development start cell.
- Full progression, combat coverage, save/load persistence and a complete playthrough are unverified.
- Additional firmware, controller and native-title loader combinations are unverified.
- HD textures produced flickering; removing that data root while keeping the executable resolved the reported flickering.
- Native block-compressed texture sampling remains unresolved; expanded texture data increases memory use.
- The known pre-context `glGetString` warning remains.

Host regressions exercise submission/lifetime guards and rendering helpers. They cannot establish console visual correctness or compatibility.
