"""BB_ORDER_FPS=60: illusion0001's GoldHEN "60 FPS Unlock" for The Order 01.02 (TheOrder1886-Orbis.xml,
eboot.bin 0x8507c4 = image 0x4507c4). The game stores its vsync interval in a global,
movl $2, 0x117058c(%rip) at 0x4507be: 2 vblanks per frame (30 FPS); 1 gives 60 FPS.
His note: softlocks occur in chapters 6 and 9 during QTEs; disable the patch to progress."""
import struct


def exec_segments(elf):
    phoff, = struct.unpack_from('<Q', elf, 0x20)
    entry_size, count = struct.unpack_from('<HH', elf, 0x36)
    for index in range(count):
        kind, flags, offset, address, _, size, _, _ = struct.unpack_from(
            '<IIQQQQQQ', elf, phoff + index * entry_size)
        if kind == 1 and flags & 1:
            yield offset, address, size


def elf_offset(elf, target, length):
    for offset, address, size in exec_segments(elf):
        if address <= target and target + length <= address + size:
            return offset + target - address
    raise ValueError(f'FPS: {target:#x} fora de segmento executavel')

# movl $imm32, rel32(%rip): opcode, displacement, then the interval (patched at +6).
INTERVAL = (0x4507be, bytes.fromhex('c7 05 8c 05 17 01'))
# The stores just before it (rdx into 0xc58(%rbx); test r14d; jz; call) locate the
# instruction when another dump has it elsewhere.
ANCHOR = bytes.fromhex('48 89 93 58 0c 00 00 45 85 f6 74 08 48 89 df e8')
ANCHOR_TO_INTERVAL = 0x4507be - 0x4507aa  # past the call rel32
INTERVALS = {'30': 2, '60': 1}


def locate(elf):
    target, opcode = INTERVAL
    start = elf_offset(elf, target, len(opcode) + 4)
    if elf[start:start + len(opcode)] == opcode:
        return target
    found = []
    for offset, address, size in exec_segments(elf):
        segment = elf[offset:offset + size]
        at = segment.find(ANCHOR)
        while at >= 0:
            candidate = address + at + ANCHOR_TO_INTERVAL
            start = offset + at + ANCHOR_TO_INTERVAL
            # Same store into the same global, so the displacement shifts with the code.
            if elf[start:start + 2] == opcode[:2]:
                found.append(candidate)
            at = segment.find(ANCHOR, at + 1)
    if len(found) != 1:
        raise ValueError(f'FPS: intervalo de vsync nao encontrado ({len(found)} ocorrencias); eboot nao suportado')
    print(f'FPS: instrucao deslocada {found[0] - target:+#x} em relacao ao eboot de referencia')
    return found[0]


def fps_writes(elf, mode):
    if mode not in INTERVALS:
        raise ValueError(f'BB_ORDER_FPS: modo nao suportado: {mode} (use 30 ou 60)')
    target = locate(elf)
    start = elf_offset(elf, target, 10)
    current, = struct.unpack_from('<I', elf, start + 6)
    if current not in INTERVALS.values():
        raise ValueError(f'FPS: intervalo inesperado {current} em {target:#x}')
    wanted = INTERVALS[mode]
    print(f'The Order: {mode} FPS (intervalo de vsync {current} -> {wanted})'
          + ('; ATENCAO: QTEs dos capitulos 6 e 9 travam, use 30 FPS para passar' if mode == '60' else ''))
    return [(target + 6, struct.pack('<I', wanted))]
