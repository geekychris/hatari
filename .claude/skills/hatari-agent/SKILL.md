---
name: hatari-agent
description: Drive the Hatari Atari ST/STE/TT/Falcon emulator through its HTTP agent API - boot TOS/EmuTOS ROMs, type/click/joystick input, native-resolution screenshots, program console output, snapshots, memory/registers and the 68k debugger (breakpoints, stepping). Use when running, testing or debugging Atari software in this repo's emulator, or when asked to "start Hatari", "look at the Atari screen", "click/type in the emulator", "boot TOS", "set a breakpoint in the 68k code".
---

# Driving Hatari through the agent API

Full reference: `doc/agent-api.md`. Design: `doc/agent-design.md`.

## Start / stop

```sh
tools/agent/fetch-emutos.sh                      # once; fills roms/ with EmuTOS
tools/agent/hatari-agent-run.sh --machine ste --conout 2 --sound off
curl -s localhost:7777/status                     # state, machine, TOS, frame
curl -s -XPOST localhost:7777/emu/quit            # when done
```

Add `--harddrive <dir>` to expose a host directory as drive C: (best way to
get freshly built programs in), `--natfeats on` so programs can print to
`/console`, and `--fast-forward on` to boot quickly (switch it off with
`POST /emu/fastforward on=0` before timing-sensitive input).

## Core loop: act, advance time, observe

1. Act: `/input/*`, `/media/*`, `/roms/select`, `/debug/*`.
2. Let emulated time pass: `POST /emu/run?frames=N&pause=0` (50/60 frames ≈ 1 s).
   GEM needs ~5-10 frames to react to a click or hover, and programs need
   more to start. `pause=1` (default) leaves it paused, for deterministic steps.
3. Observe:
   - `curl -s -o /tmp/screen.png localhost:7777/screen`, then Read the PNG.
     It's at native resolution (320x200 low, 640x200 medium, 640x400 high).
     **Pixel coordinates in the PNG are the coordinates to click.** If the image
     viewer shows it scaled, convert back to the real image size.
   - `GET /console?since=N` for text the program printed (needs `--conout 2`
     or NatFeats). Prefer it over reading text from screenshots.
   - `GET /mem?addr=...&len=...`, `GET /cpu/regs` for exact state.

## Input cheat sheet

```sh
curl -s -XPOST 'localhost:7777/input/mouse?x=82&y=4'            # absolute, GEM coords
curl -s -XPOST 'localhost:7777/input/click?x=100&y=127'         # move + left click
curl -s -XPOST 'localhost:7777/input/click?button=right'        # click in place
curl -s -XPOST 'localhost:7777/input/click?x=40&y=30&action=double'
curl -s -XPOST localhost:7777/input/key --data-urlencode 'key=ctrl+z'   # combos: use --data-urlencode
curl -s -XPOST 'localhost:7777/input/key?key=return'
curl -s -XPOST localhost:7777/input/type --data-binary $'dir C:\\\n'    # US layout, \n = Return
curl -s -XPOST 'localhost:7777/input/joystick?port=1&dirs=right,fire&frames=25'
curl -s localhost:7777/input/mouse                               # where GEM thinks the pointer is
```

