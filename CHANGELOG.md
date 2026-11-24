# Changelog

All notable changes to the Ukrainian Mod
are documented in this file.

## [1.0.0] - 2026-11-24

Initial community release for the HiBy R1.

### Added

- Animated translucent glass power menu indicators.
- Quadratic opacity fade.
- 16 ms animation timer.
- Delayed animation for the restart slider.
- Ukrainian ґ and Ґ long-press keyboard variants.
- Host simulator shutdown and restart safeguards.
- Modification documentation.

### Changed

- Left-aligned shutdown and restart labels.
- Redistributed all 33 Ukrainian letters in the T9 layout.
- Four letters per key on T9 keys 2-8.
- Five letters on T9 key 9.
- Shortened the Ukrainian USB wake translation.

### Fixed

- Missing Ukrainian letter Е in the T9 keyboard.
- Missing Ukrainian Ґ in the QWERTY keyboard.
- Tested host simulator shutdown paths.

### Verification

- Desktop simulator: 20/20 functional checks passed.
- MIPS target compilation: passed.
- GLIBC compatibility: passed.
- SquashFS metadata verification: passed.
- OTA integrity verification: passed.
- Kernel modules: 30 checked, zero mismatches.
- CookieCaptainD firmware verification: passed.

### Known limitations

- Physical HiBy R1 testing completed successfully. All tested functions are working correctly.
- Long-press variants use a shared keyboard variant table.
- Full firmware redistribution rights have not been verified.

### Base

CookieCaptainD integration build, November 18, 2026.

Base commit:
225623c94302fc3e3d67a34eab0ca78dc7ab584f
