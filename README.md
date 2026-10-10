# Hatari: agentic development fork

This is a fork of [Hatari](https://www.hatari-emu.org/), the Atari
ST/STE/TT/Falcon emulator. It adds an **HTTP/JSON control API** so that AI
agents and automated tools can drive the emulator end to end: boot any TOS,
type and click, read the screen at native resolution, capture program text
output, save and restore snapshots, and stop, step and inspect the 68k CPU.
It also includes a **GDB remote stub**, so any m68k GDB or IDE can debug
code running in the emulator.

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
| GDB | `--gdb-port 2159`: registers, memory, stepi, breakpoints, watchpoints, Ctrl-C, `monitor` = Hatari debugger |

* API reference: [doc/agent-api.md](doc/agent-api.md)
* GDB: [doc/agent-gdb.md](doc/agent-gdb.md) (`gdb -x tools/agent/hatari.gdb`)
* Design and changes to Hatari: [doc/agent-design.md](doc/agent-design.md)
* Claude Code skill: [.claude/skills/hatari-agent/SKILL.md](.claude/skills/hatari-agent/SKILL.md)
* Python client and GUI: `tools/agent/hatari_agent.py`, `python3 tools/agent/hatari_gui.py`

## Examples

* [examples/gemdemo](examples/gemdemo): a GEM VDI showcase written in C, cross-compiled with m68k-atari-mintelf GCC
* [examples/interact](examples/interact): a GEM test target app, shell tests for every API feature,
  Python scenarios and the GUI

## Game ports

Twenty Atari ST, STE and Falcon030 ports of the games in
[geekychris/amiga_games](https://github.com/geekychris/amiga_games), made and tuned with the
agent API (profiler, breakpoints, `/mem`, scripted play), live in their own repo:
**[geekychris/atari_st_games](https://github.com/geekychris/atari_st_games)** (MIT), with
screenshots of every game. Check it out next to this one, and `make run` in a game's directory,
or the game menu here, starts it in this Hatari:

```sh
git clone https://github.com/geekychris/atari_st_games ../atari_st_games
python3 tools/games/launcher.py          # pick a game (reads ../atari_st_games/games.ini)
```

<table><tr>
<td align="center"><img src="https://raw.githubusercontent.com/geekychris/atari_st_games/main/uranus_lander/docs/flight.png" width="200" alt="Uranus Lander (ST)"><br>Uranus Lander (ST)</td>
<td align="center"><img src="https://raw.githubusercontent.com/geekychris/atari_st_games/main/rock_blaster/docs/play.png" width="200" alt="Rock Blaster (STE)"><br>Rock Blaster (STE)</td>
<td align="center"><img src="https://raw.githubusercontent.com/geekychris/atari_st_games/main/lunar_rider/docs/ride.png" width="200" alt="Lunar Rider (STE)"><br>Lunar Rider (STE)</td>
<td align="center"><img src="https://raw.githubusercontent.com/geekychris/atari_st_games/main/fractalus/docs/flight.png" width="200" alt="Fractalus (Falcon030)"><br>Fractalus (Falcon030)</td>
</tr><tr>
<td align="center"><img src="https://raw.githubusercontent.com/geekychris/atari_st_games/main/void_trader/docs/combat.png" width="200" alt="Void Trader (Falcon030)"><br>Void Trader (Falcon030)</td>
<td align="center"><img src="https://raw.githubusercontent.com/geekychris/atari_st_games/main/spectral_keep/docs/play.png" width="200" alt="Spectral Keep (Falcon030)"><br>Spectral Keep (Falcon030)</td>
<td align="center"><img src="https://raw.githubusercontent.com/geekychris/atari_st_games/main/rolling_steel/docs/play.png" width="200" alt="Rolling Steel (Falcon, 68060)"><br>Rolling Steel (Falcon, 68060)</td>
<td align="center"><img src="https://raw.githubusercontent.com/geekychris/atari_st_games/main/planet_chomp/docs/play.png" width="200" alt="Planet Chomp (Falcon, 68060)"><br>Planet Chomp (Falcon, 68060)</td>
</tr></table>

## ROMs

Only the free [EmuTOS](https://emutos.sourceforge.io/) images are fetched.
Original Atari TOS images are copyrighted. If you own them, copy them into
`roms/` (git-ignored) and `GET /roms` lists them alongside EmuTOS.

## License

GPL v2 or later, like Hatari. See [gpl.txt](gpl.txt).