Input endpoints reply after the whole sequence was delivered, and return
409 when paused or stopped. Resume first. A `+` in a query string is a space,
so use `--data-urlencode` for anything with `+`, `&`, `\` or spaces.

## Debugging 68k code

```sh
curl -s -XPOST localhost:7777/debug/break                        # stop now -> regs + disasm
curl -s -XPOST 'localhost:7777/debug/step?count=1'
curl -s -XPOST localhost:7777/debug/next                         # step over jsr/bsr/trap
curl -s -XPOST localhost:7777/debug/breakpoints -d 'addr=0xe1fe06'
curl -s -XPOST localhost:7777/debug/breakpoints --data-urlencode 'cond=pc=TEXT'   # program entry
curl -s -XPOST localhost:7777/debug/breakpoints --data-urlencode 'cond=($ff8240).w ! ($ff8240).w'  # on change
curl -s -XPOST 'localhost:7777/debug/continue?wait=1&timeout_ms=20000'  # run to next stop
curl -s 'localhost:7777/cpu/disasm?addr=pc&count=10'
curl -s 'localhost:7777/mem?addr=a0&len=64'
curl -s -XPOST localhost:7777/debug/cmd --data-urlencode 'cmd=info gemdos'   # any Hatari debugger cmd
curl -s -XDELETE 'localhost:7777/debug/breakpoints?index=all'
```

While `state` is `stopped` the CPU is frozen in the debugger. Only
`/debug/*`, `/cpu/*`, `/mem`, `/screen`, `/status`, `/console` make sense
until you continue. `/emu/resume` or `/emu/run` also leave the stop.

## GDB (source-level / IDE debugging)

Start Hatari with `--gdb-port 2159`, then `gdb -x tools/agent/hatari.gdb [prog.elf]`
(Homebrew `gdb` supports m68k; `set endian big` is required without an m68k ELF).
`monitor <hatari debugger cmd>` works inside GDB. Load program symbols with
`add-symbol-file prog.elf -o 0x<TEXT>` (TEXT from `monitor info basepage`).
Details: `doc/agent-gdb.md`. GDB and the HTTP API can be attached together;
let one of them drive stepping at a time.

## Machines and ROMs

```sh
curl -s localhost:7777/roms | python3 -m json.tool | head -40
curl -s -XPOST localhost:7777/roms/select -d 'name=emutos/emutos-192k-1.4/etos192us.img&machine=st'
curl -s -XPOST localhost:7777/roms/select -d 'name=emutos/emutos-1024k-1.4/etos1024k.img&machine=falcon&memory=4096'
```

192k = ST, 256k = STE, 512k = STE/TT/Falcon, 1024k = all machines and languages.
Pick a `us` image when you'll use `/input/type` (US layout). The 512k/1024k
images have the EmuCON shell (`ctrl+z` on the desktop).

## Deterministic testing

Save a snapshot at a known point (`POST /state/save path=/tmp/x.sav`), then
per test case: `/state/load`, input, `/emu/run?frames=N`, compare `/screen`,
`/mem` or `/console`. Restore only works with the same machine config.

## Examples to copy from

- `examples/interact/tests/lib.sh`: shell helpers (wait_for console regex, click by layout name, shots).
- `tools/agent/hatari_agent.py`: Python client (`Hatari().click(x, y)` etc.).
- `tools/agent/dma_sound_capture.py out.wav 5`: record STE/Falcon DMA sound to WAV (works with `--sound off`).
- `fractalus/rescue_test.py` in [atari_st_games](https://github.com/geekychris/atari_st_games) (the
  game ports' own repo): scenario test that teleports the player by writing game state through
  `/mem` (addresses logged by the program), then follows a state machine with screenshots.
- Performance work: `profile on`, run, break, `profile save f.txt`, then
  `python3 tools/debugger/hatari_profile.py -st -i f.txt` (needs `symbols prg` first).
- Self-describing app pattern: the program prints `LAYOUT`/`SYMBOL`/event lines via NatFeats
  (`examples/gemdemo/natfeats.c`), so tests don't guess coordinates from pixels.
- C cross toolchain: `tools/agent/fetch-cross-mint.sh`, build with `m68k-atari-mintelf-gcc ... -lgem`.

## Gotchas

- Never leave Hatari's own GUI (F12) open: requests wait until it closes.
- Joystick: TOS boots with IKBD joystick reports off; programs send IKBD `0x14,0x08`.
- If Hatari never answers after launch, check `sample <pid>`: on macOS it can hang in CoreAudio
  while opening the sound device. Relaunch with `--sound off`.
  With the mouse on, joystick-1 fire arrives as the right mouse button.
- `/emu/run` with large `frames` takes real time. Combine with fast forward.
- If the API doesn't answer, check `/tmp/hatari-agent.log`. Another Hatari
  might hold the port (`lsof -nP -iTCP:7777`).
