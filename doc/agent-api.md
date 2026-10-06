# Hatari agent API reference

The agent API is an HTTP/JSON interface for driving Hatari from other
programs: AI agents, test harnesses, CI jobs, MCP bridges or plain `curl`.
It covers emulation control, keyboard/mouse/joystick input, screenshots,
TOS ROM selection, media, memory and register access, the debugger,
snapshots and program console output.

The design is described in [agent-design.md](agent-design.md). For
debugging with GDB (`--gdb-port`) see [agent-gdb.md](agent-gdb.md).

## Starting

```sh
hatari --agent-port 7777 --agent-rom-dir roms --tos roms/emutos/emutos-512k-1.4/etos512us.img
# or, from the source tree (waits until the API answers):
tools/agent/hatari-agent-run.sh --machine ste
```

| Option | Meaning |
|---|---|
| `--agent-port <port>` | Enable the API on this TCP port. |
| `--agent-bind <addr>` | IPv4 address to listen on. Default `127.0.0.1`. **There is no authentication**, so don't expose it beyond localhost. |
| `--agent-rom-dir <dir>` | Directory scanned (recursively) by `GET /roms`. `tools/agent/fetch-emutos.sh` fills it with the free EmuTOS images. |
| `--agent-debugger <bool>` | Default `on`: when the CPU stops (breakpoint, step, exception, NatFeats debugger call...), control goes to the API instead of the console debugger. |

Useful companion options: `--conout 2` (capture TOS console output,
see `/console`), `--natfeats on` (programs can print with `NF_STDERR`),
`--harddrive <dir>` (host directory as GEMDOS drive C:), `--fast-forward on`,
`--sound off`.

When the API is enabled, non-fatal host alert dialogs are logged instead
of shown and quit confirmation is disabled, because a remote client can't
click a modal dialog away. Changes made through the API never ask for
confirmation.

## Conventions

* Every endpoint answers JSON, except `GET /screen` (image) and `GET /mem?format=bin`.
  Success is `{"ok":true,...}`. Errors are `{"ok":false,"error":"..."}` with
  status 400 (bad parameters), 404 (unknown endpoint), 405 (wrong method),
  408 (timed out), 409 (wrong emulator state, e.g. paused) or 500.
* Parameters can be sent in the query string or as an
  `application/x-www-form-urlencoded` body (`curl -d`). For text and hex data
  you can also send the raw body (`--data-binary`).
* **`+` in a query string means space.** Use `curl --data-urlencode`, or `%2B`.
  Key combos accept both `ctrl+c` and `ctrl c` for this reason.
* Numbers and addresses: `0x...` hex or decimal. Address parameters also
  accept Hatari debugger expressions (`$fc0030`, `a0+4`, `pc`, symbol names,
  e.g. `addr=main` after loading symbols).
* Most action responses include the common status fields:
  `state` (`running` / `paused` / `stopped`), `frame` (emulated frame counter,
  monotonic across resets), `pc`, and `stop` (last debugger stop:
  `reason`, `count`, `pc`).
* Coordinates are **emulated screen pixels** (e.g. 320x200 in ST low
  resolution), the same as in `GET /screen` images.
* Input needs emulated time. Endpoints that press/release keys or buttons
  over several frames reply when the input sequence is done, and fail with
  409 while emulation is paused or stopped in the debugger.
* GEM reacts to input over a few frames. After clicking a menu or opening
  a window, `POST /emu/run?frames=10&pause=0` (or more) before taking the
  screenshot that should show the result.
* Requests are executed in arrival order on the emulator thread. While the
  Hatari GUI (F12) or another host dialog is open, requests wait.

## Endpoints

`GET /` returns the endpoint list with one-line help (machine readable).

### Status and emulation control

| Method & path | Parameters | Description |
|---|---|---|
| `GET /status` | | State, frame, PC, machine, CPU level, RAM, TOS (path, version, EmuTOS), fast forward, breakpoints, console byte count, ROM dir. |
| `POST /emu/pause` | | Pause emulation. |
| `POST /emu/resume` | | Resume emulation. Also leaves a debugger stop. |
| `POST /emu/run` | `frames=N` (1), `pause=1` | Run exactly N emulated frames, then pause (`pause=0` keeps running). Works from paused, running or stopped. Replies when done. Use this for deterministic, frame-stepped interaction. |
| `POST /emu/reset` | `type=cold\|warm` | Reset (default cold). |
| `POST /emu/fastforward` | `on=1\|0` | Run as fast as possible. Handy while booting or waiting for long operations. |
| `POST /emu/quit` | `code=N` | Quit Hatari with the given exit code. |
| `POST /config` | `args=...` | Apply Hatari command line options, e.g. `args=--machine falcon --memsize 4096`. Resets the emulation when the options need it. Escape spaces inside values with a backslash. Response includes Hatari's parser output. |

### ROMs and media

