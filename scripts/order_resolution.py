"""BB_ORDER_RESOLUTION=1080: The Order 01.02 picks 1280x720 unless its (developer) resolution
flags select another size (0x44cedc: flags bit 7 = custom size, bit 8 = 1920x1080, bit 9 = 4K).
Both branches are forced to the 1920x1080 case in the loaded image; files stay intact.
BB_ORDER_SCENE_RECT=WxH (experiment): keeps the 1920x1080 size and shrinks only the rectangle
(x, y, width, height at -0x430..-0x424) the same branch writes."""
import hashlib
import os
import struct

ELF_SHA256 = '528b8d0e1d185790faf4edba702119e64a98bbceb2a7c0fd66d031b0ac27fbe3'
# (image offset, original bytes, replacement)
WRITES_1080 = [
    (0x44cef3, bytes.fromhex('a8 80 75 41'), bytes.fromhex('a8 80 eb 41')),  # always custom size
    (0x44cf38, bytes.fromhex('f6 c4 01 74 3e'), bytes.fromhex('f6 c4 01 90 90')),  # 1920x1080
]
# movl $imm32, -0x428(%rbp) / -0x424(%rbp): the rectangle's width and height
RECT_WIDTH = (0x44cf65, bytes.fromhex('c7 85 d8 fb ff ff 80 07 00 00'))
RECT_HEIGHT = (0x44cf6f, bytes.fromhex('c7 85 dc fb ff ff 38 04 00 00'))
# BB_ORDER_RESOLUTION=2160 (experimental): the game's own 3840x2160 branch (-use4K, flags bit
# 9), reached past the 1080p test, instead of patching sizes (1440p that way gave artifacts).
WRITES_2160 = [
    (0x44cef3, bytes.fromhex('a8 80 75 41'), bytes.fromhex('a8 80 eb 41')),  # always custom size
    (0x44cf38, bytes.fromhex('f6 c4 01 74 3e'), bytes.fromhex('f6 c4 01 eb 3e')),  # skip 1080p
    (0x44cf7b, bytes.fromhex('f6 c4 02 74 46'), bytes.fromhex('f6 c4 02 90 90')),  # 3840x2160
]
# BB_ORDER_RESOLUTION=retail (test): this dump's eboot carries illusion0001's GoldHEN patches
# "Resolution Patch (720p)" (the default branch's 1280x720 at 0x44cefd/0x44cf07) and "16:9
# Aspect Ratio" (1.7778f at 0x44ce8c; his note: native 1080p then has visual issues). Restore
# the retail values in memory: the default branch at 1920x1080 and the 2.40:1 camera (the PS4's
# 1920x800 picture).
RETAIL_ASPECT = (0x44ce86, bytes.fromhex('c7 85 18 fc ff ff 39 8e e3 3f'), 2.4)
RETAIL_SIZE = [(0x44cef7, bytes.fromhex('c7 85 84 fc ff ff 00 05 00 00'), 1920),
               (0x44cf01, bytes.fromhex('c7 85 88 fc ff ff d0 02 00 00'), 1080)]
RECT_Y = (0x44cf5b, bytes.fromhex('c7 85 d4 fb ff ff 00 00 00 00'))  # -0x42c(%rbp)


def elf_offset(elf, target, length):
    phoff, = struct.unpack_from('<Q', elf, 0x20)
    entry_size, count = struct.unpack_from('<HH', elf, 0x36)
    for index in range(count):
        kind, flags, offset, address, _, size, _, _ = struct.unpack_from(
            '<IIQQQQQQ', elf, phoff + index * entry_size)
        if kind == 1 and flags & 1 and address <= target and target + length <= address + size:
            return offset + target - address
    raise ValueError(f'Resolucao: {target:#x} fora de segmento executavel')


def resolution_writes(elf, mode):
    if mode == 'retail':
        if hashlib.sha256(elf).hexdigest() != ELF_SHA256:
            # Another dump of 01.02 can differ outside the patched code: the byte checks below decide.
            print('Resolucao: eboot com hash diferente; conferindo os bytes originais')
        out = []
        target, original, aspect = RETAIL_ASPECT
        start = elf_offset(elf, target, len(original))
        if elf[start:start + len(original)] != original:
            raise ValueError(f'Resolucao: proporcao original diverge em {target:#x}')
        out.append((target + 6, struct.pack('<f', aspect)))
        for target, original, value in RETAIL_SIZE:
            start = elf_offset(elf, target, len(original))
            if elf[start:start + len(original)] != original:
                raise ValueError(f'Resolucao: tamanho padrao diverge em {target:#x}')
            out.append((target + 6, struct.pack('<I', value)))
        print('The Order: valores de varejo restaurados (1920x1080, camera 2.40:1; sem os patches '
              '720p/16:9 do eboot) - TESTE')
        return out
    if mode not in ('1080', '2160'):
        raise ValueError(f'BB_ORDER_RESOLUTION: modo nao suportado: {mode}')
    if hashlib.sha256(elf).hexdigest() != ELF_SHA256:
        # Another dump of 01.02 can differ outside the patched code: the byte checks below decide.
        print('Resolucao: eboot com hash diferente; conferindo os bytes originais')
    out = []
    for target, original, replacement in (WRITES_2160 if mode == '2160' else WRITES_1080):
        start = elf_offset(elf, target, len(original))
        if elf[start:start + len(original)] != original:
            raise ValueError(f'Resolucao: bytes originais divergem em {target:#x}')
        out.append((target, replacement))
    if mode == '2160':
        # The 4K branch with the retail 2.40:1 camera (16:9 at high resolutions breaks, as at 1080p).
        target, original, aspect = RETAIL_ASPECT
        start = elf_offset(elf, target, len(original))
        if elf[start:start + len(original)] != original:
            raise ValueError(f'Resolucao: proporcao original diverge em {target:#x}')
        out.append((target + 6, struct.pack('<f', aspect)))
    print('The Order: resolucao forcada para 3840x2160 (ramo -use4K do jogo, camera 2.40:1, EXPERIMENTAL)'
          if mode == '2160' else 'The Order: resolucao forcada para 1920x1080 (2 escritas)')
    rect = os.environ.get('BB_ORDER_SCENE_RECT')
    if rect:
        # WxH or WxH+Y (vertical offset, e.g. 1920x800+140 for the PS4's 2.40:1 letterbox).
        size, _, y = rect.lower().partition('+')
        w, h = (int(v) for v in size.split('x'))
        fields = [(RECT_WIDTH, w), (RECT_HEIGHT, h)]
        if y:
            fields.append((RECT_Y, int(y)))
        for (target, original), value in fields:
            start = elf_offset(elf, target, len(original))
            if elf[start:start + len(original)] != original:
                raise ValueError(f'Resolucao: instrucao do retangulo diverge em {target:#x}')
            out.append((target + 6, struct.pack('<I', value)))
        print(f'The Order: EXPERIMENTO retangulo {w}x{h} dentro de 1920x1080')
    return out
