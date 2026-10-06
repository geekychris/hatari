"""Python client for the Hatari agent API (stdlib only).

    from hatari_agent import Hatari
    h = Hatari()                      # http://127.0.0.1:7777
    h.status()["state"]
    png = h.screen()                  # bytes, native resolution
    h.click(82, 4); h.run(10, pause=False)
    h.type_text("dir\\n")
    h.debug_break(); h.step(); h.cont()

Every method raises HatariError when the API answers {"ok": false}.
See doc/agent-api.md for the endpoints.
"""
import json
import urllib.error
import urllib.parse
import urllib.request


class HatariError(Exception):
    pass


class Hatari:
    def __init__(self, base="http://127.0.0.1:7777", timeout=60):
        self.base = base.rstrip("/")
        self.timeout = timeout

    # --- transport ------------------------------------------------------

    def request(self, method, path, params=None, body=None, raw=False, timeout=None):
        url = self.base + path
        if params:
            url += "?" + urllib.parse.urlencode(
                {k: v for k, v in params.items() if v is not None})
        data = body.encode() if isinstance(body, str) else body
        req = urllib.request.Request(url, data=data, method=method)
        try:
            with urllib.request.urlopen(req, timeout=timeout or self.timeout) as resp:
                payload = resp.read()
        except urllib.error.HTTPError as err:
            payload = err.read()
            try:
                msg = json.loads(payload).get("error", payload.decode())
            except ValueError:
                msg = payload.decode(errors="replace")
            raise HatariError(f"{method} {path}: {err.code} {msg}") from None
        except (urllib.error.URLError, ConnectionError, TimeoutError) as err:
            raise HatariError(f"{method} {path}: {err}") from None
        if raw:
            return payload
        result = json.loads(payload)
        if not result.get("ok", True):
            raise HatariError(f"{method} {path}: {result.get('error')}")
        return result

    def get(self, path, **params):
        return self.request("GET", path, params)

    def post(self, path, body=None, **params):
        return self.request("POST", path, params, body)

    def alive(self):
        try:
            self.request("GET", "/status", timeout=2)
            return True
        except HatariError:
            return False

    # --- emulation --------------------------------------------------------

    def status(self):
        return self.get("/status")

    def pause(self):
        return self.post("/emu/pause")

    def resume(self):
        return self.post("/emu/resume")

    def run(self, frames=1, pause=True):
        return self.post("/emu/run", frames=frames, pause=int(pause))

    def reset(self, cold=True):
        return self.post("/emu/reset", type="cold" if cold else "warm")

    def fast_forward(self, on=True):
        return self.post("/emu/fastforward", on=int(on))

    def quit(self):
        return self.post("/emu/quit")

    def config(self, args):
        return self.post("/config", args=args)

    # --- media & ROMs -----------------------------------------------------

    def roms(self):
        return self.get("/roms")

    def select_rom(self, name, machine=None, memory=None):
        return self.post("/roms/select", name=name, machine=machine, memory=memory)

    def floppy(self, path, drive="a", reset=False):
        return self.post("/media/floppy", path=path, drive=drive, reset=int(reset))

    def harddrive(self, path, reset=True):
        return self.post("/media/harddrive", path=path, reset=int(reset))

    def autostart(self, program, reset=True):
        return self.post("/media/autostart", program=program, reset=int(reset))

    # --- screen -----------------------------------------------------------

    def screen(self, full=False):
        """PNG bytes; native resolution unless full=True"""
        return self.request("GET", "/screen", {"full": int(full)}, raw=True)

    # --- input ------------------------------------------------------------

    def type_text(self, text, frames=1):
        return self.request("POST", "/input/type", {"frames": frames}, body=text)

    def key(self, key, action="press"):
        return self.post("/input/key", key=key, action=action)

    def mouse(self, x=None, y=None, dx=None, dy=None, frames=None):
        return self.post("/input/mouse", x=x, y=y, dx=dx, dy=dy, frames=frames)

    def mouse_pos(self):
        r = self.get("/input/mouse")
        return r["x"], r["y"]

    def click(self, x=None, y=None, button="left", action="click"):
        return self.post("/input/click", x=x, y=y, button=button, action=action)

    def button(self, action, button="left"):
        """action: down / up"""
        return self.post("/input/click", button=button, action=action)

    def joystick(self, dirs="none", frames=0, port=1):
        return self.post("/input/joystick", port=port, dirs=dirs, frames=frames)

    # --- memory & CPU -------------------------------------------------------

    def read_mem(self, addr, length=16):
        return bytes.fromhex(self.get("/mem", addr=addr, len=length)["hex"])

    def write_mem(self, addr, data):
        return self.post("/mem", addr=addr, hex=data.hex())

    def regs(self):
        return self.get("/cpu/regs")["regs"]

    def disasm(self, addr=None, count=16):
        return self.get("/cpu/disasm", addr=addr, count=count)["lines"]

    # --- debugger -----------------------------------------------------------

    def debug_break(self):
        return self.post("/debug/break")

    def step(self, count=1):
        return self.post("/debug/step", count=count)

    def next(self):
        return self.post("/debug/next")

    def cont(self, wait=False, timeout_ms=10000):
        return self.post("/debug/continue", wait=int(wait), timeout_ms=timeout_ms)

    def wait_stop(self, timeout_ms=10000):
        return self.post("/debug/wait", timeout_ms=timeout_ms)

    def breakpoints(self):
        return self.get("/debug/breakpoints")["breakpoints"]

    def add_breakpoint(self, addr=None, cond=None, options=None):
        return self.post("/debug/breakpoints", addr=addr, cond=cond, options=options)

    def clear_breakpoints(self, index="all"):
        return self.request("DELETE", "/debug/breakpoints", {"index": index})

    def debug_cmd(self, cmd):
        return self.post("/debug/cmd", cmd=cmd)["output"]

    # --- state & console -----------------------------------------------------

    def save_state(self, path):
        return self.post("/state/save", path=path)

    def load_state(self, path):
        return self.post("/state/load", path=path)

    def console(self, since=0):
        """returns (text, next_offset)"""
        r = self.get("/console", since=since)
        return r["text"], r["next"]
