#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Conservative HiBy R1 SonixOS UPT player replacement tool."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
from collections import defaultdict

BASE_SHA = '40095e51d2ec112693a85e1341b4e274e3afc766a3153e71c6201e7f13b7ffe6'
BASE_PLAYER_SHA = '3ae89648308cb91548f0ed9a19249e0029e5a255c04467265cd3ba8a5578b153'
CHUNK = 524288
PLAYER = 'usr/bin/sonix_player'
TOOLS = ('xorriso', 'unsquashfs', 'mksquashfs', 'readelf', '7z', 'file', 'strings', 'awk', 'grep', 'sed', 'sort', 'md5sum', 'bash')
ENV = {'PATH': '/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin', 'LC_ALL': 'C', 'TZ': 'UTC'}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def digest(data, kind='sha256'):
    return hashlib.new(kind, data).hexdigest()


def filehash(path):
    with path.open('rb') as f:
        h = hashlib.sha256()
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def run(*args, capture=False):
    args = [str(a) for a in args]
    print('+', ' '.join(args), flush=True)
    return subprocess.run(args, env=ENV, check=True, text=True,
                          stdout=subprocess.PIPE if capture else None).stdout


def parse_manifest(raw):
    sections = {'header': {}}
    current = sections['header']
    for line in raw.decode('ascii').splitlines():
        if not line.strip():
            continue
        key, sep, value = line.partition('=')
        require(sep and key and value, 'Malformed OTA manifest')
        if key == 'img_type':
            require(value in ('kernel', 'rootfs') and value not in sections,
                    'Unexpected or duplicate image type')
            current = sections[value] = {}
        else:
            require(key not in current, 'Duplicate manifest field')
            current[key] = value
    require(sections['header'] == {'ota_version': '0'}, 'Unsupported OTA version')
    for kind, name in [('kernel', 'xImage'), ('rootfs', 'rootfs.squashfs')]:
        s = sections.get(kind, {})
        require(set(s) == {'img_name', 'img_size', 'img_md5'} and s['img_name'] == name,
                'Unexpected manifest image fields')
        require(re.fullmatch(r'[0-9]+', s['img_size']) and int(s['img_size']) > 0,
                'Invalid image size')
        require(re.fullmatch(r'[0-9a-f]{32}', s['img_md5']), 'Invalid image MD5')
    return sections


def budget(size, partition, block, minimum):
    free = partition // block - (size + block - 1) // block
    require(free >= minimum, f'Insufficient NAND reserve: {free} blocks (minimum {minimum})')
    return free


def join_chunks(ota, s):
    name, size, whole = s['img_name'], int(s['img_size']), s['img_md5']
    count = (size + CHUNK - 1) // CHUNK
    chain = ota / f'ota_md5_{name}.{whole}'
    require(list(ota.glob(f'ota_md5_{name}.*')) == [chain], 'Unexpected MD5 chain files')
    hashes = chain.read_text('ascii').splitlines()
    require(len(hashes) == count, 'Wrong MD5 chain length')
    chunks = sorted(ota.glob(name + '.*'))
    require(len(chunks) == count, 'Wrong chunk count')
    previous, data = whole, bytearray()
    for i, p in enumerate(chunks):
        require(p.name == f'{name}.{i:04d}.{previous}', 'Wrong chunk filename or sequence')
        part = p.read_bytes()
        require(len(part) == min(CHUNK, size - i * CHUNK), 'Wrong chunk size')
        previous = digest(part, 'md5')
        require(previous == hashes[i], 'Chunk MD5 mismatch')
        data.extend(part)
    require(len(data) == size and digest(data, 'md5') == whole, 'Whole image mismatch')
    return bytes(data)


