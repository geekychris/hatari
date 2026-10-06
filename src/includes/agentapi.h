/*
  Hatari - agentapi.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  Agent control API: an HTTP/JSON server that lets external tools
  (e.g. AI agents, test harnesses, MCP bridges) drive the emulator.
  See doc/agent-api.md for the protocol.
*/

#ifndef HATARI_AGENTAPI_H
#define HATARI_AGENTAPI_H

#include <stdint.h>
#include <stdbool.h>

/* option handling (called while parsing the command line) */
extern const char *AgentApi_SetPort(const char *arg);
extern const char *AgentApi_SetBind(const char *arg);
extern const char *AgentApi_SetRomDir(const char *arg);
extern void AgentApi_SetOwnDebugger(bool own);

extern void AgentApi_Init(void);
extern void AgentApi_UnInit(void);
extern bool AgentApi_IsEnabled(void);

/* Process queued requests & tick pending jobs. Called from the main
 * thread at frame-ish rate while running, and on wake-up while paused.
 */
extern void AgentApi_Poll(void);

/* Debugger integration: when this returns true, DebugUI() calls
 * AgentApi_DebugStop() instead of reading commands from the console.
 */
extern bool AgentApi_OwnsDebugger(void);
extern void AgentApi_DebugStop(int reason);

/* Emulated program console output (--conout / NatFeats) capture */
extern void AgentApi_ConsoleWrite(const char *buf, int len);

/* Joystick override for the given port, OR'ed into the ST joystick state */
extern uint8_t AgentApi_JoystickBits(int port);

#endif /* HATARI_AGENTAPI_H */
