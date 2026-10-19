#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""TOport: launcher for The Order: 1886 on Windows (Tkinter), in the style of the BBPort one.

The player picks the folder of their own dump (CUSA00785, version 01.02) and the settings the
in-game menu (Insert) also edits; they live in bbport.ini next to this program, and
run.py turns them into the port's environment. Launcher-only options (game folder, display
mode) are in toport.json in the same folder. The banner shows the dump's own art
(sce_sys/pic0.png): nothing of the game is shipped with the port.

Frozen with PyInstaller (packaging/windows/package_toport.sh) the same TOport.exe also runs the
game without the window (`--play`), run.py (`--run`) and the preparation scripts (`--script`),
so a packaged port needs no Python installation.
"""
import ctypes
import json
import math
import os
from pathlib import Path
import queue
import runpy
import struct
import subprocess
import sys
import threading
import urllib.request
import webbrowser

FROZEN = getattr(sys, 'frozen', False)
PORT_DIR = Path(sys.executable).resolve().parent if FROZEN else Path(__file__).resolve().parent.parent
DATA_DIR = Path(os.environ.get('BB_DATA_DIR', PORT_DIR))
CONFIG = DATA_DIR / 'bbport.ini'
LAUNCHER_FILE = DATA_DIR / 'toport.json'
LOG_FILE = DATA_DIR / 'last_run.log'
ASSETS = PORT_DIR / 'launcher'
TITLE_ID = 'CUSA00785'
GAME_VERSION = '01.02'
VERSION = '0.1.3'
# Releases (pre-releases included): the launcher offers a newer one on its sidebar.
RELEASES_API = 'https://api.github.com/repos/IgorAugustoHH/The-Order-1886-PC-Port/releases?per_page=5'
RELEASES_PAGE = 'https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/releases'
MAX_LOG_LINES = 6000
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


def default_ui_language():
    """pt for Portuguese Windows, en otherwise."""
    try:
        primary = ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3ff
    except (AttributeError, OSError):
        return 'en'
    return 'pt' if primary == 0x16 else 'en'


def saved_ui_language():
    try:
        value = json.loads(LAUNCHER_FILE.read_text(encoding='utf-8')).get('ui_language')
    except (OSError, ValueError):
        value = None
    return value if value in ('pt', 'en') else default_ui_language()


UI_LANG = saved_ui_language()


def T(pt, en):
    """The launcher's text in its language (Portuguese or English)."""
    return en if UI_LANG == 'en' else pt


# ---------------------------------------------------------------------------------------------
# Command line roles of the frozen executable (as the BBPort launcher).

def attach_stdio():
    """A windowed executable starts without sys.stdout; inherited pipes or a console still exist."""
    import msvcrt
    for name, std in (('stdout', -11), ('stderr', -12)):
        if getattr(sys, name) is not None:
            continue
        stream = None
        handle = ctypes.windll.kernel32.GetStdHandle(std)
        if handle and handle != ctypes.c_void_p(-1).value:
            try:
                stream = open(msvcrt.open_osfhandle(handle, os.O_WRONLY), 'w', encoding='utf-8',
                              errors='replace', buffering=1)
            except OSError:
                stream = None
        setattr(sys, name, stream or open(os.devnull, 'w'))


def run_role(argv):
    """--run [args]: run.py; --script <file> [args]: a preparation script. Returns an exit code."""
    attach_stdio()
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(line_buffering=True)
        except (AttributeError, ValueError):
            pass
    if argv[0] == '--script':
        path, sys.argv = argv[1], argv[1:]
    else:
        path = str(PORT_DIR / 'run.py')
        sys.argv = [path, *argv[1:]]
    # As `python script.py`: the script's own folder first on the import path (scripts/ import
    # each other, e.g. link_libc.py -> prepare.py).
    sys.path.insert(0, str(Path(path).resolve().parent))
    try:
        runpy.run_path(path, run_name='__main__')
    except SystemExit as stop:
        return stop.code if isinstance(stop.code, int) else (0 if stop.code is None else 1)
    return 0


def run_command():
    """The command that starts run.py: this executable when frozen, else Python."""
    return [sys.executable, '--run'] if FROZEN else [sys.executable, str(PORT_DIR / 'run.py')]


# ---------------------------------------------------------------------------------------------
# Settings: bbport.ini (shared with the in-game menu) and toport.json (launcher only).

def AA_MODES():
    return [('game', T('TAA original do jogo', "The game's original TAA")),
            ('dlaa', T('DLSS DLAA (resolução nativa)', 'DLSS DLAA (native resolution)')),
            ('quality', T('DLSS Qualidade', 'DLSS Quality')),
            ('balanced', T('DLSS Equilibrado', 'DLSS Balanced')),
            ('performance', T('DLSS Desempenho', 'DLSS Performance')),
            ('ultra', T('DLSS Ultra Desempenho', 'DLSS Ultra Performance'))]


def RESOLUTIONS():
    return [('retail', T('Original do PS4: 1920 x 800 (2.40:1)', 'PS4 original: 1920 x 800 (2.40:1)')),
            ('1080', T('1920 x 1080 tela cheia (16:9)', '1920 x 1080 full screen (16:9)')),
            ('720', T('1280 x 720 tela cheia (16:9)', '1280 x 720 full screen (16:9)'))]


UI_LANGUAGES = [('pt', 'Português'), ('en', 'English')]
FPS_LIMITS = [('30', '30 FPS'), ('60', '60 FPS'), ('90', '90 FPS'), ('120', '120 FPS')]
def GAME_FPS():
    return [('30', T('30 FPS (original)', '30 FPS (original)')),
            ('60', T('60 FPS (experimental)', '60 FPS (experimental)'))]
# PS4 system language codes (gpu/shim/bbport_settings.h GameLanguages).
LANGUAGES = [('17', 'Português (Brasil)'), ('7', 'Português (Portugal)'), ('1', 'English (US)'),
             ('18', 'English (UK)'), ('3', 'Español (España)'), ('20', 'Español (Latinoamérica)'),
             ('2', 'Français'), ('22', 'Français (Canada)'), ('4', 'Deutsch'), ('5', 'Italiano'),
             ('6', 'Nederlands'), ('8', 'Русский'), ('16', 'Polski'), ('19', 'Türkçe'),
             ('12', 'Suomi'), ('13', 'Svenska'), ('14', 'Dansk'), ('15', 'Norsk'),
             ('21', 'العربية (Arabic)'), ('0', '日本語 (Japanese)'), ('9', '한국어 (Korean)'),
             ('10', '中文 繁體 (Traditional Chinese)'), ('11', '中文 简体 (Simplified Chinese)')]
