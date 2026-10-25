# SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
# SPDX-License-Identifier: GPL-2.0-or-later
"""Mede quanto de CPU cada thread do bb-probe usa (nome via GetThreadDescription).
Uso: python cpu_threads.py [segundos_de_amostra]   (o jogo precisa estar aberto)"""
import ctypes
import subprocess
import sys
import time
from ctypes import wintypes

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
TH32CS_SNAPTHREAD = 0x4
THREAD_QUERY_LIMITED_INFORMATION = 0x0800


class THREADENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', wintypes.DWORD), ('cntUsage', wintypes.DWORD), ('th32ThreadID', wintypes.DWORD),
                ('th32OwnerProcessID', wintypes.DWORD), ('tpBasePri', wintypes.LONG),
                ('tpDeltaPri', wintypes.LONG), ('dwFlags', wintypes.DWORD)]


k32.OpenThread.restype = wintypes.HANDLE
k32.GetThreadDescription.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_wchar_p)]


def pid_do_jogo():
    out = subprocess.run(['powershell', '-NoProfile', '-Command',
                          '(Get-Process bb-probe -ErrorAction SilentlyContinue | Select-Object -First 1).Id'],
                         capture_output=True, text=True).stdout.strip()
    return int(out) if out else None


def amostra(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32(); te.dwSize = ctypes.sizeof(te)
    res = {}
    ok = k32.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            h = k32.OpenThread(THREAD_QUERY_LIMITED_INFORMATION, False, te.th32ThreadID)
            if h:
                c, e, kt, ut = (wintypes.FILETIME() for _ in range(4))
                nome = ctypes.c_wchar_p()
                k32.GetThreadDescription(h, ctypes.byref(nome))
                if k32.GetThreadTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt), ctypes.byref(ut)):
                    t = lambda f: (f.dwHighDateTime << 32 | f.dwLowDateTime) / 1e7
                    res[te.th32ThreadID] = (nome.value or '?', t(kt) + t(ut))
                k32.CloseHandle(h)
        ok = k32.Thread32Next(snap, ctypes.byref(te))
    k32.CloseHandle(snap)
    return res


def main():
    seg = float(sys.argv[1]) if len(sys.argv) > 1 else 10
    pid = pid_do_jogo()
    if not pid:
        print('bb-probe não está aberto'); return
    a = amostra(pid); time.sleep(seg); b = amostra(pid)
    linhas = []
    for tid, (nome, t1) in b.items():
        t0 = a.get(tid, (nome, t1))[1]
        linhas.append(((t1 - t0) / seg * 100, nome, tid))
    linhas.sort(reverse=True)
    total = sum(x[0] for x in linhas)
    print(f'CPU total do jogo: {total:.0f}% de um núcleo ({total / 12:.0f}% dos 12 lógicos), {len(linhas)} threads')
    for pct, nome, tid in linhas[:20]:
        print(f'{pct:6.1f}%  {nome[:40]:40} tid={tid}')


if __name__ == '__main__':
    main()
