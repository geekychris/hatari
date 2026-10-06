# Hatari: agentic development fork

This is a fork of [Hatari](https://www.hatari-emu.org/), the Atari
ST/STE/TT/Falcon emulator. It adds an **HTTP/JSON control API** so that AI
agents and automated tools can drive the emulator end to end: boot any TOS,
type and click, read the screen at native resolution, capture program text
output, save and restore snapshots, and stop, step and inspect the 68k CPU.

For the original Hatari documentation see [readme.txt](readme.txt) and
[doc/](doc/).

## Quick start (macOS / Linux)

```sh
# build
mkdir -p build && cd build
cmake .. -DCMAKE_OSX_ARCHITECTURES=arm64   # macOS + Homebrew; omit on Linux
cmake --build . -j$(getconf _NPROCESSORS_ONLN)
cd ..

# free EmuTOS ROMs (58 images: ST/STE/TT/Falcon, 19 languages) into roms/
tools/agent/fetch-emutos.sh

# start Hatari with the agent API on http://127.0.0.1:7777/
tools/agent/hatari-agent-run.sh --machine ste --conout 2

curl -s localhost:7777/status
curl -s -o screen.png localhost:7777/screen
curl -s -XPOST 'localhost:7777/input/click?x=82&y=4'
curl -s -XPOST localhost:7777/input/type --data-binary $'hello\n'
curl -s -XPOST localhost:7777/debug/break
```

## What you can do

| Area | Endpoints |
|---|---|
| Emulation | pause / resume / run exactly N frames / reset / fast forward / quit / any Hatari option |
| ROMs and media | list and boot TOS images (EmuTOS or your own), floppy images, host folder as drive C:, autostart a program |
| Screen | PNG at native emulated resolution (coordinates = mouse coordinates) |
| Input | type text, key combos, absolute or relative mouse, clicks, joystick |
| Memory and CPU | read/write memory, registers, disassembly |
| Debugger | break, step, step over, breakpoints (address or conditional), wait for stop, any Hatari debugger command with captured output |
| State | save/restore full snapshots, read program console output as text |

* API reference: [doc/agent-api.md](doc/agent-api.md)
* Design and changes to Hatari: [doc/agent-design.md](doc/agent-design.md)
* Claude Code skill: [.claude/skills/hatari-agent/SKILL.md](.claude/skills/hatari-agent/SKILL.md)

## ROMs

Only the free [EmuTOS](https://emutos.sourceforge.io/) images are fetched.
Original Atari TOS images are copyrighted. If you own them, copy them into
`roms/` (git-ignored) and `GET /roms` lists them alongside EmuTOS.

## License

GPL v2 or later, like Hatari. See [gpl.txt](gpl.txt).
