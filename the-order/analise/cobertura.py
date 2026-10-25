# SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
# SPDX-License-Identifier: GPL-2.0-or-later
"""Cobertura das importações do The Order pelo runtime do BBPort.

Uma importação está coberta se: (1) o NID aparece literal no runtime/shim da GPU, (2) o NID é o
hash de um nome das tabelas RuntimeExport, ou (3) a libc/Fios2 nativa do jogo resolve
(work/out/link.json, "bindings"). O que sobra é a lista de trabalho."""
import json
import re
import sys
from collections import Counter
from pathlib import Path

AQUI = Path(__file__).parent
RAIZ = AQUI.parent
sys.path.insert(0, str(AQUI))
from nomes_nid import nid  # noqa: E402

BB = RAIZ / 'bbport-base'


def main():
    link = json.loads((RAIZ / 'work/out/link.json').read_text())
    resto = [str(x) for x in link['unresolved_imports']]
    fontes = ''.join(p.read_text(encoding='utf-8', errors='ignore') for p in
                     list((BB / 'src').glob('*.c')) + list((BB / 'gpu/shim').glob('*.cpp')))
    literais = set(re.findall(r'"([A-Za-z0-9+\-]{11})#', fontes))
    # funções HLE do shadPS4 registradas na biblioteca da GPU (bbgpu_resolve)
    for p in (BB / 'gpu').rglob('*.cpp'):
        literais |= set(re.findall(r'LIB_FUNCTION\("([A-Za-z0-9+\-]{11})"', p.read_text(encoding='utf-8', errors='ignore')))
    nomes = set(re.findall(r'\{\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*,\s*[A-Za-z_]\w*\s*\}', fontes))
    por_nome = {nid(n): n for n in nomes}
    conhecidos = json.loads((AQUI / 'nomes_faltantes.json').read_text())
    falta = [i for i in resto if i.split('#')[0] not in literais and i.split('#')[0] not in por_nome]
    print(f'importações para o runtime: {len(resto)} | cobertas: {len(resto) - len(falta)} | faltam: {len(falta)}')
    for i in falta:
        print(' ', i, conhecidos.get(i.split('#')[0], '?'))
    (AQUI / 'falta.json').write_text(json.dumps(falta, indent=1))


if __name__ == '__main__':
    main()
