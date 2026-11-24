# SonixOS UPT Repacker

Version 0.4.0

A conservative firmware repacking utility for the HiBy R1.

## Purpose

This tool replaces the sonix_player executable inside
an existing compatible SonixOS UPT firmware image.

Unlike the full firmware packer, it does not rebuild
the operating system or replace its system resources.

It preserves:

- Original kernel and kernel modules.
- System libraries and resources.
- Existing translations and fonts.
- File ownership and permissions.
- File modification timestamps.
- Extended attributes, including SELinux attributes.
- Symbolic links and hardlink relationships.
- OTA configuration and compatibility information.

## Supported firmware

Currently supported device: HiBy R1.

The tool accepts only the specifically tested
SonixOS 1.3.0 base firmware.

Expected input firmware SHA256:

    40095e51d2ec112693a85e1341b4e274e3afc766a3153e71c6201e7f13b7ffe6

Other firmware images are intentionally rejected.

The SHA256 restriction must not be removed simply
to force an unsupported firmware image through the tool.

## Requirements

- Linux or Windows Subsystem for Linux (WSL).
- Python 3.11 or newer.
- SquashFS Tools 4.7.5 (tested version).
- xorriso.
- 7z.
- readelf and standard GNU command-line utilities.
- Root privileges.
- At least 1 GiB of available workspace.
- A filesystem capable of preserving SELinux XATTRs,
  symbolic links and hardlinks.

The original verify_upt.sh and image.lock must remain
available alongside the repacker.

## Workspace requirements

Use a compatible native Linux filesystem.

For WSL, create a workspace inside your Linux home
directory rather than under /mnt/c.

    mkdir -p "$HOME/sonix-repack-work"

Some tmpfs filesystems do not correctly enumerate
security.selinux extended attributes.

The repacker checks the workspace before extracting
the firmware and refuses incompatible filesystems.

## Usage

Run the following commands from the repository root.

Only run reviewed and trusted scripts with root privileges.

### Input validation (dry run)

    sudo python3 sonix-packer/repack_existing_upt.py \
      --input original.upt \
      --player sonix-player/sonix_player \
      --output modified.upt \
      --model R1 \
      --workdir "$HOME/sonix-repack-work" \
      --dry-run

No output firmware is generated in dry-run mode.

### Full repacking

    sudo python3 sonix-packer/repack_existing_upt.py \
      --input original.upt \
      --player sonix-player/sonix_player \
      --output modified.upt \
      --model R1 \
      --workdir "$HOME/sonix-repack-work"

A successful build ends with:

    FINAL UPT VERIFICATION PASSED

### Additional options

    --keep-workdir

Preserve intermediate files for further inspection.

    --verbose

Display detailed Python error information.

    --version

Display the repacker version.

    --help

Display command-line usage information.

## Safety checks

The repacker performs:

- Input firmware SHA256 validation.
- ELF architecture validation.
- Dynamic library and GLIBC compatibility checks.
- OTA chunk and MD5 chain validation.
- NAND partition reserve checks.
- Workspace metadata compatibility checks.
- Root filesystem content and metadata verification.
- Kernel and module compatibility verification.
- Final UPT verification using verify_upt.sh.

Existing output files are not overwritten.

## Regression tests

Run the integration tests using the supported base
firmware and a compatible replacement executable:

    sudo bash sonix-packer/tests/test_repack_existing_upt.sh \
      original.upt \
      sonix-player/sonix_player \
      "$HOME/sonix-repack-work"

The tests check:

- Rejection of a corrupted input firmware.
- Rejection of an incompatible ELF architecture.
- Rejection of identical input and output paths.
- Protection against overwriting existing output files.
- Preservation of protected input and output files.

## Verification results

Tested on Ubuntu WSL using SquashFS Tools 4.7.5.

- Full repacking: passed.
- Regression checks: 6 passed, 0 failed.
- Filesystem entries: 2559.
- Preserved SELinux XATTRs: 2559.
- Preserved hardlinks: 412.
- Kernel modules: 30 checked, 0 mismatched.
- RootFS NAND reserve: 20 erase blocks.
- Kernel NAND reserve: 17 erase blocks.

During development, the automatically repacked rootfs
was independently compared with a manually repacked
reference image.

All file contents, permissions, ownership, modification
timestamps, extended attributes and hardlink groups
were identical.

The SquashFS images differed only in their filesystem
creation timestamp.

## Limitations

- Only one specifically tested base firmware is supported.
- Other HiBy models are not supported.
- Other versions of SquashFS Tools have not been tested.
- Physical-device compatibility has not been confirmed.
- Static verification cannot guarantee successful booting.
- Byte-for-byte reproducible ISO output is not guaranteed.

This is an experimental tool, not an official HiBy
firmware utility.

Keep a known-working firmware image and recovery method.

## Licensing

The repacker source code is distributed under GPL-3.0,
consistent with the original Sonix Player project.

Third-party firmware components may have separate
licensing requirements.

Users must supply their own compatible firmware image.

## Credits

Based on the Sonix Player firmware format and verification
tools maintained by CookieCaptainD and the SonixOS community.

Original project:
https://github.com/CookieCaptainD/sonix-player

Repacker development:
https://github.com/navitanh-hash/sonix-player
