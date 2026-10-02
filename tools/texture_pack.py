"""Offline MET archive validation shared by staging, packaging and transfer."""
import hashlib
import json
from pathlib import Path, PurePosixPath
import struct

ROOT = Path(__file__).resolve().parents[1]
PACK = 'MET6.1-2K'
SUPPORTED_PACKS = ('MET6.1', PACK)
DEFAULT_STAGE = ROOT / 'game-mods' / PACK
REMOTE_DATA = '/app0/assets/Mods/' + PACK
TITLE = 'PPSA99630'
REMOTE_TITLE = '/mnt/ext1/etaHEN/games/' + TITLE


def safe_relative(name):
    path = PurePosixPath(name)
    if not name or '\\' in name or any(ord(c) < 32 or ord(c) == 127 for c in name) or path.is_absolute() or any(
            part in ('', '.', '..') for part in name.split('/')):
        raise ValueError(f'Unsafe texture path: {name!r}')
    if path.parts[0] != 'textures' or path.suffix.lower() != '.dds':
        raise ValueError(f'Unexpected non-texture file: {name!r}')
    return path.as_posix()


def sha256(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def inspect_dds(header, size):
    if len(header) < 128 or header[:4] != b'DDS ':
        raise ValueError('Invalid DDS header')
    fields = struct.unpack_from('<31I', header, 4)
    if fields[0] != 124 or fields[18] != 32:
        raise ValueError('Invalid DDS header sizes')
    height, width, depth, mip_count = fields[2], fields[3], fields[5], fields[6] or 1
    if not width or not height or depth > 1 or fields[27] & (0x200 | 0x200000):
        raise ValueError('Only ordinary 2D DDS textures are supported')
    if mip_count > max(width, height).bit_length():
        raise ValueError('DDS mip count exceeds dimensions')
    flags, fourcc, bits = fields[19], header[84:88], fields[21]
    blocks = {b'DXT1': 8, b'DXT3': 16, b'DXT5': 16}
    if flags & 4:
        if fourcc not in blocks:
            raise ValueError(f'Unsupported DDS FourCC {fourcc!r}')
        fmt = fourcc.decode('ascii')
        expected = sum(max(1, ((width >> level) + 3) // 4) *
                       max(1, ((height >> level) + 3) // 4) * blocks[fourcc]
                       for level in range(mip_count))
    elif flags & 0x40 and bits in (24, 32):
        # OSG DDS reader's supported 24/32-bit RGB table (not merely bit count).
        rgb_masks = {
            24: {(0xff0000, 0xff00, 0xff, 0), (0xff, 0xff00, 0xff0000, 0)},
            32: {(0xff0000, 0xff00, 0xff, 0xff000000),
                 (0xff0000, 0xff00, 0xff, 0),
                 (0xff, 0xff00, 0xff0000, 0xff000000),
                 (0xff, 0xff00, 0xff0000, 0),
                 (0x3ff, 0xffc00, 0x3ff00000, 0xc0000000),
                 (0x3ff00000, 0xffc00, 0x3ff, 0xc0000000)},
        }
        if tuple(fields[22:26]) not in rgb_masks[bits]:
            raise ValueError('Unsupported DDS RGB channel masks')
        fmt = 'RGB' + str(bits)
        expected = sum(max(1, width >> level) * max(1, height >> level) * (bits // 8)
                       for level in range(mip_count))
    else:
        raise ValueError(f'Unsupported DDS pixel flags/bits {flags:#x}/{bits}')
    if size < 128 + expected:
        raise ValueError(f'Truncated DDS mip payload: {size} < {128 + expected}')
    return dict(format=fmt, width=width, height=height, mip_count=mip_count,
                complete_mipchain=mip_count == max(width, height).bit_length())


def load_manifest(stage=DEFAULT_STAGE, verify=False):
    stage = Path(stage).resolve()
    manifest = json.loads((stage / 'manifest.json').read_text())
    if manifest.get('schema') != 1 or manifest.get('pack') not in SUPPORTED_PACKS:
        raise ValueError('Unrecognized texture manifest')
    seen = set()
    for item in manifest['files']:
        name = safe_relative(item['path'])
        if name.casefold() in seen:
            raise ValueError('Duplicate case-insensitive texture path: ' + name)
        seen.add(name.casefold())
        path = stage / name
        if path.is_symlink() or not path.is_file() or not path.resolve().is_relative_to(stage):
            raise ValueError('Missing/unsafe staged texture: ' + name)
        if path.stat().st_size != item['bytes']:
            raise ValueError('Texture size changed: ' + name)
        if verify and sha256(path) != item['sha256']:
            raise ValueError('Texture hash changed: ' + name)
    if not seen or manifest['total_bytes'] != sum(i['bytes'] for i in manifest['files']):
        raise ValueError('Invalid texture manifest totals')
    return manifest


def config_data_line(pack=PACK):
    # OpenMW searches later data roots first; loose MET textures override BSA data.
    if pack not in SUPPORTED_PACKS:
        raise ValueError('Unsupported texture profile')
    return f'data="/app0/assets/Mods/{pack}"\n'


def cap_dds(blob, maximum=2048):
    """Drop whole top mip levels without re-encoding; normalize RGB row padding."""
    if maximum < 1:
        raise ValueError("DDS size cap must be positive")
    info = inspect_dds(blob[:128], len(blob))
    fields = list(struct.unpack_from('<31I', blob, 4))
    width, height, count = info['width'], info['height'], info['mip_count']
    skip = 0
    while max(max(1, width >> skip), max(1, height >> skip)) > maximum:
        skip += 1
    if skip >= count:
        raise ValueError('DDS lacks a supplied mip level within the requested cap')
    compressed = info['format'].startswith('DXT')
    dimensions = [(max(1, width >> level), max(1, height >> level)) for level in range(count)]
    if compressed:
        block = 8 if info['format'] == 'DXT1' else 16
        sizes = [max(1, (w+3)//4)*max(1, (h+3)//4)*block for w,h in dimensions]
        if len(blob) != 128 + sum(sizes):
            raise ValueError('Ambiguous/trailing compressed DDS payload')
        payload = blob[128 + sum(sizes[:skip]):]
        linear_size = sizes[skip]
    else:
        pixel_bytes = int(info['format'][3:]) // 8
        tight = [w*h*pixel_bytes for w,h in dimensions]
        aligned = [((w*pixel_bytes+3)//4)*4*h for w,h in dimensions]
        if len(blob) == 128 + sum(tight):
            sizes = tight
            strides = [w*pixel_bytes for w,h in dimensions]
        elif len(blob) == 128 + sum(aligned):
            sizes = aligned
            strides = [((w*pixel_bytes+3)//4)*4 for w,h in dimensions]
        else:
            raise ValueError('Ambiguous/trailing RGB DDS row layout')
        if fields[1] & 8 and fields[4] not in (width*pixel_bytes, strides[0]):
            raise ValueError('DDS declared pitch does not match pixel layout')
        offset = 128 + sum(sizes[:skip])
        chunks = []
        for level in range(skip, count):
            w,h = dimensions[level]
            for row in range(h):
                chunks.append(blob[offset + row*strides[level]:offset + row*strides[level]+w*pixel_bytes])
            offset += sizes[level]
        payload = b''.join(chunks)
        linear_size = dimensions[skip][0]*pixel_bytes
    fields[2], fields[3] = dimensions[skip][1], dimensions[skip][0]
    fields[4], fields[6] = linear_size, count-skip
    fields[1] |= 0x1007  # CAPS, HEIGHT, WIDTH, PIXELFORMAT
    fields[1] &= ~(8 | 0x80000 | 0x20000)
    fields[1] |= 0x80000 if compressed else 8
    fields[26] = (fields[26] | 0x1000) & ~(8 | 0x400000)
    if count-skip > 1:
        fields[1] |= 0x20000
        fields[26] |= 8 | 0x400000
    result = b'DDS ' + struct.pack('<31I', *fields) + payload
    inspect_dds(result[:128], len(result))
    return result, skip


def memory_estimate(dds, file_bytes):
    """Current ordinary-2D RGBA8 fallback model; excludes temporary/metadata allocations."""
    width, height, count = dds['width'], dds['height'], dds['mip_count']
    if min(width, height, count) < 1 or count > max(width, height).bit_length() or file_bytes < 128:
        raise ValueError('Invalid DDS memory estimate input')
    payload = file_bytes - 128
    rgba_texels = sum(max(1, width >> level) * max(1, height >> level) * 4
                      for level in range(count))
    # ps5_linear_mip_storage_extent uses ceiling extents; reverse level order
    # does not affect allocation size. Driver rounds each row to256B and total
    # allocation toPS5_DIRECT_ALIGNMENT (16KiB).
    linear = 0
    for level in range(count):
        divisor = 1 << level
        w = max(1, (width + divisor - 1) // divisor)
        h = max(1, (height + divisor - 1) // divisor)
        linear += ((w * 4 + 255) // 256) * 256 * h
    gpu = ((linear + 16383) // 16384) * 16384
    compressed_copy = payload if dds['format'].startswith('DXT') else 0
    return dict(osg_image_payload_bytes=payload,
                mesa_compressed_cpu_bytes=compressed_copy,
                rgba_texel_bytes=rgba_texels,
                rgba_gpu_allocation_bytes=gpu,
                modeled_resident_bytes=payload + compressed_copy + gpu)


def add_memory_accounting(manifest):
    totals = {}
    for item in manifest['files']:
        item['memory'] = memory_estimate(item['dds'], item['bytes'])
        for key, value in item['memory'].items():
            totals[key] = totals.get(key, 0) + value
    largest = max(manifest['files'], key=lambda item: item['memory']['modeled_resident_bytes'])
    manifest['memory_model'] = dict(
        version=1, model='ordinary 2D supplied mip levels, RGBA8 software fallback',
        scope='all textures summed, not expected simultaneous scene residency',
        exclusions='temporary decode/upload buffers, allocator and object metadata, engine/render resources',
        gpu_row_alignment=256, gpu_allocation_alignment=16384,
        totals=totals,
        largest_texture=dict(path=largest['path'], **largest['memory']))
    return manifest
