#!/usr/bin/env python3
"""Game launcher for the Atari ports of the Amiga games
(geekychris/atari_st_games, checked out next to this repo).

    python3 tools/games/launcher.py              # pick a game in a window
    python3 tools/games/launcher.py --list       # list the games
    python3 tools/games/launcher.py --play void_trader

The games are defined in atari_st_games/games.ini (or --ini FILE; or
$ATARI_GAMES_DIR/games.ini): directory,
program, machine (ST, STE, Falcon...), TOS, Hatari options, screenshot,
description and controls.  Playing a game builds it if needed (make in
its directory), stops a Hatari already answering on the agent API port,
then starts Hatari through tools/agent/hatari-agent-run.sh with the
game's machine, its build directory as drive C: and the program
autostarted, so the agent API is available as usual.

Only needs the Python standard library (Tk 8.6 for PNG screenshots).
"""
import argparse
import configparser
import os
import shlex
import subprocess
import sys
import threading
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
# the games live in their own repo, geekychris/atari_st_games: by default
# a checkout next to this one
GAMES_DIR = os.path.abspath(os.environ.get("ATARI_GAMES_DIR")
                            or os.path.join(REPO, "..", "atari_st_games"))
DEFAULT_INI = os.path.join(GAMES_DIR, "games.ini")
RUN_SCRIPT = os.path.join(REPO, "tools", "agent", "hatari-agent-run.sh")
AGENT_GUI = os.path.join(REPO, "tools", "agent", "hatari_gui.py")

DEFAULT_TOS = {
    "st": "roms/emutos/emutos-192k-1.4/etos192uk.img",
    "other": "roms/emutos/emutos-1024k-1.4/etos1024k.img",
}
MACHINE_NAMES = {
    "st": "Atari ST", "megast": "Mega ST", "ste": "Atari STE",
    "megaste": "Mega STE", "tt": "Atari TT", "falcon": "Falcon030",
}
GAME_KEYS = ("title", "dir", "program", "machine")


class Game:
    def __init__(self, gid, sec, base):
        self.id = gid
        missing = [k for k in GAME_KEYS if not sec.get(k)]
        if missing:
            raise ValueError("[%s] is missing %s" % (gid, ", ".join(missing)))
        self.title = sec["title"]
        self.dir = os.path.normpath(os.path.join(base, sec["dir"]))
        self.program = sec["program"].upper()
        self.machine = sec["machine"].lower()
        tos = sec.get("tos") or DEFAULT_TOS["st" if self.machine == "st" else "other"]
        self.tos = os.path.expanduser(tos if os.path.isabs(tos) else os.path.join(REPO, tos))
        self.options = shlex.split(sec.get("options", ""))
        shot = sec.get("screenshot", "")
        self.screenshot = os.path.join(self.dir, shot) if shot else ""
        self.description = sec.get("description", "").strip()
        self.controls = sec.get("controls", "").strip()

    @property
    def machine_name(self):
        return MACHINE_NAMES.get(self.machine, self.machine)

    @property
    def build_dir(self):
        return os.path.join(self.dir, "build")


