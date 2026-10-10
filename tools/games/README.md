# Game launcher

A menu for the Atari ports of the Amiga games, which live in their own repo, [geekychris/atari_st_games](https://github.com/geekychris/atari_st_games). Pick a game and it starts in Hatari on the right machine. For a menu that runs on the Atari itself, with all the games on one C: drive, see that repo's `st_launcher`, which reads the same `games.ini`.

Check the games out next to this repo (or set `ATARI_GAMES_DIR`, or pass `--ini`):

```sh
git clone https://github.com/geekychris/atari_st_games ../atari_st_games
```

```sh
python3 tools/games/launcher.py              # window: list, screenshot, controls, Play
python3 tools/games/launcher.py --list       # the game ids
python3 tools/games/launcher.py --play lunar_rider [--no-build] [--no-sound]
```

The games are defined in that repo's [`games.ini`](https://github.com/geekychris/atari_st_games/blob/main/games.ini), one section per game:

```ini
[void_trader]
title = Void Trader
dir = void_trader                 ; relative to the INI file
program = VTRADER.PRG             ; in <dir>/build, autostarted as C:\VTRADER.PRG
machine = falcon                  ; st, ste, megaste, tt, falcon
options = --dsp none --memsize 14 --monitor vga
screenshot = docs/combat.png
description = Elite-style space combat and trading in filled 3D.
controls = W/S pitch, A/D yaw, Q/E roll, R/F thrust
    Space: fire / start, Tab: dock, U: undock
```

`tos` is optional. The default is EmuTOS 192k for `st` and EmuTOS 1024k for everything else; on an STE, the 192k image makes Hatari fall back to a plain ST. To add a game, add a section. `--ini FILE` uses another file.

The `[launcher]` section holds these settings:
- `cross`: the cross-compiler prefix. If empty, `$CROSS` is used, then `m68k-atari-mintelf-` on the PATH.
- `build`: `yes` runs `make` in the game's directory before starting it. This is quick when the game is up to date.
- `sound`: Hatari's host audio, `on` or `off`.
- `port`: the agent API port.

Playing a game:
1. Builds the game, if `build` is on.
2. Quits any Hatari already answering on the agent port.
3. Starts Hatari through `tools/agent/hatari-agent-run.sh`, with the game's `build/` directory as drive C: and NatFeats on.

The agent API stays available, so you can screenshot, profile, read the `/console` log or poke `/mem` while you play. In the window, **Agent console** opens `tools/agent/hatari_gui.py` on the same port.

Only needs the Python standard library (Tk 8.6, for PNG screenshots).