| Method & path | Parameters | Description |
|---|---|---|
| `GET /roms` | | Lists TOS images in `--agent-rom-dir`, with `name` (relative path), `size_kb`, `tos_version`, `country`, `language`, `emutos`, `emutos_version`, plus `current`. |
| `POST /roms/select` | `name=`, optional `machine=st\|megast\|ste\|megaste\|tt\|falcon`, `memory=KB` | Switch TOS image (and machine) and cold boot. `name` is from `/roms`, or an absolute path. |
| `POST /media/floppy` | `path=`, `drive=a\|b`, `reset=0` | Insert a floppy image (`.st`, `.msa`, `.stx`, `.dim`, `.ipf`, zip...). |
| `POST /media/harddrive` | `path=`, `reset=1` | Attach a host directory as GEMDOS drive C:. This is the easiest way to get freshly built programs into the emulator. |
| `POST /media/autostart` | `program=C:\X.PRG`, `reset=1` | Start this program automatically after the next boot (Hatari `--auto`). |

EmuTOS image choice: 192k for ST, 256k for STE, 512k for STE/TT/Falcon
(also works on ST in Hatari), 1024k for all machines and languages. The 512k
and 1024k images include the EmuCON shell (desktop *File > Execute EmuCON*,
or `ctrl+z`).

### Screen

| Method & path | Parameters | Description |
|---|---|---|
| `GET /screen` | `full=0`, `format=png\|bmp\|neo\|ximg` | PNG screenshot of the emulated display area at **native emulated resolution** (320x200 / 640x200 / 640x400, TT and Falcon modes), so pixel coordinates are mouse coordinates. `full=1` gives the host window contents instead (borders, zoom, statusbar). `neo`/`ximg` are Atari image formats of the raw screen memory. |

Note: when you look at a screenshot through a tool that rescales images
for display, measure positions relative to the real image size (320x200
etc.), not the displayed size.

### Input

| Method & path | Parameters | Description |
|---|---|---|
| `POST /input/type` | `text=` or raw body, `frames=1` | Type ASCII text (US keyboard layout). `\n` is Return, `\t` Tab, `\b` Backspace, ESC (0x1b) Esc. `frames` per key event; increase it if a program drops keys. |
| `POST /input/key` | `key=`, `action=press\|down\|up`, `frames=2` | Press one key or combo. `key` is a name (below), a single character, or a raw ST scancode (`0x1c`). Combos: `ctrl+c`, `alt+x`, `shift+f1`, `ctrl+alt+delete`. `down`/`up` apply at once without needing emulated time (to hold a key, e.g. for a game). |
| `GET /input/mouse` | | GEM pointer position (`x`, `y`) read from the Line-A variables, and button state. 409 when no GEM is running. |
| `POST /input/mouse` | `x=,y=` or `dx=,dy=`, `frames=4` | Absolute or relative move. Absolute moves steer the GEM pointer along a straight line until GEM reports it at the target (so open menus stay open). Without GEM (e.g. games), absolute moves pin the pointer to the top-left corner first. Relative moves are sent as IKBD mouse packets. |
| `POST /input/click` | `button=left\|right`, `action=click\|double\|down\|up`, optional `x=,y=`, `frames=3` | Click (optionally moving there first). `frames` = how long the button is held. |
| `POST /input/joystick` | `port=1`, `dirs=up,down,left,right,fire,none`, `fire=1`, `frames=N` | Set ST joystick state. `frames=0` (default) latches it until changed, `frames=N` holds for N frames then releases (replies when done). Port 1 is the normal joystick port, port 0 the mouse port (only read when a program switches the IKBD to joystick mode). Combined with real/emulated joystick input. |

Key names: `esc return enter backspace tab space delete insert home clrhome
help undo up down left right f1`..`f10 ctrl control alt shift lshift rshift
capslock plus minus kp0`..`kp9 kp( kp) kp/ kp* kp- kp+ kp. kpenter`.

### Memory and CPU

| Method & path | Parameters | Description |
|---|---|---|
| `GET /mem` | `addr=`, `len=256`, `format=hex\|bin` | Read memory: `{"hex":"..."}`, or raw bytes with `format=bin`. Reads in the IO area (`$ff8000`+) go through the emulated hardware and may have side effects. |
| `POST /mem` | `addr=`, `hex=` or raw hex body | Write bytes (white-space in hex is ignored). |
| `GET /cpu/regs` | | `pc sr d0-d7 a0-a7 usp isp` (and `msp vbr cacr caar sfc dfc` on 68020+). `a7` is the active stack pointer. |
| `POST /cpu/regs` | `d0=...&pc=...` | Set registers (paused or stopped only). |
| `GET /cpu/disasm` | `addr=` (PC), `count=16` | Disassembly, `{"lines":[{"addr","text"}]}`. |

### Debugger

With `--agent-debugger on` (the default), every CPU stop (breakpoint, step,
exception configured with `--debug-except`, `NF_DEBUGGER`...) is handed to
the API: the emulator freezes in the stop, keeps serving requests, and
waits for `/debug/continue`, `/debug/step` etc.

