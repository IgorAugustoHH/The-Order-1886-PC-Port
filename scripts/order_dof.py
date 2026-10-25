# SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
# SPDX-License-Identifier: GPL-2.0-or-later
"""Withdrawn global DoF experiment; historical static analysis only.

The render dispatch selects either CC...DoF at 0xcccee0 or the already
registered 'Depth of Field Disabled' (fs_downsample) object at renderer+0xaf18.
Redirect only the enabled arm to that existing disabled arm. This is global
DoF, including cutscenes/photo mode; it does not change FOV, aiming or lights.
Gameplay validation is required to associate DoF with the reported aim blur.
"""
import hashlib
import struct
from pathlib import Path

from order_photo_camera import ELF_SHA256, TITLE_ID, APP_VERSION

PATCH_OFFSET = 0xca49d0
DISABLED_ARM = 0xca49e1
CONTEXT_OFFSET = 0xca49ce
CONTEXT = bytes.fromhex(
    '74 11 49 8d be a0 89 00 00 4c 89 ee e8 01 85 02 00 eb 14 '
    '49 8d be 18 af 00 00 ba 08 00 00 00 4c 89 ee e8 4b 50 0a 00')
REPLACEMENT = b'\xe9' + struct.pack('<i', DISABLED_ARM - PATCH_OFFSET - 5) + b'\x90\x90'


def dof_writes(elf, metadata):
    if metadata.get('TITLE_ID') != TITLE_ID or metadata.get('APP_VER') != APP_VERSION:
        raise ValueError('DoF: teste exclusivo de The Order CUSA00785 01.02')
    if hashlib.sha256(elf).hexdigest() != ELF_SHA256:
        raise ValueError('DoF: eboot diferente da imagem analisada')
    phoff, = struct.unpack_from('<Q', elf, 0x20)
    size, count = struct.unpack_from('<HH', elf, 0x36)
    for index in range(count):
        kind, flags, offset, address, _, length, _, _ = struct.unpack_from(
            '<IIQQQQQQ', elf, phoff + index * size)
        if (kind == 1 and flags & 1 and address <= CONTEXT_OFFSET
                and CONTEXT_OFFSET + len(CONTEXT) <= address + length):
            start = offset + CONTEXT_OFFSET - address
            if elf[start:start + len(CONTEXT)] != CONTEXT:
                raise ValueError('DoF: instrucao ou caminho nativo desativado diverge')
            return [(PATCH_OFFSET, REPLACEMENT)]
    raise ValueError('DoF: caminho fora do segmento executavel')


def compile_dof(game, out):
    raise ValueError('Experimento global DoF retirado a pedido de Igor; investigar mira separadamente')
