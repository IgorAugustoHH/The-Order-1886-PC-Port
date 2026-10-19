"""Opt-in cutscene photo-camera experiment for the verified Order 01.02 image.

Only the camera-control branch in PhotoCamera is bypassed. The native loader
applies the write to its in-memory image; game files and GPU caches stay intact.
Static analysis identifies a control gate, not a gameplay-validated unlock.
"""
import hashlib
import struct
from pathlib import Path

ELF_SHA256 = '528b8d0e1d185790faf4edba702119e64a98bbceb2a7c0fd66d031b0ac27fbe3'
TITLE_ID = 'CUSA00785'
APP_VERSION = '01.02'
CONTROL_GATE = 0x2f7ef7
PATCH_OFFSET = 0x2f7eff
GATE_BYTES = bytes.fromhex('41 f6 85 91 00 00 00 20 0f 85 b7 4b 00 00')
REPLACEMENT = b'\x90' * 6


def camera_writes(elf, metadata):
    """Fail closed on another title, image or branch; return one image-relative write."""
    if metadata.get('TITLE_ID') != TITLE_ID or metadata.get('APP_VER') != APP_VERSION:
        raise ValueError('Modo foto: candidato exclusivo de CUSA00785 01.02')
    if hashlib.sha256(elf).hexdigest() != ELF_SHA256:
        # Another dump of 01.02 can differ outside the gate: the byte check below decides.
        print('Modo foto: eboot com hash diferente; conferindo a instrucao original')
    phoff, = struct.unpack_from('<Q', elf, 0x20)
    entry_size, count = struct.unpack_from('<HH', elf, 0x36)
    for index in range(count):
        kind, flags, offset, address, _, size, _, _ = struct.unpack_from(
            '<IIQQQQQQ', elf, phoff + index * entry_size)
        if kind == 1 and flags & 1 and address <= CONTROL_GATE and CONTROL_GATE + len(GATE_BYTES) <= address + size:
            start = offset + CONTROL_GATE - address
            if elf[start:start + len(GATE_BYTES)] != GATE_BYTES:
                raise ValueError('Modo foto: instrucao original da trava diverge')
            return [(PATCH_OFFSET, REPLACEMENT)]
    raise ValueError('Modo foto: trava fora de segmento executavel')


def compile_camera(game, out):
    from prepare import sfo
    metadata = sfo((Path(game) / 'sce_sys/param.sfo').read_bytes())
    writes = camera_writes((Path(out) / 'eboot.elf').read_bytes(), metadata)
    print('Modo foto EXPERIMENTAL: PhotoCamera, trava de controles ignorada; '
          '1 escrita, 6 bytes; cutscene ainda precisa de validacao')
    return writes