def write_chunks(ota, data):
    name = 'rootfs.squashfs'
    whole = previous = digest(data, 'md5')
    hashes = []
    for p in list(ota.glob(name + '.*')) + list(ota.glob('ota_md5_' + name + '.*')):
        p.unlink()
    for i, offset in enumerate(range(0, len(data), CHUNK)):
        part = data[offset:offset + CHUNK]
        (ota / f'{name}.{i:04d}.{previous}').write_bytes(part)
        previous = digest(part, 'md5')
        hashes.append(previous)
    (ota / f'ota_md5_{name}.{whole}').write_text('\n'.join(hashes) + '\n', encoding='ascii')
    return whole


def update_manifest(raw, data):
    pattern = rb'(img_name=rootfs\.squashfs\r?\nimg_size=)\d+(\r?\nimg_md5=)[0-9a-f]{32}'
    result, n = re.subn(pattern, lambda m: m[1] + str(len(data)).encode() + m[2] + digest(data, 'md5').encode(), raw)
    require(n == 1, 'Unsupported manifest format')
    return result


def attrs(path):
    return {k: os.getxattr(path, k, follow_symlinks=False)
            for k in os.listxattr(path, follow_symlinks=False)}


def inventory(root):
    entries, links = {}, defaultdict(list)
    for p in [root, *root.rglob('*')]:
        s = p.lstat()
        rel = p.relative_to(root).as_posix()
        content = None
        if stat.S_ISREG(s.st_mode):
            content = filehash(p)
        elif stat.S_ISLNK(s.st_mode):
            content = os.readlink(p)
        entries[rel] = (s.st_mode, s.st_uid, s.st_gid, s.st_mtime_ns,
                        s.st_rdev, s.st_nlink, attrs(p), content)
        if not stat.S_ISDIR(s.st_mode) and s.st_nlink > 1:
            links[(s.st_dev, s.st_ino)].append(rel)
    return entries, sorted(tuple(sorted(v)) for v in links.values())


def check_tree(before, after, player_hash):
    a, la = before
    b, lb = after
    require(a.keys() == b.keys() and la == lb, 'Filesystem paths or hardlinks changed')
    for name in a:
        expected = a[name]
        if name == PLAYER:
            expected = expected[:-1] + (player_hash,)
        require(expected == b[name], f'Unexpected filesystem difference: {name}')


def iso_inventory(root):
    result = {}
    for p in root.rglob('*'):
        require(not p.is_symlink(), 'Unexpected symlink in ISO')
        if p.is_dir():
            result[p.relative_to(root).as_posix()] = 'directory'
        else:
            require(p.is_file(), 'Unexpected ISO entry type')
            result[p.relative_to(root).as_posix()] = filehash(p)
    return result


def protected(entries):
    return {k: v for k, v in entries.items() if not (
        k == 'ota_v0/ota_update.in' or k.startswith('ota_v0/rootfs.squashfs.')
        or k.startswith('ota_v0/ota_md5_rootfs.squashfs.'))}


def elf_signature(path):
    with path.open('rb') as f:
        header = f.read(52)
    require(len(header) == 52 and header[:7] == b'\x7fELF\x01\x01\x01', 'Not ELF32 little endian')
    typ, machine = struct.unpack_from('<HH', header, 16)
    flags = struct.unpack_from('<I', header, 36)[0]
    require((typ, machine, flags) == (2, 8, 0x70001005), 'Unexpected MIPS ELF type or flags')
    prog = run('readelf', '-lW', path, capture=True)
    interp = re.findall(r'Requesting program interpreter: ([^\]]+)', prog)
    require(interp == ['/lib/ld.so.1'], 'Unexpected ELF interpreter')
    dynamic = run('readelf', '-dW', path, capture=True)
    libs = sorted(re.findall(r'\(NEEDED\).*?\[([^\]]+)\]', dynamic))
    require(libs, 'Missing dynamic libraries')
    require(not re.search(r'\((?:RPATH|RUNPATH)\)', dynamic), 'Unexpected ELF library search path')
    versions = run('readelf', '-VW', path, capture=True)
    glibc = [tuple(map(int, v.split('.'))) for v in re.findall(r'GLIBC_(\d+(?:\.\d+)+)', versions)]
    require(all(v <= (2, 22) for v in glibc), 'Requires GLIBC newer than 2.22')
    require('GLIBC_PRIVATE' not in versions, 'Private GLIBC dependency')
    require(b'[HOST TEST]' not in path.read_bytes(), 'Host test marker found in target binary')
    return libs


