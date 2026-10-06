# Agentic development with Hatari: design

This fork adds a control plane to Hatari so that software agents (LLM coding
agents such as Claude, test harnesses, CI) can develop, run, test and debug
Atari ST/STE/TT/Falcon software end to end, without a human at the keyboard.

The goal is **strong, deterministic control**: an agent must be able to
put the machine into a known state, feed it input, observe the result
(pixels, memory, text output), and stop and inspect the CPU, all through
one documented interface.

The protocol reference is [agent-api.md](agent-api.md).

## What existed before

Hatari already had useful pieces, which this work builds on rather than
replaces:

* `--control-socket` / `--cmd-fifo` (`src/control.c`): a one-way text
  command channel with key/mouse event injection, shortcuts (reset,
  screenshot), option changes and debugger command pass-through.
  **It never replies.** Commands can't fail visibly, can't return data,
  and debugger output goes to Hatari's stdout.
* A capable built-in debugger (`src/debug/`): conditional breakpoints,
  stepping, memory and register commands, symbols, profiling, tracing.
  It is **console-bound**: on a breakpoint it blocks reading commands from
  stdin.
* Screenshots, memory snapshots, `--conout` console capture to stdout,
  NatFeats, GEMDOS host-directory drives and `--auto` program start.

These need a human, or a process that scrapes stdout. Nothing tells a
client *when* something happened (a breakpoint hit, N frames passed, input
consumed).

## Design

```
 agent / curl / MCP bridge
          │  HTTP/1.1 + JSON (localhost)
 ┌────────▼─────────┐
 │ agenthttp.c      │ listener thread + one thread per connection;
 │                  │ parses request, queues it, blocks until completed
 └────────┬─────────┘
          │ queue (mutex/cond)          wake-up: SDL user event
 ┌────────▼──────────────────────────────────────────────────────────┐
 │ agentapi.c  (runs ONLY on the emulator main thread)               │
 │  AgentApi_Poll()      from GuiEvent_EventHandler: several times   │
 │                       per frame while running; on wake-up/100ms   │
 │                       while paused                                │
 │  AgentApi_DebugStop() replaces the console loop in DebugUI() when │
 │                       the CPU stops; serves requests until resumed│
 │  jobs                 requests that need emulated time (typing,   │
 │                       clicks, run N frames, wait for stop)        │
 └───────────────────────────────────────────────────────────────────┘
```

### Threading: emulator state is touched only on the main thread

Hatari is single-threaded and its state is global. Rather than add locking
across the emulator, network threads only parse HTTP and hand complete
requests to the main thread, which executes them at well-defined points.
These are the same points where the existing control socket and the
debugger already run:

1. **Between instructions in the event handler** (`IKBD_InterruptHandler_AutoSend`
   → `GuiEvent_EventHandler` → `AgentApi_Poll`). This is where keyboard
   shortcuts like reset are handled today, so reset, option changes and
   snapshot capture are safe here.
2. **While paused**: the SDL event loop waits with a 100 ms timeout when the
   API is enabled, and network threads push an SDL user event to wake it at
   once.
3. **While stopped in the debugger**: `DebugUI()` calls
   `AgentApi_DebugStop()` instead of reading stdin. It loops on the request
   queue until a request resumes emulation.

So request handlers are plain sequential C with no locks.

### Jobs: asynchronous requests over emulated time

Many interactions only make sense over emulated time. A key press has to
be seen by the OS before the release. A click needs the button held for a
few frames. "Run 100 frames" or "wait for the breakpoint" finish later.
Such requests become **jobs** that are checked on every poll and complete
the HTTP request when done. This keeps the client simple: one blocking
request = one complete action. It also makes interaction
**frame-deterministic**: input is delivered at given emulated frame
offsets, not wall-clock times, so results don't depend on host speed or
fast-forward.

A monotonic frame counter (`Frames`, derived from `nVBLs`, which resets on
reset) is the time base.

### Debugger ownership

With `--agent-debugger on` (default), every debugger entry (breakpoints,
steps, configured exceptions, `NF_DEBUGGER`, the debugger shortcut) is
routed to the API. The CPU freezes inside the stop, exactly as with the
console debugger. Meanwhile the API serves registers, memory, disassembly,
breakpoint edits, arbitrary debugger commands (output captured by
temporarily redirecting stdout/stderr) and resume requests. Clients can
long-poll `/debug/wait` or use `wait=1` on continue/step to block until the
next stop. Breaking a running program sets the debugger's single-step
counter to 1 (`DebugCpu_RequestBreak()`), so the existing per-instruction
debugger check makes the stop.

All of Hatari's debugger stays available through `/debug/cmd`, so the API
doesn't need its own endpoint for every debugger feature.

### Seeing the screen the way the program does

`GET /screen` returns the emulated display area at **native resolution**
(e.g. 320x200), cropped from Hatari's zoomed, bordered host surface
(`ConvST_GetDisplayArea()`, `ScreenSnapShot_SavePNG_Native()`). Image
coordinates equal GEM mouse coordinates, so an agent can find something
in a screenshot and click it without any conversion. `full=1` keeps the
old full-window capture.

### Precise pointer control

