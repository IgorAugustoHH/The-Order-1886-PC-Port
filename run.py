#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Windows counterpart of run.sh: prepares the game image and starts bb-probe.exe.

Same environment variables as run.sh (BB_GAME_DIR, BB_DATA_DIR, BB_FPS, BB_RENDER_RES,
BB_LIVE_RES, BB_PATCHES, BB_MODS_*, BB_USER_DIR, ...). Extra arguments go to bb-probe.
The in-game "Apply and restart" runs this script again through BB_RESTART_COMMAND;
`--after PID` waits for the previous game process to end first (its GPU device and memory).
"""
import ctypes
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

PORT = Path(__file__).resolve().parent


def fail(message):
    print(message, file=sys.stderr)
    sys.exit(1)


def wait_for(pid):
    kernel32 = ctypes.windll.kernel32
    SYNCHRONIZE = 0x00100000
    handle = kernel32.OpenProcess(SYNCHRONIZE, False, pid)
    if handle:
        kernel32.WaitForSingleObject(handle, 30000)
        kernel32.CloseHandle(handle)


def no_console():
    """Started without a console (the launcher): console programs must not open their own."""
    return 0 if ctypes.windll.kernel32.GetConsoleWindow() else subprocess.CREATE_NO_WINDOW


def run_script(name, *args, capture=False):
    # Inside the packaged Bloodborne.exe (PyInstaller) the executable runs scripts itself.
    script = ['--script'] if getattr(sys, 'frozen', False) else []
    command = [sys.executable, *script, str(PORT / 'scripts' / name), *map(str, args)]
    if capture:
        result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, text=True,
                                creationflags=no_console())
        if result.returncode:
            sys.exit(result.returncode)
        return result.stdout
    # stdin given: the output handles are passed explicitly (a windowed Bloodborne.exe child would
    # get none otherwise).
    result = subprocess.run(command, stdin=subprocess.DEVNULL, creationflags=no_console())
    if result.returncode:
        sys.exit(result.returncode)
    return None


def ini_value(path, key):
    try:
        for line in Path(path).read_text(encoding='utf-8', errors='replace').splitlines():
            match = re.fullmatch(rf'{re.escape(key)}=(.*)', line.strip())
            if match:
                return match.group(1)
    except OSError:
        pass
    return None


# TOport (The Order: 1886): the configuration approved in testing (runs 147-161). Variables
# already in the environment win (scripts, A/B tests).
TOPORT_DEFAULTS = {
    # Fixes from the helper's research (faces, particles, aim focus, 64-bit buffer loads).
    'BB_ADDR64_LOADS': '1', 'BB_FACE_DEPTH_FIX': '1', 'BB_FACE_SECONDARY_FIX': '1',
    'BB_FACE_CUTSCENE_FIX': '1', 'BB_PARTICLE_SRT_FIX': '1', 'BB_AIM_FOCUS_SYNC': '1',
    'BB_CAPTURE_DOF_CONSTANTS': '1', 'BB_CAPTURE_FOCUS_DEPTH': '1',
    # Guest memory and readbacks.
    'BB_DIRECT_MEMORY_ACCESS': '1', 'BB_DMA_CLEAN_SKIP': '1', 'BB_DMA_SYNC_GENERATION': '1',
    'BB_DEFERRED_READBACK': '1', 'BB_READBACK_MERGE': 'fixed', 'BB_COPY_GPU_BUFFERS': '0',
    'BB_DEPTH_ALIAS_FIX': '0', 'BB_TOGGLES': '16384',
    # Shaders and pipelines: compiled in the background, cached per game in user/cache.
    'BB_ASYNC_PIPELINES': '1', 'BB_ASYNC_SHADERS': '1', 'BB_ASYNC_COMPUTE': '0',
    'BB_GRAPHICS_SYNC': '0', 'BB_PIPELINE_CACHE': '1', 'BB_DRIVER_PIPELINE_CACHE': '0',
    'BB_COMPILE_THREADS': '2', 'BB_COMPILE_FAIR_QUEUE': '0', 'BB_COMPILE_AGGRESSIVE': '0',
    'BB_PRELOAD_BUDGET_MS': '0',
    # Recording and presentation.
    'BB_FRAMES_AHEAD': '2', 'BB_VK_RECORD_THREAD': '1', 'BB_VK_RECORD_THREADS': '1',
    'BB_VK_SEGMENT_KB': '64', 'BB_ASYNC_SUBMIT': '0', 'BB_EMPTY_IMAGE_QUEUE_FAST': '0',
    'BB_GC_IDLE_SECONDS': '0', 'BB_VMA_BLOCK_MB': '0', 'BB_VMA_DEDICATED_MB': '0',
    'BB_VBLANK_HZ': '0', 'BB_FULLSCREEN': '1', 'BB_JITTER': '0', 'BB_OBJECT_MOTION': '0',
    'BB_FRAME_STATS': '1', 'BB_DMA_TRACE': '1', 'BB_PERM_TRACE': '1', 'BB_TIMEOUT': '0',
    # Third-party Vulkan layers (overlays, recorders) stay out, as in every approved run.
    'VK_LOADER_LAYERS_DISABLE': '~implicit~',
}

# A new bbport.ini starts from the settings of the approved runs (teste-mira/pt-dlss.ini).
TOPORT_INI = '''# bbport settings (in-game menu: Insert)
upscaler=dlss
preset=3
sharpen=1
sharpness=0.30
jitter=0
object_motion=0
show_fps=1
output_res=1920x1080
live_resolution=0
order_aa=quality
order_resolution=retail
game_language=17
photo_camera=1
fast_loading=1
preload_shaders=1
fps_limit=60
order_fps=30
'''


def toport_environment(env, config):
    """The Order settings of bbport.ini (launcher and Insert/Select menu) as the environment
    the renderer, patches.py and the runtime read."""
    if env.get('BB_TOPORT', '1') != '1':
        return
    # "Apply and restart" runs this script again with the previous launch's environment: drop
    # what this function set last time so the menu's new choices in bbport.ini win.
    for key in env.pop('BB_TOPORT_KEYS', '').split(','):
        if key:
            env.pop(key, None)
    before = set(env)
    if not Path(config).is_file():
        Path(config).parent.mkdir(parents=True, exist_ok=True)
        Path(config).write_text(TOPORT_INI, encoding='utf-8')
    setting = lambda key, default: ini_value(config, key) or default
    for key, value in TOPORT_DEFAULTS.items():
        env.setdefault(key, value)
    # Runtime optimization mask of the approved runs (16384: small read-only buffer copies
    # stay off the recording thread), read from a file the runtime watches.
    if not env.get('BB_TOGGLE_FILE'):
        toggles = Path(config).parent / 'toggles.txt'
        toggles.write_text('16384', encoding='utf-8')
        env['BB_TOGGLE_FILE'] = str(toggles)
    env.setdefault('BB_WAIT_TRACE', '1')
    env.setdefault('BB_WAIT_TRACE_FILE', str(Path(config).parent / 'esperas.txt'))
    aa = setting('order_aa', 'quality')
    env.setdefault('BB_UPSCALER', 'none')
    if aa == 'game':
        env.setdefault('BB_ORDER_DLSS', '0')
    else:
        env.setdefault('BB_ORDER_DLSS', '1')
        env.setdefault('BB_DLSS', '1')
        if aa in ('quality', 'balanced', 'performance', 'ultra'):
            env.setdefault('BB_ORDER_DLSS_MODE', aa)
    # 720 and 1080 keep the dump eboot's 16:9 patches (illusion0001); 2160 is the -use4K branch.
    resolution = setting('order_resolution', 'retail')
    # 2160 stays reachable only through bbport.ini/BB_ORDER_RESOLUTION for investigation.
    if resolution in ('retail', '1080', '2160'):  # retail: original 1080p + 2.40:1 camera
        env.setdefault('BB_ORDER_RESOLUTION', resolution)
    if resolution == '2160':
        env.setdefault('BB_GAME_ARGS', '-use4K')
    env.setdefault('BB_LANGUAGE', setting('game_language', '17'))
    env.setdefault('BB_ORDER_PHOTO_CAMERA_UNLOCK', setting('photo_camera', '1'))
    if setting('fast_loading', '1') == '1':
        env.setdefault('BB_COMPUTE_PIPE', '1')
        env.setdefault('BB_COMPUTE_PIPE_PARTS', 'ade')
        env.setdefault('BB_COMMAND_PIPE', '1')
    env.setdefault('BB_FPS_LIMIT', setting('fps_limit', '60'))
    # The game's own frame rate (scripts/order_fps.py): 60 is illusion0001's unlock, which
    # softlocks QTEs in chapters 6 and 9. fps_limit only caps what the game produces.
    env.setdefault('BB_ORDER_FPS', setting('order_fps', '30'))
    # Pipelines of the last two sessions built before the game starts (fewer first-use stalls).
    env.setdefault('BB_PRELOAD_PIPELINES', setting('preload_shaders', '1'))
    env['BB_TOPORT_KEYS'] = ','.join(sorted(set(env) - before))


def find_executable(name):
    """bin/ (packaged), else out/ (built with build.sh)."""
    for candidate in (PORT / 'bin' / name, PORT / 'out' / name):
        if candidate.is_file():
            return candidate
    return None


def main():
    args = sys.argv[1:]
    if len(args) >= 2 and args[0] == '--after':
        wait_for(int(args[1]))
        args = args[2:]
    # As run.sh: relative paths (BB_GAME_DIR, BB_DATA_DIR, fsr4_shaders/) are from the port.
    os.chdir(PORT)
    env = os.environ
    data = Path(env.get('BB_DATA_DIR', PORT))
    out = data / 'out'
    out.mkdir(parents=True, exist_ok=True)
    env.setdefault('BB_CONFIG', str(data / 'bbport.ini'))
    config = env['BB_CONFIG']
    toport_environment(env, config)
    if not env.get('BB_FSR411_DIR') and not (PORT / 'fsr4_411').is_dir() and (data / 'fsr4_411').is_dir():
        env['BB_FSR411_DIR'] = str(data / 'fsr4_411')

    probe = Path(env['BB_PROBE']) if env.get('BB_PROBE') else find_executable('bb-probe.exe')
    if not probe or not probe.is_file():
        fail('bb-probe.exe not found: build it with build.sh (MSYS2 CLANG64) or use a packaged build.')
    # Development builds run from the MSYS2 tree: its CLANG64 DLLs (SDL3, FFmpeg, ...).
    if not (probe.parent / 'SDL3.dll').is_file():
        clang64 = Path(env.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
        if clang64.is_dir():
            env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'

    game = Path(env.get('BB_GAME_DIR', PORT.parent / 'CUSA00785'))
    if not (game / 'eboot.bin').is_file():
        fail(f'No eboot.bin in {game} (set BB_GAME_DIR).')
    original_game = game.resolve()
    game = Path(run_script('mods.py', game, '--out', out,
                           '--mods-dir', env.get('BB_MODS_DIR', data / 'mods'),
                           '--config', env.get('BB_MODS_CONFIG', data / 'mods.json'),
                           '--enabled', env.get('BB_MODS_ENABLED', '1'), capture=True).strip())
    mod_view = game if game.resolve() != original_game else None
    try:
        run_script('prepare.py', game, '--out', out)
        run_script('link_libc.py', game, '--out', out)
        run_script('link_modules.py', game, '--out', out)
        run_script('content_profile.py', game, '--out', out, '--sku', env.get('BB_CONTENT_SKU', 'full'))

        # Patches exist for game version 01.09 only (patches.py applies none to others): other
        # versions keep the game's 30 FPS timing and change resolutions live.
        sys.path.insert(0, str(PORT / 'scripts'))
        from patches import game_app_version
        version = game_app_version(game)
        patched = version in (None, '01.09') or bool(env.get('BB_FORCE_PATCHES'))
        if not patched:
            print(f'Game version {version}: community patches need 01.09; 30 FPS, no effect patches')
        # Sizes chosen below for the previous launch are recomputed after an in-game restart.
        if env.get('BB_AUTO_RENDER_RES') == '1':
            for key in ('BB_RENDER_RES', 'BB_OUTPUT_RES', 'BB_AUTO_RENDER_RES'):
                env.pop(key, None)
        fps = env.get('BB_FPS', 'uncap') if patched else '30'
        scaled_render = scaled_output = None
        if not env.get('BB_RENDER_RES'):
            printed = run_script('patches.py', '--print-scaled', '--settings', config, capture=True).split()
            if len(printed) == 2:
                scaled_render, scaled_output = printed
        live = '0'
        if scaled_output:
            live = env.get('BB_LIVE_RES') or ini_value(config, 'live_resolution') or '0'
            if live == 'auto':
                caps = probe.parent / 'bb-gpu-capabilities.exe'
                result = subprocess.run([str(caps), '--live-resolution'], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                        text=True, creationflags=no_console())
                live = result.stdout.strip() if result.returncode == 0 else '0'
            live = '1' if live == '1' or not patched else '0'
        if live == '1':
            print(f'Output {scaled_output}: live resolution changes (live_resolution=0: startup patch)')
        elif scaled_output:
            env.update(BB_RENDER_RES=scaled_render, BB_OUTPUT_RES=scaled_output, BB_AUTO_RENDER_RES='1')
            env.setdefault('BB_DMEM_MB', '9152')
            print(f'Output {scaled_output}: scene {scaled_render}, direct memory {env["BB_DMEM_MB"]} MiB '
                  '(live_resolution=1: live changes)')
        run_script('patches.py', '--out', out, '--fps', fps, '--extra', env.get('BB_PATCHES', ''),
                   '--settings', config, '--game-dir', game, '--render-res', env.get('BB_RENDER_RES', ''),
                   '--output-res', env.get('BB_OUTPUT_RES', ''),
                   '--patches-dir', env.get('BB_PATCHES_DIR', data / 'patches'),
                   '--patches-config', env.get('BB_PATCHES_CONFIG', data / 'patches.json'))
        if not env.get('BB_VBLANK_HZ'):
            env['BB_VBLANK_HZ'] = {'uncap': '0', '90': '90'}.get(fps, '60')

        # The in-game restart starts this script again once this process is gone.
        # GPU caches (shaders, pipelines) beside the saves; read before main(), so set here.
        user_dir = Path(env.get('BB_USER_DIR', data / 'user')).resolve()
        user_dir.mkdir(parents=True, exist_ok=True)
        env.setdefault('BB_GPU_USER_DIR', str(user_dir))
        this = ['--run'] if getattr(sys, 'frozen', False) else [str(Path(__file__).resolve())]
        restart = [sys.executable, *this, '--after', str(os.getpid()), *args]
        env['BB_RESTART_COMMAND'] = subprocess.list2cmdline(restart)
        command = [str(probe), str(out / 'boot-linked.bin'), '--content-profile', str(out / 'content.bin'),
                   '--patches', str(out / 'patches.bin'), '--app0', str(game),
                   '--user', str(user_dir),
                   '--timeout', env.get('BB_TIMEOUT', '0'), *args]
        capture_wrapper = bool(env.get('BB_RENDERDOC_CMD') or env.get('BB_RENDERDOC_PREFIX'))
        if capture_wrapper:
            sys.path.insert(0, str(PORT.parent))
            from captura_gpu import wrap_command
            command = wrap_command(command, env, PORT)
        # No stdin: an inherited pipe (shells such as Git Bash) cost the game its console output.
        code = subprocess.run(command, stdin=subprocess.DEVNULL, creationflags=no_console()).returncode
        # The game's own exit code (0xC0000409: fail-fast, 0xC0000005: access violation, ...):
        # some sessions ended with nothing else in the log.
        label = 'RenderDoc wrapper exit code (nao e o codigo do jogo)' if capture_wrapper else 'Game exit code'
        print(f'{label} {code} ({code & 0xFFFFFFFF:#010x})', flush=True)
        return code
    finally:
        if mod_view:
            shutil.rmtree(mod_view, ignore_errors=True)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
