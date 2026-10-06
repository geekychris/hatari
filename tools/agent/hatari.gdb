# GDB setup for debugging 68k code running in Hatari (--gdb-port 2159)
#
#   gdb -x tools/agent/hatari.gdb [program.elf]
#
# A multi-target GDB (e.g. Homebrew "gdb", built with --enable-targets=all)
# defaults to the host byte order when no m68k executable is loaded.
set endian big
set architecture m68k
target remote :2159
