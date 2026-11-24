# UPT Repacker Changelog

## 0.4.0 - 2026-11-24

First public experimental version.

- Required explicit workspace directory.
- Added workspace filesystem compatibility checks.
- Added SELinux XATTR validation for files,
  directories and symbolic links.
- Added hardlink support verification.
- Added minimum free-space validation.
- Added --version.
- Added integration regression tests.
- Full firmware repacking successfully tested.
- Six regression checks passed.

## 0.3.0

- Corrected exact SELinux XATTR value validation.
- Added XATTR mismatch diagnostics.
- First successful complete firmware repacking.
- Independently verified rootfs equivalence
  against the manually repacked reference image.

## 0.2.0

- Added configurable workspace directory.
- Added strict validation of all 2559 XATTRs.

## 0.1.0

- Initial experimental implementation.
- Input firmware validation.
- ELF compatibility checks.
- SquashFS replacement and verification.
- OTA chunk generation.
- NAND partition budget checks.
- Final UPT verification.
