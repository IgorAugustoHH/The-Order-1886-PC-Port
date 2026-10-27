#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds dist/TOport/ (and dist/TOport-Windows.zip): TOport.exe (the The Order: 1886 launcher, frozen
# with PyInstaller so players need no Python), bb-probe.exe with the MSYS2 CLANG64 DLLs it needs,
# DLSS, the preparation scripts and run.py. No game files: the player points TOport.exe to
# their own dump (CUSA00785 01.02). Run from an MSYS2 CLANG64 shell after `bash build.sh`.
# Freezing uses a Windows Python 3.10+ (WINPYTHON overrides) and the private venv out/pyenv.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
source ./msys2-env.sh
[[ -f out/bb-probe.exe ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }

python=${WINPYTHON:-}
if [[ -z $python ]]; then
    for candidate in /c/Python3*/python.exe "${LOCALAPPDATA:-/c/Users/$USER/AppData/Local}"/Programs/Python/Python3*/python.exe; do
        [[ -x $candidate ]] && python=$candidate
    done
fi
[[ -n $python ]] || { echo 'Need a Windows Python 3 (python.org) or WINPYTHON=path\to\python.exe' >&2; exit 1; }
export USERPROFILE=${USERPROFILE:-$(cygpath -w "/c/Users/$(id -un)")}
if [[ ! -x out/pyenv/Scripts/python.exe ]]; then
    "$python" -m venv out/pyenv
fi
out/pyenv/Scripts/python.exe -m pip install -q --disable-pip-version-check pyinstaller
# The scripts run inside TOport.exe (--script): the standard modules they import come along.
hidden=()
for module in argparse base64 collections hashlib json re shutil struct tempfile xml.etree.ElementTree \
              urllib.request ctypes.wintypes; do
    hidden+=(--hidden-import "$module")
done
out/pyenv/Scripts/python.exe -m PyInstaller --noconfirm --clean --log-level WARN --windowed \
    --name TOport --icon "$(cygpath -w "$PWD/launcher/toport.ico")" --distpath out/pyi-dist --workpath out/pyi-work --specpath out/pyi-work \
    --paths "$(cygpath -w "$PWD/scripts")" "${hidden[@]}" \
    "$(cygpath -w "$PWD/launcher/toport_launcher_win.py")"

dest=out/stage/TOport
rm -rf -- out/stage
mkdir -p "$dest/bin" "$dest/launcher"
cp -r out/pyi-dist/TOport/. "$dest/"
cp launcher/toport.ico launcher/toport.png launcher/toport_256.png launcher/banner.png "$dest/launcher/"
for exe in bb-probe.exe bb-gpu-capabilities.exe; do
    [[ -f out/$exe ]] && llvm-strip --strip-debug -o "$dest/bin/$exe" "out/$exe"
done
ldd "$dest/bin/bb-probe.exe" | awk '/\/clang64\/bin\// {print $3}' | sort -u | while read -r dll; do
    cp -u "$dll" "$dest/bin/"
done
cp launcher/loading_*.bmp "$dest/bin/"
cp -r scripts patches "$dest/"
cp run.py LICENSE "$dest/"
cp packaging/windows/LEIA-ME-TOport.txt "$dest/"
if [[ -f out/bbport_dlss.dll && -f out/nvngx_dlss.dll ]]; then
    cp out/bbport_dlss.dll out/nvngx_dlss.dll "$dest/bin/"
    mkdir -p "$dest/licenses" && cp out/NVIDIA-DLSS-LICENSE.txt "$dest/licenses/"
    cp gpu/dlss_bridge/LICENSE.txt "$dest/licenses/bbport_dlss-LICENSE.txt"
else
    echo "DLSS bridge not built (packaging/windows/build_dlss.sh): no DLSS in this package" >&2
fi
find "$dest" -name __pycache__ -prune -exec rm -r {} +
mkdir -p dist
rm -f dist/TOport-Windows.zip
(cd out/stage && powershell -NoProfile -Command \
    "Compress-Archive -Path TOport -DestinationPath ../../dist/TOport-Windows.zip")

# Refresh dist/TOport, keeping what players create there (saves, settings, prepared game).
play=dist/TOport
running=$(powershell -NoProfile -Command \
    "@(Get-Process | Where-Object { \$_.Path -like '$(cygpath -w "$PWD/$play")\\*' }).Count" | tr -d '\r')
if [[ ${running:-0} != 0 ]]; then
    echo "dist/TOport is in use ($running processes): not refreshed; the zip is ready." >&2
else
    mkdir -p "$play"
    find "$play" -mindepth 1 -maxdepth 1 ! -name user ! -name out ! -name bbport.ini \
        ! -name toport.json ! -name toggles.txt -exec rm -rf {} +
    cp -r "$dest/." "$play/"
fi
du -sh "$dest" dist/TOport-Windows.zip