| Method & path | Parameters | Description |
|---|---|---|
| `POST /debug/break` | `timeout_ms=5000` | Stop the CPU at the next instruction. Replies with the stop state (`regs`, `disasm`). |
| `POST /debug/continue` | `wait=0`, `timeout_ms=10000` | Leave the stop. With `wait=1`, block until the next stop (e.g. the next breakpoint hit) and return its state. |
| `POST /debug/step` | `count=1` | Execute N instructions, return the new stop state. |
| `POST /debug/next` | `type=` | Step over subroutine calls / traps (Hatari debugger `next`, with optional instruction `type` such as `subreturn`). |
| `POST /debug/wait` | `timeout_ms=10000`, `current=1` | Block until the CPU stops. Returns at once when already stopped (unless `current=0`). On timeout returns `{"stopped":false}`. |
| `GET /debug/breakpoints` | | `[{"index","expression","hits","once","trace"}]` |
| `POST /debug/breakpoints` | `addr=` or `cond=`, `options=` | Address breakpoint (address/symbol/expression) or conditional breakpoint using Hatari's condition syntax, e.g. `cond=d0=$20 && pc>$e00000`, `cond=($ff8240).w ! ($ff8240).w` (value changed). `options`: `once`, `trace`, `quiet`, `lock`, a hit count, ... (see Hatari debugger `b help`). |
| `DELETE /debug/breakpoints` | `index=N\|all` | Remove breakpoint(s). |
| `POST /debug/cmd` | `cmd=`, `wait=0` | Run any Hatari debugger command and get its output, e.g. `info osheader`, `symbols prg`, `profile on`, `m $ff8240-$ff8260`, `trace gemdos`. If the command resumes emulation (`c`, `s`, `n`...), `resumed` is true and `wait=1` waits for the next stop. |

A stop state response looks like:

```json
{"ok":true,"stopped":true,"state":"stopped","frame":7243,"pc":"0xe1fe06",
 "stop":{"reason":"breakpoint","count":3,"pc":"0xe1fe06"},
 "regs":{"pc":"0xe1fe06","sr":"0x2300","d0":"0x00002304",...},
 "disasm":[{"addr":"0xe1fe06","text":"00e1fe06 4e72 2300   stop #$2300"},...]}
```

Stop reasons: `breakpoint`, `step`, `user` (from `/debug/break` or the
debugger shortcut), `cpu_exception`, `program` (`NF_DEBUGGER`, Hatari
xbios), `dsp_*`.

### Snapshots and console

| Method & path | Parameters | Description |
|---|---|---|
| `POST /state/save` | `path=` | Save a full emulation snapshot (memory, CPU, chips). |
| `POST /state/load` | `path=`, `pause=0` | Restore a snapshot. Takes one frame. Stays paused if it was paused. Snapshots need the same machine configuration they were saved with. |
| `GET /console` | `since=0` | Text the emulated program wrote to the TOS console (needs `--conout 2`) or with NatFeats `NF_STDERR` (needs `--natfeats on`). Returns `text`, `from`, `next`. Pass `next` as `since` to get only new output. Keeps the last 256 KB. |

## Recipes

Boot to the desktop and look at it:

```sh
curl -s -XPOST localhost:7777/emu/fastforward -d on=1
curl -s -XPOST 'localhost:7777/emu/run?frames=600&pause=0'
curl -s -XPOST localhost:7777/emu/fastforward -d on=0
curl -s -o screen.png localhost:7777/screen
```

Run a freshly built program from a host directory and read its output:

```sh
curl -s -XPOST localhost:7777/media/harddrive --data-urlencode "path=$PWD/build-st"
curl -s -XPOST localhost:7777/media/autostart --data-urlencode 'program=C:\TEST.TOS'
curl -s -XPOST 'localhost:7777/emu/run?frames=900&pause=0'
curl -s localhost:7777/console        # with --conout 2 / --natfeats on
```

Open a GEM menu entry:

```sh
curl -s -XPOST 'localhost:7777/input/mouse?x=82&y=4'      # hover "File"
curl -s -XPOST 'localhost:7777/emu/run?frames=10&pause=0'
curl -s -XPOST 'localhost:7777/input/click?x=100&y=127'   # click an entry
```

Type into a shell:

```sh
curl -s -XPOST localhost:7777/input/key --data-urlencode 'key=ctrl+z'   # EmuCON
curl -s -XPOST localhost:7777/input/type --data-binary $'dir C:\\\n'
```

Break on a program's entry point and single-step:

```sh
curl -s -XPOST localhost:7777/debug/breakpoints --data-urlencode 'cond=pc=TEXT'
curl -s -XPOST 'localhost:7777/debug/wait?timeout_ms=30000'
curl -s -XPOST 'localhost:7777/debug/step?count=1'
curl -s 'localhost:7777/mem?addr=a0&len=32'
curl -s -XPOST localhost:7777/debug/continue
```

(`TEXT` is the debugger variable for the program's text segment start,
available once GEMDOS has loaded it. Use `--debug-except` / `--bios-intercept`
and `/debug/cmd` for more Hatari debugger features.)

Deterministic test loop: save a snapshot once at the interesting point,
then for each test: `/state/load`, inject input, `/emu/run?frames=N`,
compare `/screen` or `/mem` with the expected result.
