#!/usr/bin/env python3
"""Hatari Agent Console: a Tkinter front end for the Hatari agent API.

    python3 tools/agent/hatari_gui.py [--api http://127.0.0.1:7777]

- Live view of the emulated screen (native resolution, zoomable).
  Click / right-click / drag on it to send mouse input, type to send keys.
- Tabs: program console output, joystick pad, debugger (break, step,
  registers, disassembly, breakpoints, debugger commands), machine
  (TOS ROMs, floppy, host folder as C:, Hatari options).
- Menus for emulation control, snapshots, screenshots, the INTERACT
  demo scenarios (examples/interact/scenarios.py) and its test suite.

Only needs the Python standard library (Tk 8.6 for PNG support).
"""
import argparse
import base64
import os
import queue
import re
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, scrolledtext, simpledialog, ttk

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
INTERACT = os.path.join(REPO, "examples", "interact")
sys.path.insert(0, HERE)
sys.path.insert(0, INTERACT)
from hatari_agent import Hatari, HatariError  # noqa: E402

try:
    import scenarios  # INTERACT demo scenarios
except ImportError:
    scenarios = None

MONO = ("Menlo", 11) if sys.platform == "darwin" else ("Courier", 10)
ANSI = re.compile(r"\x1b\[[0-9;]*m")
SPECIAL_KEYS = {
    "Return": "return", "KP_Enter": "kpenter", "BackSpace": "backspace",
    "Escape": "esc", "Tab": "tab", "Delete": "delete", "Insert": "insert",
    "Home": "home", "Up": "up", "Down": "down", "Left": "left", "Right": "right",
    "Help": "help", "End": "undo",
    **{f"F{i}": f"f{i}" for i in range(1, 11)},
}