class Launcher:
    """Reads the INI file; builds, starts and stops games."""

    def __init__(self, ini):
        cp = configparser.ConfigParser(comment_prefixes=(";", "#"),
                                       inline_comment_prefixes=None,
                                       interpolation=None)
        if not cp.read(ini):
            raise FileNotFoundError(ini)
        base = os.path.dirname(os.path.abspath(ini))
        opts = cp["launcher"] if cp.has_section("launcher") else {}
        self.cross = os.path.expanduser(opts.get("cross", "") or
                                        os.environ.get("CROSS", "m68k-atari-mintelf-"))
        self.build = opts.get("build", "yes").lower() in ("yes", "true", "on", "1")
        self.sound = opts.get("sound", "on").lower() in ("yes", "true", "on", "1")
        self.port = int(opts.get("port", "7777"))
        self.games = [Game(s, cp[s], base) for s in cp.sections() if s != "launcher"]

    def find(self, gid):
        for g in self.games:
            if g.id == gid or g.title.lower() == gid.lower():
                return g
        raise KeyError(gid)

    # agent API on the configured port
    def api(self, path, method="GET", timeout=1.0):
        req = urllib.request.Request("http://127.0.0.1:%d%s" % (self.port, path),
                                     method=method)
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read()

    def running(self):
        try:
            self.api("/status")
            return True
        except OSError:
            return False

    def stop(self, log=print):
        """Quit a Hatari answering on the port; True when it is gone."""
        if not self.running():
            return True
        log("Stopping the running Hatari...")
        try:
            self.api("/emu/quit", method="POST")
        except OSError:
            pass
        for _ in range(50):
            if not self.running():
                return True
            time.sleep(0.1)
        log("Hatari on port %d did not quit" % self.port)
        return False

    def make(self, game, log=print):
        log("Building %s (make in %s)..." % (game.title, os.path.relpath(game.dir, REPO)))
        p = subprocess.run(["make", "CROSS=" + self.cross], cwd=game.dir,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if p.returncode:
            log(p.stdout[-2000:])
            raise RuntimeError("build of %s failed" % game.title)

    def play(self, game, build=None, log=print):
        if self.build if build is None else build:
            self.make(game, log)
        prg = os.path.join(game.build_dir, game.program)
        if not os.path.exists(prg):
            raise RuntimeError("%s not found; build it (make in %s)" % (prg, game.dir))
        if not os.path.exists(game.tos):
            raise RuntimeError("TOS image %s not found; run tools/agent/fetch-emutos.sh"
                               % game.tos)
        if not self.stop(log):
            raise RuntimeError("could not stop the Hatari already running")
        cmd = [RUN_SCRIPT, "--machine", game.machine] + game.options + [
            "--natfeats", "on", "--harddrive", game.build_dir,
            "--auto", "C:\\" + game.program]
        if not self.sound:
            cmd += ["--sound", "off"]
        env = dict(os.environ, AGENT_TOS=game.tos, AGENT_PORT=str(self.port))
        log("Starting %s on the %s..." % (game.title, game.machine_name))
        p = subprocess.run(cmd, env=env, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True)
        log(p.stdout.strip())
        if p.returncode:
            raise RuntimeError("Hatari did not start")


def run_gui(launcher):
    import tkinter as tk
    from tkinter import ttk

    root = tk.Tk()
    root.title("Hatari Game Launcher")
    root.minsize(980, 680)

    left = ttk.Frame(root, padding=8)
    left.pack(side="left", fill="y")
    ttk.Label(left, text="Games", font=("TkDefaultFont", 13, "bold")).pack(anchor="w")
    lb = tk.Listbox(left, width=30, height=20, exportselection=False,
                    activestyle="none", font=("TkDefaultFont", 12))
    lb.pack(fill="y", expand=True, pady=(4, 0))
    for g in launcher.games:
        lb.insert("end", "%s  (%s)" % (g.title, g.machine_name))

    right = ttk.Frame(root, padding=8)
    right.pack(side="left", fill="both", expand=True)
    title = ttk.Label(right, font=("TkDefaultFont", 18, "bold"))
    title.pack(anchor="w")
    machine = ttk.Label(right, foreground="gray40")
    machine.pack(anchor="w")
    shot = tk.Label(right, width=640, height=480, bg="black")
    shot.pack(anchor="w", pady=6)
    text = ttk.Frame(right)
    text.pack(fill="x", anchor="w")
    desc = ttk.Label(text, wraplength=640, justify="left")
    desc.pack(anchor="w")
    ttk.Label(text, text="Controls", font=("TkDefaultFont", 12, "bold")).pack(
        anchor="w", pady=(6, 0))
    ctrl = ttk.Label(text, justify="left", font=("Menlo", 11) if sys.platform == "darwin"
                     else ("Courier", 10))
    ctrl.pack(anchor="w")

    bar = ttk.Frame(right)
    bar.pack(fill="x", pady=(8, 0))
    status = ttk.Label(root, relief="sunken", anchor="w", padding=(6, 2))
    status.pack(side="bottom", fill="x", before=left)
    play_btn = ttk.Button(bar, text="Play")
    play_btn.pack(side="left")
    stop_btn = ttk.Button(bar, text="Stop Hatari")
    stop_btn.pack(side="left", padx=6)
    console_btn = ttk.Button(bar, text="Agent console")
    console_btn.pack(side="left")
    build_var = tk.BooleanVar(value=launcher.build)
    ttk.Checkbutton(bar, text="Build first", variable=build_var).pack(side="left", padx=12)
    sound_var = tk.BooleanVar(value=launcher.sound)
    ttk.Checkbutton(bar, text="Sound", variable=sound_var).pack(side="left")

    images = {}
    busy = []

    def log(msg):
        line = (msg or "").strip().splitlines()
        if line:
            root.after(0, status.config, {"text": line[-1]})

    def current():
        sel = lb.curselection()
        return launcher.games[sel[0]] if sel else None

    def show(_event=None):
        g = current()
        if not g:
            return
        title.config(text=g.title)
        machine.config(text="%s  ·  %s  ·  %s" % (
            g.machine_name, os.path.relpath(g.dir, REPO), g.program))
        img = images.get(g.id)
        if img is None and g.screenshot and os.path.exists(g.screenshot):
            try:
                img = tk.PhotoImage(file=g.screenshot)
                if img.width() > 700:          # 2x captures of wide screens
                    img = img.subsample(2)
            except tk.TclError:
                img = None
            images[g.id] = img
        shot.config(image=img or "", text="" if img else "(no screenshot)",
                    fg="white")
        desc.config(text=" ".join(g.description.split("\n")))
        ctrl.config(text=g.controls)

    def in_background(fn, *args):
        if busy:
            return
        busy.append(1)
        play_btn.state(["disabled"])

        def work():
            try:
                fn(*args)
            except Exception as e:      # shown in the status bar
                log("Error: %s" % e)
            finally:
                busy.clear()
                root.after(0, play_btn.state, ["!disabled"])
        threading.Thread(target=work, daemon=True).start()

    def play(_event=None):
        g = current()
        if g:
            launcher.sound = sound_var.get()
            in_background(lambda: (launcher.play(g, build=build_var.get(), log=log),
                                   log("%s is running - close Hatari or press Stop "
                                       "to pick another game" % g.title)))

    def stop():
        in_background(lambda: (launcher.stop(log), log("Hatari stopped")))

    def console():
        subprocess.Popen([sys.executable, AGENT_GUI, "--api",
                          "http://127.0.0.1:%d" % launcher.port])

    play_btn.config(command=play)
    stop_btn.config(command=stop)
    console_btn.config(command=console)
    lb.bind("<<ListboxSelect>>", show)
    lb.bind("<Double-Button-1>", play)
    lb.bind("<Return>", play)
    root.bind("<Escape>", lambda e: root.destroy())

    if launcher.games:
        lb.selection_set(0)
        lb.focus_set()
        show()
    log("%d games from %s. Double-click or Return to play."
        % (len(launcher.games), os.path.relpath(launcher_ini, REPO)))
    root.mainloop()


def main():
    global launcher_ini
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ini", default=DEFAULT_INI, help="games file (default %(default)s)")
    ap.add_argument("--list", action="store_true", help="list the games and exit")
    ap.add_argument("--play", metavar="ID", help="start a game without the window")
    ap.add_argument("--no-build", action="store_true", help="don't run make first")
    ap.add_argument("--no-sound", action="store_true", help="start Hatari with --sound off")
    args = ap.parse_args()

    launcher_ini = args.ini
    if not os.path.exists(args.ini):
        sys.exit("no games file at %s\n"
                 "The games are in their own repo; check it out next to this one:\n"
                 "  git clone https://github.com/geekychris/atari_st_games %s\n"
                 "or set ATARI_GAMES_DIR, or pass --ini FILE."
                 % (args.ini, os.path.join(os.path.dirname(REPO), "atari_st_games")))
    launcher = Launcher(args.ini)
    if args.no_sound:
        launcher.sound = False
    if args.list:
        w = max(len(g.id) for g in launcher.games)
        for g in launcher.games:
            print("%-*s  %-20s %s" % (w, g.id, g.title, g.machine_name))
        return 0
    if args.play:
        try:
            launcher.play(launcher.find(args.play), build=False if args.no_build else None)
        except KeyError:
            sys.exit("unknown game %r (see --list)" % args.play)
        except RuntimeError as e:
            sys.exit("error: %s" % e)
        return 0
    if args.no_build:
        launcher.build = False
    run_gui(launcher)
    return 0


launcher_ini = DEFAULT_INI

if __name__ == "__main__":
    sys.exit(main())
