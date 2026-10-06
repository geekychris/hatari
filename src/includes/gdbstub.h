/*
  Hatari - gdbstub.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  GDB remote serial protocol stub for debugging emulated 68k code
  with m68k GDB (target remote :port).  See doc/agent-gdb.md.
*/

#ifndef HATARI_GDBSTUB_H
#define HATARI_GDBSTUB_H

#include <stdbool.h>

extern const char *GdbStub_SetPort(const char *arg);
extern void GdbStub_Init(void);
extern void GdbStub_UnInit(void);

/* true while a GDB client is connected (it then owns debugger stops) */
extern bool GdbStub_IsAttached(void);
extern bool GdbStub_IsEnabled(void);

/* main thread: accept/read client.  'stopped' = CPU is in debugger stop */
extern void GdbStub_Poll(bool stopped);

/* CPU entered debugger stop: send stop reply to GDB if it waits for one */
extern void GdbStub_NotifyStop(int reason);

#endif /* HATARI_GDBSTUB_H */