class Gui:
    def __init__(self, root, api):
        self.root = root
        self.h = Hatari(api)
        self.ui_q = queue.Queue()      # callables to run on the Tk thread
        self.work_q = queue.Queue()    # API calls from UI actions
        self.zoom = tk.IntVar(value=2)
        self.refresh = tk.IntVar(value=150)
        self.follow = tk.BooleanVar(value=False)
        self.ff = tk.BooleanVar(value=False)
        self.connected = None
        self.console_pos = 0
        self.last_stop = None
        self.image = None
        self.native = (320, 200)
        self.drag = None
        self.busy = False

        root.title("Hatari Agent Console")
        self.build_menus()
        self.build_toolbar()
        self.build_body()
        self.status = ttk.Label(root, text="connecting...", anchor="w", relief="sunken")
        self.status.pack(side="bottom", fill="x")

        threading.Thread(target=self.worker, daemon=True).start()
        threading.Thread(target=self.poller, daemon=True).start()
        root.after(30, self.pump)

    # ------------------------------------------------------------------ threads

    def ui(self, fn, *args):
        self.ui_q.put((fn, args))

    def pump(self):
        try:
            while True:
                fn, args = self.ui_q.get_nowait()
                fn(*args)
        except queue.Empty:
            pass
        self.root.after(30, self.pump)

    def do(self, fn, *args, then=None, quiet=False):
        """run API call on worker thread, optional 'then(result)' on Tk thread"""
        self.work_q.put((fn, args, then, quiet))

    def worker(self):
        while True:
            fn, args, then, quiet = self.work_q.get()
            try:
                result = fn(*args)
                if then:
                    self.ui(then, result)
            except HatariError as err:
                if not quiet:
                    self.ui(self.log, f"error: {err}")
            except Exception as err:  # keep the worker alive
                self.ui(self.log, f"error: {err!r}")

    def poller(self):
        n = 0
        while True:
            try:
                png = self.h.screen()
                self.ui(self.show_screen, png)
                if n % 3 == 0:
                    st = self.h.status()
                    try:
                        mouse = self.h.mouse_pos()
                    except HatariError:
                        mouse = None
                    text, nxt = self.h.console(self.console_pos)
                    self.console_pos = nxt
                    self.ui(self.show_status, st, mouse, text)
                if not self.connected:
                    self.connected = True
                    self.ui(self.log, f"connected to {self.h.base}")
                    self.do(self.h.roms, then=self.show_roms, quiet=True)
            except HatariError:
                if self.connected is not False:
                    self.connected = False
                    self.ui(self.show_disconnected)
                time.sleep(1)
            n += 1
            time.sleep(self.refresh.get() / 1000)

    # ------------------------------------------------------------------ layout

    def build_menus(self):
        mb = tk.Menu(self.root)
        m = tk.Menu(mb, tearoff=0)
        m.add_command(label="Start emulator", command=self.start_emulator)
        m.add_command(label="Start INTERACT demo", command=self.start_interact)
        m.add_separator()
        m.add_command(label="Save screenshot...", command=lambda: self.save_screenshot(False))
        m.add_command(label="Save full-window screenshot...", command=lambda: self.save_screenshot(True))
        m.add_command(label="Save state...", command=self.save_state)
        m.add_command(label="Load state...", command=self.load_state)
        m.add_separator()
        m.add_command(label="Quit Hatari", command=lambda: self.do(self.h.quit))
        m.add_command(label="Exit console", command=self.root.destroy)
        mb.add_cascade(label="File", menu=m)

        m = tk.Menu(mb, tearoff=0)
        m.add_command(label="Pause", command=lambda: self.do(self.h.pause))
        m.add_command(label="Resume", command=lambda: self.do(self.h.resume))
        m.add_command(label="Run 1 frame", command=lambda: self.do(self.h.run, 1))
        m.add_command(label="Run 1 second", command=lambda: self.do(self.h.run, 50, False))
        m.add_command(label="Run N frames...", command=self.run_frames)
        m.add_separator()
        m.add_command(label="Cold reset", command=lambda: self.do(self.h.reset, True))
        m.add_command(label="Warm reset", command=lambda: self.do(self.h.reset, False))
        m.add_checkbutton(label="Fast forward", variable=self.ff,
                          command=lambda: self.do(self.h.fast_forward, self.ff.get()))
        mb.add_cascade(label="Emulation", menu=m)

        m = tk.Menu(mb, tearoff=0)
        m.add_command(label="Break", command=self.dbg_break)
        m.add_command(label="Step", command=self.dbg_step)
        m.add_command(label="Step over", command=self.dbg_next)
        m.add_command(label="Continue", command=self.dbg_cont)
        m.add_separator()
        m.add_command(label="Add breakpoint...", command=self.add_breakpoint_dialog)
        m.add_command(label="Clear breakpoints", command=self.clear_breakpoints)
        mb.add_cascade(label="Debug", menu=m)

        m = tk.Menu(mb, tearoff=0)
        if scenarios:
            for fn in scenarios.SCENARIOS:
                m.add_command(label=f"{fn.__name__} - {fn.__doc__}",
                              command=lambda n=fn.__name__: self.run_scenario(n))
            m.add_separator()
        m.add_command(label="Run INTERACT test suite", command=self.run_tests)
        mb.add_cascade(label="Scenarios", menu=m)

        m = tk.Menu(mb, tearoff=0)
        for z in (1, 2, 3):
            m.add_radiobutton(label=f"Zoom {z}x", variable=self.zoom, value=z)
        m.add_separator()
        for ms in (100, 150, 300, 1000):
            m.add_radiobutton(label=f"Refresh every {ms} ms", variable=self.refresh, value=ms)
        m.add_separator()
        m.add_checkbutton(label="Mouse follows pointer", variable=self.follow)
        mb.add_cascade(label="View", menu=m)
        self.root.config(menu=mb)

    def build_toolbar(self):
        bar = ttk.Frame(self.root, padding=(4, 4))
        bar.pack(side="top", fill="x")
        for text, cmd in (("Pause", lambda: self.do(self.h.pause)),
                          ("Resume", lambda: self.do(self.h.resume)),
                          ("+1 frame", lambda: self.do(self.h.run, 1)),
                          ("+1 s", lambda: self.do(self.h.run, 50, False)),
                          ("Reset", lambda: self.do(self.h.reset, True)),
                          ("Screenshot", lambda: self.save_screenshot(False))):
            ttk.Button(bar, text=text, command=cmd).pack(side="left", padx=2)
        ttk.Checkbutton(bar, text="Fast forward", variable=self.ff,
                        command=lambda: self.do(self.h.fast_forward, self.ff.get())).pack(side="left", padx=8)
        ttk.Label(bar, text="click/drag/type on the screen to send input",
                  foreground="gray").pack(side="right")

    def build_body(self):
        body = ttk.Frame(self.root, padding=4)
        body.pack(side="top", fill="both", expand=True)

        self.canvas = tk.Canvas(body, width=640, height=400, bg="black",
                                highlightthickness=2, highlightcolor="#4a90d9", takefocus=1)
        self.canvas.pack(side="left", anchor="n")
        self.canvas_img = self.canvas.create_image(0, 0, anchor="nw")
        self.canvas_msg = self.canvas.create_text(320, 200, fill="white", text="", font=MONO)
        c = self.canvas
        c.bind("<ButtonPress-1>", self.on_press)
        c.bind("<B1-Motion>", self.on_drag)
        c.bind("<ButtonRelease-1>", self.on_release)
        # right button: Button-2 on macOS Tk, Button-3 elsewhere
        c.bind("<ButtonPress-2>" if sys.platform == "darwin" else "<ButtonPress-3>", self.on_right)
        c.bind("<Control-ButtonPress-1>", self.on_right)
        c.bind("<Motion>", self.on_motion)
        c.bind("<KeyPress>", self.on_key)

        nb = ttk.Notebook(body)
        nb.pack(side="left", fill="both", expand=True, padx=(6, 0))
        self.build_console_tab(nb)
        self.build_joystick_tab(nb)
        self.build_debugger_tab(nb)
        self.build_machine_tab(nb)

    def build_console_tab(self, nb):
        f = ttk.Frame(nb, padding=4)
        nb.add(f, text="Console")
        self.console = scrolledtext.ScrolledText(f, width=60, height=24, font=MONO, state="disabled")
        self.console.pack(fill="both", expand=True)
        self.console.tag_config("gui", foreground="#2a6fbb")
        ttk.Button(f, text="Clear", command=self.clear_console).pack(anchor="e", pady=(4, 0))

    def build_joystick_tab(self, nb):
        f = ttk.Frame(nb, padding=12)
        nb.add(f, text="Joystick")
        self.joy_port = tk.IntVar(value=1)
        ports = ttk.Frame(f)
        ports.pack(anchor="w")
        ttk.Radiobutton(ports, text="Port 1 (joystick)", variable=self.joy_port, value=1).pack(side="left")
        ttk.Radiobutton(ports, text="Port 0 (mouse port)", variable=self.joy_port, value=0).pack(side="left")
        ttk.Label(f, text="Hold a button to hold the direction:").pack(anchor="w", pady=(10, 4))
        pad = ttk.Frame(f)
        pad.pack(anchor="w")
        layout = [["up,left", "up", "up,right"],
                  ["left", None, "right"],
                  ["down,left", "down", "down,right"]]
        arrows = {"up,left": "↖", "up": "↑", "up,right": "↗", "left": "←", "right": "→",
                  "down,left": "↙", "down": "↓", "down,right": "↘"}
        for r, row in enumerate(layout):
            for col, dirs in enumerate(row):
                if dirs is None:
                    dirs, label = "fire", "FIRE"
                else:
                    label = arrows[dirs]
                b = ttk.Button(pad, text=label, width=5)
                b.grid(row=r, column=col, padx=2, pady=2, ipady=6)
                b.bind("<ButtonPress-1>", lambda e, d=dirs: self.joy(d))
                b.bind("<ButtonRelease-1>", lambda e: self.joy("none"))
        ttk.Label(f, text="With the mouse enabled the ST reports joystick-1\n"
                          "fire as the right mouse button (same hardware line).",
                  foreground="gray").pack(anchor="w", pady=(12, 0))

    def build_debugger_tab(self, nb):
        f = ttk.Frame(nb, padding=4)
        nb.add(f, text="Debugger")
        bar = ttk.Frame(f)
        bar.pack(fill="x")
        for text, cmd in (("Break", self.dbg_break), ("Step", self.dbg_step),
                          ("Step over", self.dbg_next), ("Continue", self.dbg_cont),
                          ("Run to breakpoint", self.dbg_cont_wait)):
            ttk.Button(bar, text=text, command=cmd).pack(side="left", padx=2)
        self.regs = tk.Text(f, height=6, width=60, font=MONO, state="disabled")
        self.regs.pack(fill="x", pady=4)
        self.dis = tk.Text(f, height=9, width=60, font=MONO, state="disabled")
        self.dis.pack(fill="x")
        self.dis.tag_config("pc", background="#ffef9f")

        bp = ttk.LabelFrame(f, text="Breakpoints (address/symbol, or condition like d0=$20)", padding=4)
        bp.pack(fill="x", pady=4)
        self.bp_entry = ttk.Entry(bp, width=30)
        self.bp_entry.pack(side="left")
        self.bp_entry.bind("<Return>", lambda e: self.add_breakpoint(self.bp_entry.get()))
        ttk.Button(bp, text="Add", command=lambda: self.add_breakpoint(self.bp_entry.get())).pack(side="left", padx=2)
        ttk.Button(bp, text="Clear all", command=self.clear_breakpoints).pack(side="left", padx=2)
        self.bp_label = ttk.Label(f, text="no breakpoints", font=MONO)
        self.bp_label.pack(anchor="w")

        cmdf = ttk.LabelFrame(f, text="Hatari debugger command (output in Console tab)", padding=4)
        cmdf.pack(fill="x", pady=4)
        self.cmd_entry = ttk.Entry(cmdf, width=40)
        self.cmd_entry.pack(side="left", fill="x", expand=True)
        self.cmd_entry.insert(0, "info osheader")
        self.cmd_entry.bind("<Return>", lambda e: self.debug_cmd())
        ttk.Button(cmdf, text="Run", command=self.debug_cmd).pack(side="left", padx=2)

    def build_machine_tab(self, nb):
        f = ttk.Frame(nb, padding=4)
        nb.add(f, text="Machine")
        rf = ttk.LabelFrame(f, text="TOS ROM", padding=4)
        rf.pack(fill="both", expand=True)
        self.rom_list = tk.Listbox(rf, height=10, font=MONO, exportselection=False)
        self.rom_list.pack(fill="both", expand=True)
        row = ttk.Frame(rf)
        row.pack(fill="x", pady=4)
        ttk.Label(row, text="Machine").pack(side="left")
        self.machine = ttk.Combobox(row, width=8, state="readonly",
                                    values=["", "st", "megast", "ste", "megaste", "tt", "falcon"])
        self.machine.pack(side="left", padx=4)
        ttk.Label(row, text="RAM KB").pack(side="left")
        self.memory = ttk.Entry(row, width=6)
        self.memory.pack(side="left", padx=4)
        ttk.Button(row, text="Boot selected", command=self.boot_rom).pack(side="left", padx=4)
        ttk.Button(row, text="Refresh", command=lambda: self.do(self.h.roms, then=self.show_roms)).pack(side="left")

        mf = ttk.LabelFrame(f, text="Media", padding=4)
        mf.pack(fill="x", pady=4)
        ttk.Button(mf, text="Insert floppy A...", command=self.insert_floppy).pack(side="left", padx=2)
        ttk.Button(mf, text="Host folder as C:...", command=self.attach_folder).pack(side="left", padx=2)
        ttk.Button(mf, text="Autostart program...", command=self.autostart).pack(side="left", padx=2)

        of = ttk.LabelFrame(f, text="Hatari options (may reset)", padding=4)
        of.pack(fill="x")
        self.opt_entry = ttk.Entry(of, width=40)
        self.opt_entry.pack(side="left", fill="x", expand=True)
        self.opt_entry.insert(0, "--monitor mono")
        ttk.Button(of, text="Apply", command=lambda: self.do(self.h.config, self.opt_entry.get(),
                                                              then=lambda r: self.log(r.get("output", "").strip()))).pack(side="left", padx=2)

    # ------------------------------------------------------------------ display

    def show_screen(self, png):
        try:
            img = tk.PhotoImage(data=base64.b64encode(png))
        except tk.TclError:
            return
        self.native = (img.width(), img.height())
        z = self.zoom.get()
        if z > 1:
            img = img.zoom(z)
        self.image = img
        self.canvas.itemconfig(self.canvas_img, image=img)
        self.canvas.itemconfig(self.canvas_msg, text="")
        if int(self.canvas["width"]) != img.width() or int(self.canvas["height"]) != img.height():
            self.canvas.config(width=img.width(), height=img.height())

    def show_status(self, st, mouse, text):
        stop = st["stop"]
        parts = [st["state"].upper(), f"frame {st['frame']}", f"PC ${st['pc'][2:]}",
                 f"{st['machine'].upper()} {'EmuTOS' if st['tos']['emutos'] else 'TOS'} {st['tos']['version']}",
                 f"{self.native[0]}x{self.native[1]}"]
        if mouse:
            parts.append(f"GEM mouse {mouse[0]},{mouse[1]}")
        if stop["count"]:
            parts.append(f"last stop: {stop['reason']} @ ${stop['pc'][2:]}")
        self.status.config(text="   |   ".join(parts))
        if text:
            self.append_console(text.replace("\r", ""))
        self.ff.set(st["fast_forward"])
        # refresh debugger view when the CPU (newly) stopped
        key = (st["state"], stop["count"])
        if st["state"] == "stopped" and key != self.last_stop:
            self.refresh_debugger()
        self.last_stop = key
        if st["breakpoints"] != getattr(self, "bp_count", None):
            self.bp_count = st["breakpoints"]
            self.do(self.h.breakpoints, then=self.show_breakpoints, quiet=True)

    def show_disconnected(self):
        self.status.config(text=f"no emulator at {self.h.base} - File > Start emulator / Start INTERACT demo")
        self.canvas.itemconfig(self.canvas_msg, text="not connected",
                               )
        self.canvas.coords(self.canvas_msg, int(self.canvas["width"]) // 2, int(self.canvas["height"]) // 2)

    def append_console(self, text, tag=None):
        self.console.config(state="normal")
        self.console.insert("end", text, tag)
        self.console.see("end")
        self.console.config(state="disabled")

    def log(self, msg):
        self.append_console(f"» {msg}\n", "gui")

    def clear_console(self):
        self.console.config(state="normal")
        self.console.delete("1.0", "end")
        self.console.config(state="disabled")

    def set_text(self, widget, text):
        widget.config(state="normal")
        widget.delete("1.0", "end")
        widget.insert("1.0", text)
        widget.config(state="disabled")

    # ------------------------------------------------------------------ input

    def to_native(self, event):
        z = self.zoom.get()
        x = max(0, min(self.native[0] - 1, event.x // z))
        y = max(0, min(self.native[1] - 1, event.y // z))
        return x, y

    def on_press(self, event):
        self.canvas.focus_set()
        self.drag = {"start": self.to_native(event), "dragging": False}

    def on_drag(self, event):
        if not self.drag:
            return
        x, y = self.to_native(event)
        if not self.drag["dragging"]:
            sx, sy = self.drag["start"]
            if abs(x - sx) + abs(y - sy) < 3:
                return
            self.drag["dragging"] = True
            self.do(self.h.mouse, sx, sy, None, None, 2)
            self.do(self.h.button, "down")
        # throttle: skip intermediate moves while the worker is busy
        if self.work_q.qsize() < 2:
            self.do(self.h.mouse, x, y, None, None, 1)

    def on_release(self, event):
        if not self.drag:
            return
        x, y = self.to_native(event)
        if self.drag["dragging"]:
            self.do(self.h.mouse, x, y, None, None, 1)
            self.do(self.h.button, "up")
        else:
            self.do(self.h.click, x, y)
        self.drag = None

    def on_right(self, event):
        self.canvas.focus_set()
        x, y = self.to_native(event)
        self.do(self.h.click, x, y, "right")

    def on_motion(self, event):
        if self.follow.get() and self.work_q.qsize() == 0:
            x, y = self.to_native(event)
            self.do(self.h.mouse, x, y, None, None, 1, quiet=True)

    def on_key(self, event):
        ctrl = event.state & 0x4
        if event.keysym in SPECIAL_KEYS:
            self.do(self.h.key, SPECIAL_KEYS[event.keysym])
        elif ctrl and len(event.keysym) == 1:
            self.do(self.h.key, f"ctrl+{event.keysym.lower()}")
        elif event.char and 32 <= ord(event.char) < 127:
            self.do(self.h.type_text, event.char)
        return "break"

    def joy(self, dirs):
        self.do(self.h.joystick, dirs, 0, self.joy_port.get())

    # ------------------------------------------------------------------ debugger

    def show_stop(self, result):
        if "regs" in result:
            self.show_regs(result["regs"])
        if "disasm" in result:
            self.show_disasm(result["disasm"])

    def refresh_debugger(self):
        self.do(self.h.regs, then=self.show_regs, quiet=True)
        self.do(lambda: self.h.disasm("pc", 9), then=self.show_disasm, quiet=True)

    def show_regs(self, r):
        d = "  ".join(f"D{i} {r[f'd{i}'][2:]}" for i in range(4)) + "\n" + \
            "  ".join(f"D{i} {r[f'd{i}'][2:]}" for i in range(4, 8)) + "\n" + \
            "  ".join(f"A{i} {r[f'a{i}'][2:]}" for i in range(4)) + "\n" + \
            "  ".join(f"A{i} {r[f'a{i}'][2:]}" for i in range(4, 8)) + "\n" + \
            f"PC {r['pc'][2:]}   SR {r['sr'][2:]}   USP {r.get('usp', '0x?')[2:]}   ISP {r.get('isp', '0x?')[2:]}"
        self.set_text(self.regs, d)

    def show_disasm(self, lines):
        self.set_text(self.dis, "\n".join(l["text"] for l in lines))
        self.dis.tag_add("pc", "1.0", "1.end")

    def dbg_break(self):
        self.do(self.h.debug_break, then=self.show_stop)

    def dbg_step(self):
        self.do(self.h.step, then=self.show_stop)

    def dbg_next(self):
        self.do(self.h.next, then=self.show_stop)

    def dbg_cont(self):
        self.do(self.h.cont)

    def dbg_cont_wait(self):
        def done(r):
            if r.get("stopped"):
                self.show_stop(r)
                self.log(f"stopped: {r['stop']['reason']} at {r['pc']}")
            else:
                self.log("no stop within 30 s")
        self.do(lambda: self.h.cont(True, 30000), then=done)

    def add_breakpoint(self, text):
        text = text.strip()
        if not text:
            return
        cond = any(op in text for op in "=<>!")
        self.do(lambda: self.h.add_breakpoint(cond=text) if cond else self.h.add_breakpoint(addr=text),
                then=lambda r: self.show_breakpoints(r["breakpoints"]))

    def add_breakpoint_dialog(self):
        text = simpledialog.askstring("Breakpoint", "Address, symbol or condition:", parent=self.root)
        if text:
            self.add_breakpoint(text)

    def clear_breakpoints(self):
        self.do(self.h.clear_breakpoints, then=lambda r: self.show_breakpoints(r["breakpoints"]))

    def show_breakpoints(self, bps):
        self.bp_label.config(text="\n".join(f"{b['index']}: {b['expression']}  (hits {b['hits']})"
                                            for b in bps) or "no breakpoints")

    def debug_cmd(self):
        cmd = self.cmd_entry.get().strip()
        if cmd:
            self.do(self.h.debug_cmd, cmd, then=lambda out: self.append_console(f"> {cmd}\n{out}"))

    # ------------------------------------------------------------------ machine

    def show_roms(self, r):
        self.roms = r["roms"]
        self.rom_list.delete(0, "end")
        for rom in self.roms:
            tag = "EmuTOS " + rom.get("emutos_version", "") if rom["emutos"] else "TOS"
            self.rom_list.insert("end", f"{rom['name']}  [{tag} {rom['tos_version']}, {rom['language']}]")
            if rom["name"] and r["current"].endswith(rom["name"]):
                self.rom_list.selection_set("end")
                self.rom_list.see("end")

    def boot_rom(self):
        sel = self.rom_list.curselection()
        if not sel:
            messagebox.showinfo("Boot", "Select a ROM first", parent=self.root)
            return
        name = self.roms[sel[0]]["name"]
        self.do(self.h.select_rom, name, self.machine.get() or None, self.memory.get() or None,
                then=lambda r: self.log(f"booting {name} on {r.get('machine')}"))

    def insert_floppy(self):
        path = filedialog.askopenfilename(title="Floppy image",
                                          filetypes=[("Disk images", "*.st *.msa *.stx *.dim *.ipf *.zip *.gz"),
                                                     ("All files", "*")])
        if path:
            reset = messagebox.askyesno("Floppy", "Reset to boot from it?", parent=self.root)
            self.do(self.h.floppy, path, "a", reset, then=lambda r: self.log(f"inserted {path}"))

    def attach_folder(self):
        path = filedialog.askdirectory(title="Host folder for GEMDOS drive C:")
        if path:
            self.do(self.h.harddrive, path, True, then=lambda r: self.log(f"C: = {path} (rebooting)"))

    def autostart(self):
        prog = simpledialog.askstring("Autostart", "Program to start after boot:",
                                      initialvalue="C:\\PROGRAM.PRG", parent=self.root)
        if prog:
            self.do(self.h.autostart, prog, True, then=lambda r: self.log(f"autostart {prog} (rebooting)"))

    # ------------------------------------------------------------------ files

    def save_screenshot(self, full):
        path = filedialog.asksaveasfilename(defaultextension=".png", filetypes=[("PNG", "*.png")],
                                            initialfile="hatari.png")
        if path:
            def save():
                with open(path, "wb") as fh:
                    fh.write(self.h.screen(full))
                return path
            self.do(save, then=lambda p: self.log(f"saved {p}"))

    def save_state(self):
        path = filedialog.asksaveasfilename(defaultextension=".sav", initialfile="hatari.sav")
        if path:
            self.do(self.h.save_state, path, then=lambda r: self.log(f"state saved to {path}"))

    def load_state(self):
        path = filedialog.askopenfilename(filetypes=[("Hatari snapshots", "*.sav"), ("All", "*")])
        if path:
            self.do(self.h.load_state, path, then=lambda r: self.log(f"state restored from {path}"))

    def run_frames(self):
        n = simpledialog.askinteger("Run", "Frames to run:", initialvalue=100, minvalue=1, parent=self.root)
        if n:
            self.do(self.h.run, n)

    # ------------------------------------------------------------------ processes

    def spawn(self, title, argv, cwd=None):
        """run a command, streaming its output to the console tab"""
        def run():
            self.ui(self.log, f"{title}: {' '.join(argv)}")
            try:
                proc = subprocess.Popen(argv, cwd=cwd, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, text=True)
            except OSError as err:
                self.ui(self.log, f"{title} failed: {err}")
                return
            for line in proc.stdout:
                self.ui(self.append_console, ANSI.sub("", line))
            self.ui(self.log, f"{title} finished (exit {proc.wait()})")
        threading.Thread(target=run, daemon=True).start()

    def start_emulator(self):
        self.console_pos = 0
        self.spawn("start", [os.path.join(HERE, "hatari-agent-run.sh"), "--conout", "2", "--natfeats", "on"])

    def start_interact(self):
        build = os.path.join(INTERACT, "build", "INTERACT.PRG")
        if not os.path.exists(build):
            messagebox.showerror("INTERACT", f"Build it first:\n  cd {INTERACT} && make CROSS=...")
            return
        self.console_pos = 0
        self.spawn("start INTERACT", [os.path.join(INTERACT, "tests", "start.sh"), "--restart"])

    def run_tests(self):
        self.spawn("tests", [os.path.join(INTERACT, "tests", "run-all.sh")])

    def run_scenario(self, name):
        if self.busy:
            self.log("a scenario is already running")
            return

        def run():
            self.busy = True
            self.ui(self.log, f"scenario '{name}' started")
            try:
                scenarios.run(name, self.h, lambda m: self.ui(self.log, m))
                self.ui(self.log, f"scenario '{name}' done")
            except HatariError as err:
                self.ui(self.log, f"scenario '{name}' failed: {err}")
            finally:
                self.busy = False
        threading.Thread(target=run, daemon=True).start()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--api", default=os.environ.get("HATARI_API", "http://127.0.0.1:7777"))
    args = ap.parse_args()
    root = tk.Tk()
    Gui(root, args.api)
    root.mainloop()


if __name__ == "__main__":
    main()
