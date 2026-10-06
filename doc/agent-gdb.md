# Debugging 68k code in Hatari with GDB

`--gdb-port <port>` starts a GDB remote serial protocol stub, so any GDB
that knows m68k can debug the emulated CPU: `m68k-elf-gdb`,
`m68k-atari-mintelf-gdb`, `gdb-multiarch`, or Homebrew's `gdb`, which is
built with all targets. IDEs that speak GDB remote (VS Code with cortex-debug
or Native Debug, CLion, ...) work the same way.

```sh
hatari --gdb-port 2159 --tos roms/emutos/emutos-512k-1.4/etos512us.img
gdb -x tools/agent/hatari.gdb            # set endian big + target remote :2159
```

The stub listens on 127.0.0.1 only. It can run together with the HTTP
agent API (`--agent-port`), and both see the same stops.

## What works

| GDB feature | Notes |
|---|---|
| Attach | Connecting stops the CPU at the next instruction. |
| Registers | `d0-d7 a0-a5 fp(a6) sp(a7) ps pc` (read/write). `ps` is SR. FPU registers aren't exposed. |
| Memory | `x`, `print *ptr`, `set var`... ST RAM, ROM and IO (IO reads go through emulated hardware). |
| `stepi`, `nexti`, `continue` | Instruction stepping. Source-level `step`/`next` work when GDB has symbols and line info. |
| Ctrl-C / `interrupt` | Stops the running CPU (SIGINT). |
| `break *addr`, `break func` | Implemented as Hatari address breakpoints. `hbreak` behaves the same. |
| `watch expr` (1, 2 or 4 bytes) | Write watchpoints, implemented as Hatari "value changed" breakpoints. Writes that store the same value are not seen. GDB shows old/new values. `rwatch`/`awatch` are not supported. |
| `monitor <cmd>` | Runs any Hatari debugger command and shows its output: `monitor info osheader`, `monitor info basepage`, `monitor m $ff8240-$ff8260`, `monitor symbols prg`, `monitor history on`, `monitor profile on`, `monitor b` (list all breakpoints). Commands that would resume the CPU are refused. Use GDB's own continue/step. |
| `qOffsets` | Reports the current GEMDOS program's TEXT address as the relocation offset (see Symbols). |
| `detach` / disconnect | Removes GDB's breakpoints and resumes emulation. |

Stop signals: `SIGTRAP` for breakpoints, watchpoints and steps, `SIGINT` for
Ctrl-C / `/debug/break`, `SIGBUS` for CPU exceptions caught with
`--debug-except`.

## Symbols for your program

Atari programs (PRG/TOS/APP) are relocated by GEMDOS when they load. Build an
ELF next to your PRG (e.g. `m68k-atari-mintelf-gcc` produces ELF that is
converted to PRG, or use `vlink -b elf32m68k` / `vasm -Felf`), and tell GDB
where the TEXT segment ended up:

```gdb
# after the program has started (e.g. stopped at its first instruction):
monitor info basepage            # shows TEXT start
add-symbol-file prog.elf -o 0x<TEXT>
```

To stop at the very first instruction of a program, set a Hatari
breakpoint on its entry point *before* starting it. Through GDB:

```gdb
monitor b pc=TEXT :once
continue
```

(`TEXT` is a Hatari debugger variable: the TEXT address of the program
currently being started.) If GDB connects after the program started,
`qOffsets` already relocates a `file prog.elf` given on the command line.

Programs built for address 0 with contiguous TEXT/DATA/BSS (the usual case)
need one offset. Toolchains that give DATA/BSS separate addresses need
`add-symbol-file prog.elf -s .data 0x<DATA> -s .bss 0x<BSS> ...`.

## Mixing with the agent API

When GDB is connected it owns debugger stops. The HTTP API keeps working
during a stop (screenshots, memory, `/status` shows `stopped`). If an HTTP
client resumes the CPU (`/debug/continue`), GDB isn't told and still waits
for a stop. It gets its stop reply at the next stop, so prefer one
controller at a time.

## Implementation notes

`src/gdbstub.c` runs entirely on the emulator main thread with non-blocking
sockets. It's polled from the SDL event handler while the emulation runs
(accepting clients, catching Ctrl-C), and from the shared debugger stop loop
(`AgentApi_DebugStop()`) while the CPU is stopped. Breakpoints and steps
use the existing Hatari debugger (conditional breakpoints, single-step
counter), so they cost nothing while none are set.

Supported packets: `? g G p P m M c s vCont Z0/z0 Z1/z1 Z2/z2 D k H T
qSupported qXfer:features:read qAttached qC qfThreadInfo qsThreadInfo
qOffsets qRcmd QStartNoAckMode vMustReplyEmpty`.
