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

## Gotchas

- Never leave Hatari's own GUI (F12) open: requests wait until it closes.
- `/emu/run` with large `frames` takes real time. Combine with fast forward.
- If the API doesn't answer, check `/tmp/hatari-agent.log`. Another Hatari
  might hold the port (`lsof -nP -iTCP:7777`).