def DISPLAY_MODES():
    return [('1', T('Tela cheia sem bordas', 'Borderless full screen')),
            ('2', T('Tela cheia exclusiva', 'Exclusive full screen')), ('0', T('Janela', 'Window'))]
INI_DEFAULTS = {'order_aa': 'quality', 'order_resolution': 'retail', 'fps_limit': '60',
                'game_language': '17', 'photo_camera': '1', 'fast_loading': '1', 'show_fps': '1',
                'preload_shaders': '1', 'order_fps': '30'}
INI_FLAGS = ('photo_camera', 'fast_loading', 'show_fps', 'preload_shaders')
APP_DEFAULTS = {'game_dir': str(PORT_DIR.parent / TITLE_ID), 'display': '1',
                'close_on_play': False, 'ui_language': UI_LANG}


def read_ini():
    values = dict(INI_DEFAULTS)
    try:
        for line in CONFIG.read_text(encoding='utf-8', errors='replace').splitlines():
            key, sep, value = line.strip().partition('=')
            if sep and not key.startswith('#'):
                values[key] = value
    except OSError:
        pass
    if values.get('order_resolution') not in ('retail', '720', '1080'):
        values['order_resolution'] = 'retail'  # 1440p/4K tests left out of the launcher
    return values


def write_ini(changes):
    """Updates the given keys, keeping every other line (run.py writes the defaults)."""
    try:
        lines = CONFIG.read_text(encoding='utf-8', errors='replace').splitlines()
    except OSError:
        lines = ['# bbport settings (in-game menu: Insert)']
    pending = dict(changes)
    for i, line in enumerate(lines):
        key = line.partition('=')[0].strip()
        if key in pending:
            lines[i] = f'{key}={pending.pop(key)}'
    lines += [f'{key}={value}' for key, value in pending.items()]
    CONFIG.parent.mkdir(parents=True, exist_ok=True)
    CONFIG.write_text('\n'.join(lines) + '\n', encoding='utf-8')


def read_launcher():
    values = dict(APP_DEFAULTS)
    try:
        values.update(json.loads(LAUNCHER_FILE.read_text(encoding='utf-8')))
    except (OSError, ValueError):
        pass
    return values


def write_launcher(values):
    LAUNCHER_FILE.parent.mkdir(parents=True, exist_ok=True)
    LAUNCHER_FILE.write_text(json.dumps(values, indent=1, ensure_ascii=False), encoding='utf-8')


def read_sfo(path):
    """param.sfo key/value pairs (TITLE_ID, APP_VER, TITLE...)."""
    data = path.read_bytes()
    if data[:4] != b'\0PSF':
        raise ValueError('param.sfo inválido')
    key_table, data_table, count = struct.unpack_from('<III', data, 8)
    values = {}
    for i in range(count):
        key_off, fmt, length, _, data_off = struct.unpack_from('<HHIII', data, 20 + i * 16)
        key = data[key_table + key_off:data.index(b'\0', key_table + key_off)].decode()
        raw = data[data_table + data_off:data_table + data_off + length]
        values[key] = struct.unpack('<I', raw[:4])[0] if fmt == 0x0404 else \
            raw.split(b'\0')[0].decode('utf-8', 'replace')
    return values


def check_game(folder):
    """(ok, message) for a game folder: the port is verified on CUSA00785 01.02 only."""
    folder = Path(folder or '.')
    if not (folder / 'eboot.bin').is_file():
        return False, T('Escolha a pasta do seu dump (CUSA00785) em "Jogo".', 'Choose your dump folder (CUSA00785) in "Game & language".')
    try:
        sfo = read_sfo(folder / 'sce_sys' / 'param.sfo')
    except (OSError, ValueError) as error:
        return False, T(f'Não foi possível ler sce_sys/param.sfo ({error}).', f'Could not read sce_sys/param.sfo ({error}).')
    title, version = sfo.get('TITLE_ID'), sfo.get('APP_VER')
    if title != TITLE_ID:
        return False, T(f'Este dump é {title}; o TOport roda The Order: 1886 ({TITLE_ID}).',
                        f'This dump is {title}; TOport runs The Order: 1886 ({TITLE_ID}).')
    if version != GAME_VERSION:
        return False, T(f'Versão {version}: o TOport foi verificado na {GAME_VERSION} '
                        '(as correções do executável recusam outras versões).',
                        f'Version {version}: TOport is verified on {GAME_VERSION} '
                        '(the executable fixes refuse other versions).')
    return True, T(f'The Order: 1886 · {TITLE_ID} · versão {version}', f'The Order: 1886 · {TITLE_ID} · version {version}')


def game_environment(launcher):
    env = dict(os.environ)
    env['BB_GAME_DIR'] = launcher['game_dir']
    env['BB_DATA_DIR'] = str(DATA_DIR)
    env['BB_CONFIG'] = str(CONFIG)
    env['BB_FULLSCREEN'] = launcher.get('display', '1')
    return env


class GameJob:
    """A Windows job holding run.py and everything it starts: bb-probe.exe and the launches made
    by the in-game restart (no longer descendants of the first process)."""

    def __init__(self, process):
        kernel32 = ctypes.windll.kernel32
        kernel32.CreateJobObjectW.restype = ctypes.c_void_p
        self.handle = kernel32.CreateJobObjectW(None, None)
        if self.handle:
            kernel32.AssignProcessToJobObject(ctypes.c_void_p(self.handle),
                                              ctypes.c_void_p(int(process._handle)))

    def terminate(self):
        if self.handle:
            ctypes.windll.kernel32.TerminateJobObject(ctypes.c_void_p(self.handle), 1)

    def close(self):
        if self.handle:
            ctypes.windll.kernel32.CloseHandle(ctypes.c_void_p(self.handle))
            self.handle = None