The ST mouse is relative (IKBD packets), so "click at (x,y)" isn't
directly possible. The API finds the Line-A variable block in RAM (by its
screen-geometry signature, so it works with any TOS/EmuTOS), reads GEM's
pointer position (`GCURX/GCURY`), and steers the pointer along a straight
line in chunks of at most one IKBD packet per frame until GEM reports the
target position. A straight path matters: menus close if the pointer
leaves them. Without GEM (games), absolute moves fall back to pinning the
pointer into the top-left corner first.

### Text instead of OCR

Console output (`--conout 2`, BIOS device 2 through Hatari's VT52 filter)
and NatFeats `NF_STDERR` output are copied into a 256 KB ring buffer that
clients read incrementally (`/console?since=`). A program under test can
just print, and the agent reads text instead of parsing screenshots.

### No modal dialogs

A remote client can't dismiss a host dialog, and a modal dialog blocks the
main thread and with it the whole API. While the API is enabled,
non-fatal alerts are logged instead of shown and quit confirmation is
off. Option changes made through the API never ask the "must reset,
continue?" question (`apply_cmdline()`).

### GDB remote stub

`--gdb-port` adds a GDB remote serial protocol stub (`src/gdbstub.c`,
documented in [agent-gdb.md](agent-gdb.md)). It doesn't have its own
stop machinery. It plugs into the same pieces as the HTTP API:

* **Stops**: a connected GDB makes `AgentApi_OwnsDebugger()` true, so
  `DebugUI()` enters the shared stop loop, which calls
  `GdbStub_NotifyStop()` (stop reply, including `watch:` for watchpoints)
  and polls the GDB socket next to the HTTP queue.
* **Resume / break**: `AgentApi_RequestResume()` and
  `AgentApi_RequestBreak()`, the same calls the HTTP endpoints use.
  Single step uses `DebugCpu_RequestBreak()`.
* **Breakpoints**: GDB `Z0` and `Z2` become Hatari conditional breakpoints
  (`pc=$addr`, `($addr).w ! ($addr).w` for value-change watchpoints), so
  they show up in `/debug/breakpoints` and `monitor b` and cost nothing
  when unused.
* **`monitor`**: passes through to the Hatari debugger with captured
  output, so GDB users get every Hatari debugger feature.

The socket is non-blocking and serviced on the main thread, so it needs
no threads. The stop reply is only sent when GDB waits for one. An
unsolicited stop reply after attach confuses GDB's protocol state.

### Security

The API listens on 127.0.0.1 by default and has **no authentication**.
Anyone who can connect can read and write emulated memory, change
options (including host paths used as GEMDOS drives) and quit the
emulator. Bind to other addresses only on trusted networks.

## Changes to existing Hatari code

Kept small, so the fork can keep merging upstream:

| File | Change |
|---|---|
| `src/agentapi.c`, `src/agenthttp.c`, `src/includes/agent*.h` | New: API and HTTP server. |
| `src/gdbstub.c`, `src/includes/gdbstub.h` | New: GDB remote stub. |
| `src/options.c` | `--agent-port`, `--agent-bind`, `--agent-rom-dir`, `--agent-debugger`, `--gdb-port`. |
| `src/main.c` | Start and stop the API. |
| `src/sdl/gui_event.c` | Poll the API. Wake-up event, wait timeout while paused. |
| `src/debug/debugui.c` | Route debugger stops to the API. `DebugUI_RemoteCommand()` returns the command's resume code and doesn't repeat the previous command on empty input. |
| `src/debug/debugcpu.c` | `DebugCpu_RequestBreak()`. |
| `src/debug/breakcond.c` | `BreakCond_GetCpuBreakPoint()` for structured breakpoint lists. |
| `src/debug/console.c`, `src/debug/natfeats.c` | Copy console output to the API ring buffer. |
| `src/joy.c` | OR API joystick state into the ST joystick ports. |
| `src/conv_st.c`, `src/screenSnapShot.c` | Display-area geometry. Native-resolution PNG. **Bug fix:** `ScreenSnapShot_SavePNG_ToFile()` scaled each row using the uncropped source width, which gave wrong output when cropping left/right (unnoticed upstream because only top/bottom cropping was used). |
| `src/retro/gui_event.c` | Stub for the libretro build. |

The API compiles only where Unix sockets exist (`HAVE_UNIX_DOMAIN_SOCKETS`,
same as the control socket). Elsewhere `--agent-port` reports it's
unsupported.

## Tooling

* `tools/agent/fetch-emutos.sh`: download all EmuTOS images (free, GPL)
  into `roms/` (git-ignored). Original Atari TOS images are copyrighted. If
  you own them, drop them into the same folder and `/roms` lists them.
* `tools/agent/hatari-agent-run.sh`: start Hatari with the API and wait
  until it answers.
* `tools/agent/hatari.gdb`: GDB init file (big endian, connect).
* `.claude/skills/hatari-agent/SKILL.md`: instructions that let Claude Code
  drive the emulator through the API.

## Roadmap

* MCP server wrapper exposing the endpoints as MCP tools (screenshots as
  image content).
* Joystick port 0 / IKBD joystick-mode helpers, STE joypads.
* Wait-for-condition endpoint (memory value, screen region change, console
  text match) to avoid client-side polling.
* Keyboard layouts other than US for `/input/type`.
* Windows support for the server (Winsock).