def squash_signature(path):
    with path.open('rb') as f:
        h = f.read(96)
    require(len(h) == 96 and h[:4] == b'hsqs', 'Invalid SquashFS header')
    inodes, _, block, fragments = struct.unpack_from('<4I', h, 4)
    comp, log, flags, ids, major, minor = struct.unpack_from('<6H', h, 20)
    require((block, comp, major, minor) == (131072, 3, 4, 0), 'Unsupported SquashFS format')
    return inodes, block, comp, log, flags, ids, major, minor


def verify(packer, upt, player=None):
    cmd = ['bash', packer / 'verify_upt.sh', '--model', 'R1', '--upt', upt,
           '--device', 'HiBy R1', '--lock', packer / 'image.lock']
    if player is not None:
        cmd += ['--player', player]
    run(*cmd)


def extract_iso(source, dest):
    run('xorriso', '-osirrox', 'on', '-indev', source, '-extract', '/', dest)


def writable_iso(root):
    for p in [root, *root.rglob('*')]:
        require(not p.is_symlink(), 'Unexpected ISO symlink')
        p.chmod(p.stat().st_mode | 0o200)


def check_work_filesystem(work):
    """Check metadata support before extracting firmware."""
    minimum = 1024 ** 3
    free = shutil.disk_usage(work).free

    print(f'Available work space: {free / 1024**3:.2f} GiB')

    require(
        free >= minimum,
        'At least 1 GiB of free workspace is required'
    )

    expected = (
        b'unconfined_u:object_r:user_home_t:s0'
        + bytes([0])
    )

    with tempfile.TemporaryDirectory(
        prefix='metadata-test-',
        dir=work
    ) as directory:
        base = Path(directory)
        regular = base / 'testfile'
        folder = base / 'testdir'
        symlink = base / 'testlink'
        hardlink = base / 'testhardlink'

        regular.touch()
        folder.mkdir()
        symlink.symlink_to('testfile')
        os.link(regular, hardlink)

        try:
            for entry in (regular, folder, symlink):
                os.setxattr(
                    entry,
                    'security.selinux',
                    expected,
                    follow_symlinks=False
                )

                names = os.listxattr(
                    entry,
                    follow_symlinks=False
                )

                require(
                    'security.selinux' in names,
                    f'SELinux XATTR is not listed for {entry.name}. '
                    'Use --workdir on a compatible Linux filesystem.'
                )

                actual = os.getxattr(
                    entry,
                    'security.selinux',
                    follow_symlinks=False
                )

                require(
                    actual == expected,
                    f'SELinux XATTR value mismatch: {entry.name}'
                )

        except OSError as exc:
            raise ValueError(
                'Workspace cannot preserve SELinux XATTRs. '
                'Use a compatible native Linux filesystem, '
                'such as the WSL ext4 filesystem.'
            ) from exc

        require(
            regular.stat().st_ino == hardlink.stat().st_ino,
            'Workspace does not preserve hardlinks'
        )

    print('Workspace metadata verification passed')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--input', required=True, type=Path)
    ap.add_argument('--player', required=True, type=Path)
    ap.add_argument('--output', required=True, type=Path)
    ap.add_argument('--model', choices=['R1'], default='R1')
    ap.add_argument('--dry-run', action='store_true', help='Validate inputs only; do not build')
    ap.add_argument(
        '--workdir',
        type=Path,
        required=True,
        help='Parent directory for temporary files; must support Linux metadata and XATTRs'
    )
    ap.add_argument('--keep-workdir', action='store_true')
    ap.add_argument('--verbose', action='store_true', help='Print traceback on errors')
    ap.add_argument(
        '--version',
        action='version',
        version='SonixOS UPT Repacker 0.4.0'
    )
    args = ap.parse_args()
    work = None
    try:
        require(sys.platform.startswith('linux') and os.geteuid() == 0, 'Run on Linux/WSL with sudo for metadata preservation')
        for tool in TOOLS:
            require(shutil.which(tool, path=ENV['PATH']), f'Missing tool: {tool}')
        source, player = args.input.resolve(strict=True), args.player.resolve(strict=True)
        output = args.output.absolute()
        require(source.is_file() and player.is_file(), 'Inputs must be regular files')
        require(not os.path.lexists(output), 'Output already exists; refusing overwrite')
        require(output.parent.is_dir(), 'Output directory does not exist')
        packer = Path(__file__).resolve().parent
        require((packer / 'verify_upt.sh').is_file(), 'Place script next to verify_upt.sh')
        row = next(line.split('|') for line in (packer / 'image.lock').read_text().splitlines() if line.startswith('R1|'))
        rootpart, block, kernelpart = (int(v, 0) for v in row[1:4])
        parent = args.workdir.resolve(strict=True)
        require(parent.is_dir(), 'Work directory must exist')
        work = Path(tempfile.mkdtemp(prefix='sonix-repack-', dir=parent))
        print('Temporary work directory:', work, flush=True)
        check_work_filesystem(work)

        # Snapshot inputs so validation and subsequent operations use identical bytes.
        upt = work / 'input.upt'
        newplayer = work / 'new-player'
        shutil.copyfile(source, upt)
        shutil.copyfile(player, newplayer)
        require(filehash(upt) == BASE_SHA, 'Unsupported base UPT SHA256; only the tested base is accepted')
        newhash = filehash(newplayer)
        verify(packer, upt)
        iso = work / 'iso'
        extract_iso(upt, iso)
        initial_iso = iso_inventory(iso)
        writable_iso(iso)
        ota = iso / 'ota_v0'
        require((ota / 'ota_compatibility.in').read_text().strip() == 'compatibility=hiby_r1_x1600_auto_rf_auto', 'Unexpected R1 compatibility marker')
        raw = (ota / 'ota_update.in').read_bytes()
        manifest = parse_manifest(raw)
        for kind, part, reserve in [('rootfs', rootpart, 8), ('kernel', kernelpart, 4)]:
            free = budget(int(manifest[kind]['img_size']), part, block, reserve)
            print(f'Original {kind}: {free} free erase blocks')
        olddata = join_chunks(ota, manifest['rootfs'])
        kernel = join_chunks(ota, manifest['kernel'])
        oldimage = work / 'original.squashfs'
        oldimage.write_bytes(olddata)
        signature = squash_signature(oldimage)
        root = work / 'root'
        run('unsquashfs', '-strict-errors', '-d', root, oldimage)
        target = root / PLAYER
        require(target.resolve() == target and target.is_file() and target.stat().st_nlink == 1, 'Unexpected player path or hardlink')
        require(filehash(target) == BASE_PLAYER_SHA, 'Unexpected original player')
        require(elf_signature(target) == elf_signature(newplayer), 'Dynamic library dependencies differ')
        require(newhash != BASE_PLAYER_SHA, 'Replacement is identical to original')
        before = inventory(root)
        expected_attrs = {
            'security.selinux':
                b'unconfined_u:object_r:user_home_t:s0' + bytes([0])
        }

        entries = before[0]

        xattr_count = sum(
            len(v[6]) for v in entries.values()
        )

        print('Filesystem entries:', len(entries))
        print('Extracted XATTRs:', xattr_count)

        require(
            len(entries) == 2559,
            'Unexpected number of filesystem entries'
        )

        mismatches = [
            (name, value[6])
            for name, value in entries.items()
            if value[6] != expected_attrs
        ]

        print('Expected XATTRs:', repr(expected_attrs))
        print('XATTR mismatches:', len(mismatches))

        for name, actual in mismatches[:10]:
            print('Mismatch:', name, repr(actual))

        require(
            not mismatches,
            'Original XATTRs missing or changed; try --workdir on a native Linux filesystem'
        )

        print('Original XATTR verification passed: 2559/2559')
        print('Input checks passed; replacement SHA256:', newhash)
        if args.dry_run:
            print('DRY RUN PASSED (no firmware generated)')
            return 0
        s = target.stat()
        with target.open('wb') as f:
            f.write(newplayer.read_bytes())
        os.utime(target, ns=(s.st_atime_ns, s.st_mtime_ns))
        check_tree(before, inventory(root), newhash)
        image = work / 'modified.squashfs'
        run('mksquashfs', root, image, '-comp', 'lzo', '-b', '131072', '-noappend', '-exports', '-xattrs', '-exit-on-error', '-processors', '4')
        require(squash_signature(image) == signature, 'SquashFS structural flags or counts changed')
        newdata = image.read_bytes()
        free = budget(len(newdata), rootpart, block, 8)
        print(f'New rootfs: {free} free erase blocks')
        if free < 16:
            print('WARNING: below recommended 16-block reserve')
        checked = work / 'checked-root'
        run('unsquashfs', '-strict-errors', '-d', checked, image)
        check_tree(before, inventory(checked), newhash)
        write_chunks(ota, newdata)
        newraw = update_manifest(raw, newdata)
        (ota / 'ota_update.in').write_bytes(newraw)
        newmanifest = parse_manifest(newraw)
        require(newmanifest['kernel'] == manifest['kernel'], 'Kernel manifest changed')
        require(protected(initial_iso) == protected(iso_inventory(iso)), 'Unrelated ISO files changed')
        candidate = work / 'candidate.upt'
        run('xorriso', '-as', 'mkisofs', '-J', '-r', '-o', candidate, iso)
        verify(packer, candidate, newplayer)
        finaliso = work / 'checked-iso'
        extract_iso(candidate, finaliso)
        require(iso_inventory(iso) == iso_inventory(finaliso), 'ISO round-trip mismatch')
        finalota = finaliso / 'ota_v0'
        require(join_chunks(finalota, newmanifest['kernel']) == kernel, 'Kernel round-trip mismatch')
        require(join_chunks(finalota, newmanifest['rootfs']) == newdata, 'RootFS round-trip mismatch')
        # Exclusive creation; never overwrite an existing file. Remove partial copy on failure.
        created = False
        try:
            with output.open('xb') as dst:
                created = True
                with candidate.open('rb') as src:
                    shutil.copyfileobj(src, dst)
                dst.flush()
                os.fsync(dst.fileno())
            require(filehash(output) == filehash(candidate), 'Output copy hash mismatch')
            os.chmod(output, 0o644)
            if 'SUDO_UID' in os.environ and 'SUDO_GID' in os.environ:
                os.chown(output, int(os.environ['SUDO_UID']), int(os.environ['SUDO_GID']))
        except BaseException:
            if created:
                output.unlink(missing_ok=True)
            raise
        print('FINAL UPT VERIFICATION PASSED')
        print('Output:', output)
        print('SHA256:', filehash(output))
        print('Static checks only; physical-device compatibility is not guaranteed.')
        return 0
    except (OSError, ValueError, StopIteration, subprocess.CalledProcessError) as exc:
        print('FAIL:', exc, file=sys.stderr)
        if args.verbose:
            import traceback
            traceback.print_exc()
        return 1
    finally:
        if work is not None:
            if args.keep_workdir:
                print('Work directory:', work)
            else:
                shutil.rmtree(work)


if __name__ == '__main__':
    sys.exit(main())