def play_without_window():
    """--play: starts the game with the saved settings and waits for it (shortcuts, Steam)."""
    launcher = read_launcher()
    ok, message = check_game(launcher['game_dir'])
    if not ok:
        ctypes.windll.user32.MessageBoxW(None, message, 'TOport', 0x10)
        return 1
    with open(LOG_FILE, 'w', encoding='utf-8', errors='replace') as log:
        return subprocess.run(run_command(), cwd=PORT_DIR, env=game_environment(launcher),
                              stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                              creationflags=NO_WINDOW).returncode


# ---------------------------------------------------------------------------------------------
# The window (layout and look of the BBPort launcher).

BG, PANEL, CARD, LINE = '#0e0c0b', '#151210', '#1c1815', '#2e2722'
TEXT, MUTED, GOLD, BLOOD, BLOOD_HI = '#e9e2d6', '#9a8f80', '#c8a96a', '#7c1717', '#9e2222'


class Launcher:
    def __init__(self, root):
        import tkinter as tk
        from tkinter import filedialog, messagebox, ttk
        self.tk, self.ttk, self.filedialog, self.messagebox = tk, ttk, filedialog, messagebox
        self.root = root
        self.ini = read_ini()
        self.app = read_launcher()
        self.vars = {}
        self.process = self.job = None
        self.output = queue.Queue()
        self.ui_calls = queue.Queue()
        self.gpu_text = T('• Verificando a placa de vídeo…', '• Checking the graphics card…')
        self.banner_source = self.banner_scaled = None
        self.banner_size = None
        root.title(f'The Order: 1886 — TOport {VERSION}')
        root.configure(bg=BG)
        self.dpi = root.winfo_fpixels('1i') / 96.0
        root.geometry(f'{self.px(1100)}x{self.px(720)}')
        root.minsize(self.px(960), self.px(640))
        self.set_icon()
        self.style()
        self.build()
        self.show('play')
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.after(100, self.drain_output)
        threading.Thread(target=self.detect_gpu, daemon=True).start()
        threading.Thread(target=self.check_update, daemon=True).start()

    def px(self, size):
        return int(size * self.dpi)

    def set_icon(self):
        try:
            self.root.iconbitmap(default=str(ASSETS / 'toport.ico'))
        except self.tk.TclError:
            try:
                self.icon = self.tk.PhotoImage(file=str(ASSETS / 'toport_256.png'))
                self.root.iconphoto(True, self.icon)
            except (self.tk.TclError, OSError):
                pass

    # ---- look --------------------------------------------------------------------------------
    def check_images(self):
        """16 px (DPI-scaled) check box images: the clam theme draws a cross."""
        tk = self.tk
        n = max(14, int(16 * self.dpi))
        images = []
        for checked in (False, True):
            image = tk.PhotoImage(width=n + self.px(8), height=n)
            image.put(GOLD if checked else '#5a4c40', to=(0, 0, n, n))
            image.put(BLOOD if checked else CARD, to=(1, 1, n - 1, n - 1))
            if checked:
                x0, y0, x1, y1, x2, y2 = .22 * n, .52 * n, .42 * n, .72 * n, .78 * n, .28 * n
                width = max(1, round(n / 9))
                for t in range(40):
                    f = t / 39
                    for x, y in ((x0 + (x1 - x0) * f, y0 + (y1 - y0) * f),
                                 (x1 + (x2 - x1) * f, y1 + (y2 - y1) * f)):
                        image.put('#f4ece0', to=(int(x), int(y), int(x) + width, int(y) + width))
            images.append(image)
        self.check_off, self.check_on = images

    def style(self):
        ttk = self.ttk
        s = ttk.Style(self.root)
        s.theme_use('clam')
        # The element is created once per Tk interpreter (a language switch rebuilds the window):
        # its images stay referenced on the root.
        if not hasattr(self.root, 'toport_checks'):
            self.check_images()
            self.root.toport_checks = (self.check_off, self.check_on)
            s.element_create('To.indicator', 'image', self.check_off, ('selected', self.check_on), sticky='')
        s.layout('TCheckbutton', [('Checkbutton.padding', {'sticky': 'nswe', 'children': [
            ('To.indicator', {'side': 'left', 'sticky': ''}),
            ('Checkbutton.focus', {'side': 'left', 'sticky': 'w', 'children': [
                ('Checkbutton.label', {'sticky': 'nswe'})]})]})])
        base = ('Segoe UI', 10)
        self.root.option_add('*TCombobox*Listbox.background', CARD)
        self.root.option_add('*TCombobox*Listbox.foreground', TEXT)
        self.root.option_add('*TCombobox*Listbox.selectBackground', BLOOD)
        self.root.option_add('*TCombobox*Listbox.font', base)
        s.configure('.', background=PANEL, foreground=TEXT, fieldbackground=CARD, bordercolor=LINE,
                    lightcolor=LINE, darkcolor=LINE, troughcolor=CARD, focuscolor=GOLD, font=base)
        s.configure('TFrame', background=PANEL)
        s.configure('TLabel', background=PANEL, foreground=TEXT)
        s.configure('Muted.TLabel', background=PANEL, foreground=MUTED, font=('Segoe UI', 9))
        s.configure('Section.TLabel', background=PANEL, foreground=GOLD, font=('Georgia', 13))
        s.configure('TCheckbutton', background=PANEL, foreground=TEXT, padding=(0, 3))
        s.map('TCheckbutton', background=[('active', PANEL)], foreground=[('disabled', MUTED)])
        s.configure('Warning.TLabel', background='#2a1d12', foreground='#e3b25a', padding=(10, 6))
        s.configure('TCombobox', arrowcolor=GOLD, foreground=TEXT, padding=4)
        s.map('TCombobox', fieldbackground=[('readonly', CARD)], foreground=[('readonly', TEXT)],
              selectbackground=[('readonly', CARD)], selectforeground=[('readonly', TEXT)])
        s.configure('TEntry', foreground=TEXT, insertcolor=TEXT, padding=4)
        s.configure('TButton', background=CARD, foreground=TEXT, padding=(12, 6), borderwidth=1)
        s.map('TButton', background=[('active', LINE), ('disabled', PANEL)], foreground=[('disabled', MUTED)])
        s.configure('Play.TButton', background=BLOOD, foreground='#f4ece0', font=('Georgia', 15, 'bold'),
                    padding=(36, 10), borderwidth=0)
        s.map('Play.TButton', background=[('active', BLOOD_HI), ('disabled', '#3a2420')],
              foreground=[('disabled', '#8a7a70')])
        s.configure('Vertical.TScrollbar', background=CARD, arrowcolor=GOLD, troughcolor=PANEL,
                    bordercolor=PANEL)

    # ---- widget helpers ----------------------------------------------------------------------
    def var(self, key, store):
        """The Tk variable of a setting (one per setting, shared by every widget showing it)."""
        tk = self.tk
        if key not in self.vars:
            if store == 'ini':
                value = self.ini.get(key, INI_DEFAULTS.get(key, ''))
                v = tk.BooleanVar(value=value == '1') if key in INI_FLAGS else tk.StringVar(value=value)
            else:
                value = self.app.get(key, APP_DEFAULTS[key])
                v = tk.BooleanVar(value=bool(value)) if isinstance(APP_DEFAULTS[key], bool) \
                    else tk.StringVar(value=str(value))
            v.store = store
            self.vars[key] = v
        return self.vars[key]

    def choice(self, parent, key, store, options, width=36):
        """A combobox over (value, text) pairs, kept in sync with its variable."""
        var = self.var(key, store)
        values = [v for v, _t in options]
        box = self.ttk.Combobox(parent, values=[t for _v, t in options], state='readonly', width=width)
        if var.get() not in values:
            var.set(values[0])

        def show(*_args):
            current = var.get()
            box.current(values.index(current) if current in values else 0)
        show()
        box.bind('<<ComboboxSelected>>', lambda _e: var.set(values[box.current()]))
        var.trace_add('write', show)
        return box

    def next_row(self, parent):
        return parent.grid_size()[1]

    def row(self, parent, title, widget, hint=None):
        ttk = self.ttk
        r = self.next_row(parent)
        ttk.Label(parent, text=title).grid(row=r, column=0, sticky='nw', padx=(0, 18), pady=(8, 0))
        widget.grid(row=r, column=1, sticky='w', pady=(5, 0))
        if hint:
            ttk.Label(parent, text=hint, style='Muted.TLabel', wraplength=self.px(560), justify='left').grid(
                row=r + 1, column=1, sticky='w', pady=(2, 2))
        return widget

    def check(self, parent, key, store, title, hint=None):
        ttk = self.ttk
        r = self.next_row(parent)
        ttk.Checkbutton(parent, text=title, variable=self.var(key, store)).grid(
            row=r, column=0, columnspan=2, sticky='w', pady=(4, 0))
        if hint:
            ttk.Label(parent, text=hint, style='Muted.TLabel', wraplength=self.px(640), justify='left').grid(
                row=r + 1, column=0, columnspan=2, sticky='w', padx=(26, 0))

    def note(self, parent, text, top=12):
        self.ttk.Label(parent, text=text, style='Muted.TLabel', wraplength=self.px(680), justify='left').grid(
            row=self.next_row(parent), column=0, columnspan=2, sticky='w', pady=(top, 0))

    def section(self, parent, title, top=18):
        self.ttk.Label(parent, text=title, style='Section.TLabel').grid(
            row=self.next_row(parent), column=0, columnspan=2, sticky='w', pady=(top, 2))

    def page(self, name, title, subtitle):
        """A settings page: a heading and content that scrolls vertically."""
        tk, ttk = self.tk, self.ttk
        outer = ttk.Frame(self.content)
        head = ttk.Frame(outer, padding=(28, 22, 28, 6))
        head.pack(fill='x')
        ttk.Label(head, text=title, background=PANEL, foreground=TEXT, font=('Georgia', 20)).pack(anchor='w')
        ttk.Label(head, text=subtitle, style='Muted.TLabel').pack(anchor='w')
        canvas = tk.Canvas(outer, bg=PANEL, highlightthickness=0, bd=0)
        bar = ttk.Scrollbar(outer, orient='vertical', command=canvas.yview)
        inner = ttk.Frame(canvas, padding=(28, 0, 28, 24))
        inner.columnconfigure(1, weight=1)
        window = canvas.create_window(0, 0, window=inner, anchor='nw')
        inner.bind('<Configure>', lambda _e: canvas.configure(scrollregion=canvas.bbox('all')))
        canvas.bind('<Configure>', lambda e: canvas.itemconfigure(window, width=e.width))
        canvas.configure(yscrollcommand=bar.set)
        bar.pack(side='right', fill='y')
        canvas.pack(side='left', fill='both', expand=True)
        outer.scroll = lambda units: canvas.yview_scroll(units, 'units') \
            if inner.winfo_height() > canvas.winfo_height() else None
        self.pages[name] = outer
        return inner

    # ---- layout ------------------------------------------------------------------------------
    def build(self):
        tk, ttk = self.tk, self.ttk
        side = tk.Frame(self.root, bg=BG, width=self.px(220))
        self.side = side
        side.pack(side='left', fill='y')
        side.pack_propagate(False)
        try:
            from PIL import Image, ImageTk
            with Image.open(ASSETS / 'toport.png') as logo:
                size = self.px(96)
                self.logo = ImageTk.PhotoImage(logo.resize((size, size), Image.LANCZOS))
        except (ImportError, OSError):
            try:
                self.logo = tk.PhotoImage(file=str(ASSETS / 'toport_256.png')).subsample(2)
            except (tk.TclError, OSError):
                self.logo = None
        if self.logo:
            tk.Label(side, image=self.logo, bg=BG).pack(anchor='w', padx=18, pady=(20, 6))
        tk.Label(side, text='THE ORDER: 1886', bg=BG, fg=GOLD, font=('Georgia', 15)).pack(
            anchor='w', padx=22, pady=(4 if self.logo else 24, 0))
        tk.Label(side, text=T(f'TOport · port nativo · v{VERSION}', f'TOport · native port · v{VERSION}'), bg=BG, fg=MUTED,
                 font=('Segoe UI', 9)).pack(anchor='w', padx=22, pady=(0, 18))
        self.nav, self.current_page = {}, None
        for name, title in (('play', T('Jogar', 'Play')), ('graphics', T('Imagem', 'Graphics')),
                            ('display', T('Tela e desempenho', 'Display & performance')),
                            ('game', T('Jogo e idioma', 'Game & language')),
                            ('log', T('Registro', 'Log'))):
            item = tk.Label(side, text='    ' + title, bg=BG, fg=TEXT, anchor='w', font=('Segoe UI', 11),
                            pady=10, cursor='hand2')
            item.pack(fill='x')
            item.bind('<Button-1>', lambda _e, n=name: self.show(n))
            item.bind('<Enter>', lambda _e, n=name: n != self.current_page and self.nav[n].configure(bg='#1a1613'))
            item.bind('<Leave>', lambda _e, n=name: n != self.current_page and self.nav[n].configure(bg=BG))
            self.nav[name] = item
        tk.Frame(side, bg=BG).pack(fill='both', expand=True)
        self.side_note = tk.Label(side, text=T('No jogo: Insert\nabre o menu do TOport.\nF9 alterna o limite de quadros.', 'In game: Insert\nopens the TOport menu.\nF9 cycles the frame limit.'),
                 bg=BG, fg=MUTED, font=('Segoe UI', 9), justify='left',
                 wraplength=self.px(200))
        self.side_note.pack(anchor='w', padx=22, pady=(0, 18))

        right = tk.Frame(self.root, bg=PANEL)
        right.pack(side='left', fill='both', expand=True)
        bar = tk.Frame(right, bg=BG, height=72)
        bar.pack(fill='x', side='bottom')
        bar.pack_propagate(False)
        self.status = tk.Label(bar, text='', bg=BG, fg=MUTED, font=('Segoe UI', 10), anchor='w', justify='left')
        self.status.pack(side='left', padx=24)
        self.play_button = ttk.Button(bar, text=T('JOGAR', 'PLAY'), style='Play.TButton', command=self.play)
        self.play_button.pack(side='right', padx=(10, 24), pady=11)
        self.stop_button = ttk.Button(bar, text=T('Parar', 'Stop'), command=self.stop, state='disabled')
        self.stop_button.pack(side='right', pady=11)
        self.content = tk.Frame(right, bg=PANEL)
        self.content.pack(fill='both', expand=True)

        self.pages = {}
        self.build_play()
        self.build_graphics()
        self.build_display()
        self.build_game()
        self.build_log()
        self.root.bind_all('<MouseWheel>', self.wheel)
        self.refresh_status()

    def wheel(self, event):
        page = self.pages.get(self.current_page)
        if hasattr(page, 'scroll') and not isinstance(event.widget, (self.tk.Text, self.ttk.Combobox)) \
                and 'popdown' not in str(event.widget):
            page.scroll(int(-event.delta / 120))

    def show(self, name):
        for page in self.pages.values():
            page.pack_forget()
        self.pages[name].pack(fill='both', expand=True)
        self.current_page = name
        for n, item in self.nav.items():
            item.configure(bg=PANEL if n == name else BG, fg=GOLD if n == name else TEXT)
        if name == 'play':
            self.refresh_status()

    def build_play(self):
        tk, ttk = self.tk, self.ttk
        page = tk.Frame(self.content, bg=PANEL)
        self.pages['play'] = page
        self.banner = tk.Canvas(page, height=self.px(300), bg=BG, highlightthickness=0, bd=0)
        self.banner.pack(fill='x')
        self.banner.bind('<Configure>', lambda _e: self.draw_banner())
        body = ttk.Frame(page, padding=(28, 12, 28, 8))
        body.pack(fill='both', expand=True)
        body.columnconfigure(0, weight=3)
        body.columnconfigure(1, weight=2)
        info = ttk.Frame(body)
        info.grid(row=0, column=0, sticky='nw', padx=(0, 24))
        ttk.Label(info, text=T('Verificação', 'Ready check'), style='Section.TLabel').pack(anchor='w', pady=(0, 6))
        self.checks = {}
        for key in ('game', 'saves', 'gpu', 'dlss'):
            self.checks[key] = ttk.Label(info, text='', justify='left', wraplength=self.px(440))
            self.checks[key].pack(anchor='w', pady=3)
        quick = ttk.Frame(body)
        quick.grid(row=0, column=1, sticky='nw')
        ttk.Label(quick, text=T('Principal', 'Quick settings'), style='Section.TLabel').grid(
            row=0, column=0, columnspan=2, sticky='w', pady=(0, 2))
        self.row(quick, T('Anti-aliasing', 'Anti-aliasing'), self.choice(quick, 'order_aa', 'ini', AA_MODES(), 26))
        self.row(quick, T('Resolução', 'Resolution'), self.choice(quick, 'order_resolution', 'ini', RESOLUTIONS(), 26))
        self.row(quick, T('Limite de quadros', 'Frame limit'), self.choice(quick, 'fps_limit', 'ini', FPS_LIMITS, 26))
        self.row(quick, T('Idioma', 'Game language'), self.choice(quick, 'game_language', 'ini', LANGUAGES, 26))
        self.var('game_dir', 'app').trace_add('write', lambda *_a: (self.refresh_status(), self.draw_banner()))
        self.var('order_aa', 'ini').trace_add('write', lambda *_a: self.refresh_status())

    def draw_banner(self):
        """The dump's own art (sce_sys/pic0.png, London) under the title: nothing is shipped."""
        c = self.banner
        c.delete('all')
        w, h = max(c.winfo_width(), 400), int(c['height'])
        # The banner ships with the launcher (a TOport screenshot, launcher/banner.png); the dump's
        # own cover art (sce_sys/pic0.png) is the fallback.
        art = ASSETS / 'banner.png'
        if not art.is_file():
            art = Path(self.var('game_dir', 'app').get() or '.') / 'sce_sys' / 'pic0.png'
        if self.banner_source != art or self.banner_size != w:
            self.banner_source, self.banner_size, self.banner_scaled = art, w, None
            try:
                from PIL import Image, ImageTk
                with Image.open(art) as image:
                    height = round(image.height * w / image.width)
                    self.banner_scaled = ImageTk.PhotoImage(
                        image.convert('RGB').resize((w, height), Image.LANCZOS))
            except (ImportError, OSError):
                try:
                    image = self.tk.PhotoImage(file=str(art))
                    # Tk scales by integer zoom/subsample only: the smallest a/b (b <= 4) that still
                    # covers the banner's width.
                    ratio = w / image.width()
                    zoom, sub = max(1, math.ceil(ratio)), 1
                    for b in range(2, 5):
                        a = max(1, math.ceil(ratio * b))
                        if a / b < zoom / sub:
                            zoom, sub = a, b
                    scaled = image.zoom(zoom) if zoom > 1 else image
                    self.banner_scaled = scaled.subsample(sub) if sub > 1 else scaled
                except (self.tk.TclError, OSError):
                    pass
        if self.banner_scaled:
            c.create_image(w // 2, int(h * 0.55), image=self.banner_scaled)
            for i, stipple in enumerate(('gray12', 'gray25', 'gray50', 'gray75')):
                c.create_rectangle(0, h - 120 + i * 24, w, h - 96 + i * 24, fill=PANEL, outline='',
                                   stipple=stipple)
            c.create_rectangle(0, h - 24, w, h, fill=PANEL, outline='')
        else:
            c.create_text(w // 2, h // 2 - 20, text=T('Escolha a pasta do seu dump em "Jogo e idioma"', 'Choose your dump folder in "Game & language"'),
                          fill=MUTED, font=('Segoe UI', 11))
        c.create_text(32, h - 72, text='The Order: 1886', anchor='w', fill='#f2ead9', font=('Georgia', 34))
        ok, message = check_game(self.var('game_dir', 'app').get())
        c.create_text(35, h - 30, text=message if ok else T('Pasta do jogo não definida', 'Game folder not set'), anchor='w',
                      fill=GOLD, font=('Segoe UI', 11))

    def build_graphics(self):
        f = self.page('graphics', T('Imagem', 'Graphics'),
                      T('Salvo em bbport.ini; o menu do jogo (Insert) altera os mesmos valores.',
                        'Saved in bbport.ini; the in-game menu (Insert) changes the same values.'))
        self.section(f, 'Anti-aliasing', top=4)
        self.row(f, T('Modo', 'Mode'), self.choice(f, 'order_aa', 'ini', AA_MODES()),
                 T('DLSS (NVIDIA RTX) substitui a resolução temporal do próprio jogo. DLAA mantém a cena '
                   'na resolução nativa; os outros modos renderizam a cena menor e reconstroem para a '
                   'saída (mais FPS). TAA original é o anti-aliasing do PS4.',
                   "DLSS (NVIDIA RTX) replaces the game's own temporal resolve. DLAA keeps the scene at "
                   'native resolution; the other modes render a smaller scene and reconstruct it (more '
                   "FPS). The original TAA is the PS4's anti-aliasing."))
        self.section(f, T('Resolução', 'Resolution'))
        self.row(f, T('Resolução do jogo', 'Game resolution'), self.choice(f, 'order_resolution', 'ini', RESOLUTIONS()),
                 T('Original: como no PS4, cena em 2.40:1 com faixas pretas e a iluminação correta '
                   '(recomendado). As opções 16:9 usam os patches do illusion0001 que vieram no '
                   'executável do dump; em 1080p o próprio autor avisa que há problemas visuais.',
                   'Original: as on PS4, a 2.40:1 scene with black bars and correct lighting '
                   "(recommended). The 16:9 options use illusion0001's patches found in the dump's "
                   'executable; at 1080p their author warns of visual issues.'))

    def build_display(self):
        f = self.page('display', T('Tela e desempenho', 'Display & performance'),
                      T('Modo de tela, limite de quadros e carregamento.', 'Display mode, frame limit and loading.'))
        self.section(f, T('Tela', 'Display'), top=4)
        self.row(f, T('Modo de tela', 'Display mode'), self.choice(f, 'display', 'app', DISPLAY_MODES()),
                 T('Sem bordas é o recomendado. Exclusiva troca o modo do monitor.',
                   'Borderless is recommended. Exclusive changes the monitor mode.'))
        self.check(f, 'show_fps', 'ini', T('Mostrar FPS no canto', 'Show FPS in the corner'))
        self.section(f, T('Desempenho', 'Performance'))
        self.row(f, T('Taxa de quadros do jogo', 'Game frame rate'), self.choice(f, 'order_fps', 'ini', GAME_FPS()),
                 T('O jogo foi feito para 30 FPS. 60 FPS usa o patch do illusion0001; os QTEs dos '
                   'capítulos 6 e 9 travam com ele: volte a 30 para passar por eles.',
                   "The game was made for 30 FPS. 60 FPS uses illusion0001's patch; QTEs in chapters "
                   '6 and 9 softlock with it: switch back to 30 to get past them.'))
        self.row(f, T('Limite de quadros', 'Frame limit'), self.choice(f, 'fps_limit', 'ini', FPS_LIMITS),
                 T('F9 alterna 30/60/90/120 durante o jogo. Algumas cutscenes foram feitas para 30 FPS.',
                   'F9 cycles 30/60/90/120 in game. Some cutscenes were made for 30 FPS.'))
        self.check(f, 'fast_loading', 'ini', T('Carregamento acelerado', 'Fast loading'),
                   T('Filas de compute e de leitura sem esperas desnecessárias: texturas e iluminação '
                     'chegam mais rápido, sem perder elementos da cena.',
                     'Compute and read queues without needless waits: textures and lighting arrive '
                     'faster, without losing anything in the scene.'))
        self.check(f, 'preload_shaders', 'ini', T('Pré-carregar shaders ao abrir', 'Preload shaders at start'),
                   T('Antes de o jogo abrir, monta os shaders usados nas duas últimas sessões: menos '
                     'travadas ao entrar nas áreas, abertura um pouco mais lenta. Na primeira execução '
                     'ainda não há cache para carregar.',
                     'Before the game opens, builds the shaders used in the last two sessions: fewer '
                     'stutters entering areas, a slightly slower start. The first run has no cache yet.'))
        self.section(f, 'Launcher')
        self.check(f, 'close_on_play', 'app', T('Fechar o launcher quando o jogo abrir',
                                                 'Close the launcher when the game opens'))

    def build_game(self):
        ttk = self.ttk
        f = self.page('game', T('Jogo e idioma', 'Game & language'),
                      T('A pasta do seu dump e as opções do jogo.', 'Your dump folder and the game options.'))
        self.section(f, T('Pasta do jogo', 'Game folder'), top=4)
        var = self.var('game_dir', 'app')
        holder = ttk.Frame(f)
        ttk.Entry(holder, textvariable=var, width=54).pack(side='left')

        def browse():
            chosen = self.filedialog.askdirectory(
                title=T('Pasta do seu dump de The Order: 1886 (CUSA00785)', 'Your The Order: 1886 dump folder (CUSA00785)'), initialdir=var.get() or str(PORT_DIR))
            if chosen:
                var.set(str(Path(chosen)))
        ttk.Button(holder, text=T('Procurar…', 'Browse…'), command=browse).pack(side='left', padx=(6, 0))
        ttk.Button(holder, text=T('Abrir', 'Open'), command=lambda: self.open_path(var.get())).pack(side='left', padx=(6, 0))
        self.row(f, 'Dump (CUSA00785)', holder,
                 T('O seu próprio dump, versão 01.02: eboot.bin, sce_sys, sce_module e os dados do jogo. '
                   'Nenhum arquivo do jogo vem com o TOport.',
                   'Your own dump, version 01.02: eboot.bin, sce_sys, sce_module and the game data. '
                   'No game files come with TOport.'))
        self.section(f, T('Idioma', 'Language'))
        ui_box = self.choice(f, 'ui_language', 'app', UI_LANGUAGES)
        self.row(f, T('Idioma do launcher e do menu', 'Launcher and menu language'), ui_box,
                 T('O launcher reabre no idioma escolhido; o menu do jogo (Insert) usa o mesmo.',
                   'The launcher reopens in the chosen language; the in-game menu (Insert) uses it too.'))
        self.var('ui_language', 'app').trace_add('write', lambda *_a: self.root.after(50, self.switch_language))
        self.row(f, T('Idioma do jogo', 'Game language'), self.choice(f, 'game_language', 'ini', LANGUAGES),
                 T('Idioma do sistema informado ao jogo: textos e áudio que o disco tiver.',
                   'System language given to the game: the text and audio on the disc.'))
        self.section(f, 'Extras')
        self.check(f, 'photo_camera', 'ini', T('Câmera livre nas cutscenes (modo foto)',
                                                'Free camera in cutscenes (photo mode)'),
                   T('Libera o controle da câmera do modo foto durante as cutscenes.',
                     'Unlocks the photo mode camera during cutscenes.'))
        self.section(f, T('Arquivos', 'Files'))
        holder = ttk.Frame(f)
        ttk.Button(holder, text=T('Abrir pasta do TOport', 'Open the TOport folder'), command=lambda: self.open_path(DATA_DIR)).pack(side='left')
        ttk.Button(holder, text=T('Abrir saves', 'Open saves'), command=lambda: self.open_path(DATA_DIR / 'user')).pack(
            side='left', padx=(6, 0))
        self.row(f, T('Pastas', 'Folders'), holder,
                 T('bbport.ini e toport.json (configurações), user\\ (saves e '
                   'cache de shaders), out\\ (jogo preparado), last_run.log.',
                   'bbport.ini and toport.json (settings), user\\ (saves and shader cache), '
                   'out\\ (prepared game), last_run.log.'))

    def build_log(self):
        tk, ttk = self.tk, self.ttk
        page = ttk.Frame(self.content, padding=(20, 14, 20, 8))
        self.pages['log'] = page
        top = ttk.Frame(page)
        top.pack(fill='x', pady=(0, 8))
        ttk.Label(top, text=T('Registro', 'Log'), font=('Georgia', 20)).pack(side='left')
        ttk.Button(top, text=T('Copiar', 'Copy'), command=self.copy_log).pack(side='right')
        ttk.Button(top, text=T('Limpar', 'Clear'), command=lambda: self.set_log('')).pack(side='right', padx=6)
        self.log = tk.Text(page, wrap='none', bg='#0a0908', fg='#cfc6b8', insertbackground=TEXT, relief='flat',
                           font=('Consolas', 9), padx=8, pady=6, state='disabled', highlightthickness=0)
        bar = ttk.Scrollbar(page, command=self.log.yview)
        self.log.configure(yscrollcommand=bar.set)
        bar.pack(side='right', fill='y')
        self.log.pack(fill='both', expand=True)

    # ---- state -------------------------------------------------------------------------------
    def detect_gpu(self):
        exe = PORT_DIR / 'bin' / 'bb-gpu-capabilities.exe'
        if not exe.is_file():
            exe = PORT_DIR / 'out' / 'bb-gpu-capabilities.exe'
        env = dict(os.environ)
        if not (exe.parent / 'SDL3.dll').is_file():
            clang64 = Path(os.environ.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
            env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'
        text = T('• Placa de vídeo: não verificada', '• Graphics card: not checked')
        try:
            result = subprocess.run([str(exe), '--live-resolution'], capture_output=True, text=True,
                                    timeout=30, env=env, creationflags=NO_WINDOW)
            names = [line[5:].split(':')[0] for line in result.stderr.splitlines() if line.startswith('GPU: ')]
            if names:
                text = T(f'✓ Placa de vídeo: {names[0]}', f'✓ Graphics card: {names[0]}')
            elif result.returncode:
                text = T('✗ Nenhuma placa com Vulkan 1.3 encontrada (atualize o driver)', '✗ No Vulkan 1.3 graphics card found (update the driver)')
        except (OSError, subprocess.TimeoutExpired):
            pass
        self.gpu_text = text
        self.ui_calls.put(self.refresh_status)

    def check_update(self):
        """The newest release on GitHub (pre-releases too): a sidebar button when newer."""
        def parts(tag):
            try:
                return tuple(int(x) for x in tag.lstrip('v').split('-')[0].split('.'))
            except ValueError:
                return ()
        try:
            request = urllib.request.Request(RELEASES_API, headers={'User-Agent': f'TOport/{VERSION}'})
            with urllib.request.urlopen(request, timeout=10) as response:
                releases = json.loads(response.read().decode('utf-8'))
        except (OSError, ValueError):
            return
        tags = [r.get('tag_name', '') for r in releases if not r.get('draft')]
        newest = max(tags, key=parts, default='')
        if parts(newest) > parts(VERSION):
            self.ui_calls.put(lambda: self.show_update(newest))

    def show_update(self, tag):
        if not self.side.winfo_exists():
            return
        button = self.tk.Button(self.side, text=T(f'Nova versão {tag} disponível', f'New version {tag} available'),
                                bg=BLOOD, fg='#f4ece0', activebackground=BLOOD_HI, relief='flat',
                                font=('Segoe UI', 10, 'bold'), cursor='hand2',
                                command=lambda: webbrowser.open(f'{RELEASES_PAGE}/tag/{tag}'))
        button.pack(fill='x', padx=18, pady=(0, 12), before=self.side_note)

    def refresh_status(self):
        ok, message = check_game(self.var('game_dir', 'app').get())
        self.checks['game'].configure(text=('✓ ' if ok else '✗ ') + message,
                                      foreground=TEXT if ok else '#e3b25a')
        saves = DATA_DIR / 'user'
        self.checks['saves'].configure(
            text=T('✓ Saves e cache de shaders em ', '✓ Saves and shader cache in ') + str(saves)
            if saves.is_dir() else
            T('• Primeira execução: o jogo é preparado e os shaders são compilados enquanto você joga',
              '• First run: the game is prepared and shaders compile while you play'))
        self.checks['gpu'].configure(text=self.gpu_text)
        dlss_dir = PORT_DIR / 'bin' if (PORT_DIR / 'bin').is_dir() else PORT_DIR / 'out'
        wants_dlss = self.var('order_aa', 'ini').get() != 'game'
        has_dlss = (dlss_dir / 'nvngx_dlss.dll').is_file() and (dlss_dir / 'bbport_dlss.dll').is_file()
        self.checks['dlss'].configure(
            text=T('✓ DLSS instalado', '✓ DLSS installed') if has_dlss else
            (T('✗ DLSS não encontrado: o jogo usará o TAA original', '✗ DLSS not found: the game will use the original TAA') if wants_dlss else T('• DLSS não instalado', '• DLSS not installed')))
        if not self.process:
            self.status.configure(text=T('Pronto.', 'Ready.') if ok else T('Escolha a pasta do jogo.', 'Choose the game folder.'), fg=MUTED)
        self.play_button.configure(state='normal' if ok and not self.process else 'disabled')

    def collect(self):
        ini_changes = {}
        for key, var in self.vars.items():
            value = var.get()
            if var.store == 'ini':
                ini_changes[key] = ('1' if value else '0') if key in INI_FLAGS else value
            else:
                self.app[key] = value
        ini_changes['ui_language'] = self.app.get('ui_language', UI_LANG)
        write_ini(ini_changes)
        write_launcher(self.app)

    def switch_language(self):
        global UI_LANG
        new = self.var('ui_language', 'app').get()
        if new == UI_LANG or self.process:
            return
        self.collect()
        UI_LANG = new
        root = self.root
        for child in root.winfo_children():
            child.destroy()
        Launcher(root)

    def open_path(self, path):
        try:
            os.startfile(str(path))
        except OSError:
            pass

    # ---- game ----------------------------------------------------------------------------------
    def play(self):
        if self.process:
            return
        self.collect()
        ok, message = check_game(self.app['game_dir'])
        if not ok:
            self.messagebox.showerror('TOport', message)
            self.show('game')
            return
        self.set_log('')
        try:
            self.process = subprocess.Popen(run_command(), cwd=PORT_DIR, env=game_environment(self.app),
                                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
        except OSError as error:
            self.append(T(f'Não foi possível iniciar: {error}\n', f'Could not start: {error}\n'))
            self.process = None
            return
        self.job = GameJob(self.process)
        self.log_file = open(LOG_FILE, 'w', encoding='utf-8', errors='replace')
        threading.Thread(target=self.read_output, args=(self.process,), daemon=True).start()
        self.play_button.configure(state='disabled')
        self.stop_button.configure(state='normal')
        self.status.configure(text=T('Preparando o jogo e pré-carregando shaders; ele abre na própria '
                                     'janela…', 'Preparing the game and preloading shaders; it opens in '
                                     'its own window…') if self.var('preload_shaders', 'ini').get() else
                              T('Preparando o jogo; ele abre na própria janela…',
                                'Preparing the game; it opens in its own window…'), fg=GOLD)
        if self.app.get('close_on_play'):
            self.root.after(5000, self.root.destroy)

    def read_output(self, process):
        for raw in iter(process.stdout.readline, b''):
            self.output.put(raw.decode('utf-8', errors='replace'))
        self.output.put((process.wait(),))

    def drain_output(self):
        while not self.ui_calls.empty():
            self.ui_calls.get_nowait()()
        try:
            for _i in range(500):
                item = self.output.get_nowait()
                if isinstance(item, tuple):
                    self.append(T(f'\n— o jogo terminou (código {item[0]:#x}) —\n', f'\n— the game exited (code {item[0]:#x}) —\n'))
                    self.process = None
                    if self.job:
                        self.job.close()
                    if getattr(self, 'log_file', None):
                        self.log_file.close()
                        self.log_file = None
                    self.stop_button.configure(state='disabled')
                    self.refresh_status()
                else:
                    if getattr(self, 'log_file', None):
                        self.log_file.write(item)
                    if 'Pipeline cache:' in item and 'built at start' in item:
                        self.status.configure(text=T('Shaders pré-carregados; abrindo o jogo…', 'Shaders preloaded; opening the game…'), fg=GOLD)
                    elif 'Entering original x86-64 code' in item:
                        self.status.configure(text=T('O jogo está rodando.', 'The game is running.'), fg=GOLD)
                    elif 'restarting through run.py' in item:
                        self.status.configure(text=T('Reiniciando com as novas configurações…', 'Restarting with the new settings…'), fg=GOLD)
                    self.append(item)
        except queue.Empty:
            pass
        self.root.after(100, self.drain_output)

    def stop(self):
        if self.job:
            self.job.terminate()

    def append(self, text):
        self.log.configure(state='normal')
        self.log.insert('end', text)
        lines = int(self.log.index('end-1c').split('.')[0])
        if lines > MAX_LOG_LINES:
            self.log.delete('1.0', f'{lines - MAX_LOG_LINES}.0')
        self.log.see('end')
        self.log.configure(state='disabled')

    def set_log(self, text):
        self.log.configure(state='normal')
        self.log.delete('1.0', 'end')
        self.log.insert('end', text)
        self.log.configure(state='disabled')

    def copy_log(self):
        self.root.clipboard_clear()
        self.root.clipboard_append(self.log.get('1.0', 'end-1c'))

    def close(self):
        self.collect()
        self.root.destroy()


def main_window():
    import tkinter as tk
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except (AttributeError, OSError):
        pass
    root = tk.Tk()
    Launcher(root)
    root.mainloop()


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] in ('--run', '--script'):
        sys.exit(run_role(sys.argv[1:]))
    if len(sys.argv) > 1 and sys.argv[1] == '--play':
        sys.exit(play_without_window())
    main_window()
