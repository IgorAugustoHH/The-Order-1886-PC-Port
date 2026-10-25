#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
# SPDX-License-Identifier: GPL-2.0-or-later
"""Add a NID -> name entry to src/import_names.inc for every symbol name the runtime exports.

runtime_lookup() finds an import by NID -> name -> RuntimeExport table, and import_names.inc only
listed what one game imports. A NID is SHA-1(name + salt), so the entries can be computed for every
name in the tables. Usage: python tools/gen_import_names.py [--check]
"""
import base64
import hashlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INC = ROOT / 'src' / 'import_names.inc'
# {"symbolName",function} pairs in the RuntimeExport tables
EXPORT = re.compile(r'\{\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*,\s*[A-Za-z_][A-Za-z0-9_]*\s*\}')


def nid(name):
    digest = hashlib.sha1(name.encode() + bytes.fromhex('518d64a635ded8c1e6b039b1c3e55230')).digest()[:8][::-1]
    return base64.b64encode(digest).decode().rstrip('=').replace('/', '-')


def main():
    text = INC.read_text(encoding='utf-8')
    known = {m.group(1) for m in re.finditer(r'\{"([A-Za-z0-9+\-]{11})#', text)}
    names = set()
    for source in sorted((ROOT / 'src').glob('runtime*.c')):
        names.update(EXPORT.findall(source.read_text(encoding='utf-8')))
    missing = sorted(n for n in names if nid(n) not in known)
    if '--check' in sys.argv:
        print(f'{len(missing)} exported names without a NID entry')
        return 1 if missing else 0
    if missing:
        block = '/* Computed from the runtime export tables by tools/gen_import_names.py. */\n'
        block += ''.join(f'{{"{nid(n)}#","{n}"}},\n' for n in missing)
        INC.write_text(text.rstrip('\n') + '\n' + block, encoding='utf-8')
    print(f'{len(names)} exported names, {len(missing)} entries added')
    return 0


if __name__ == '__main__':
    sys.exit(main())
