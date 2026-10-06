/*
  Hatari - agenthttp.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  Minimal HTTP/1.1 server used by the agent API (agentapi.c).

  Threading model: one listener thread plus one thread per connection.
  Connection threads parse the request, queue it and block until the
  emulator main thread completes it with AgentHttp_Complete().  All
  emulator state is therefore only ever touched from the main thread.
*/

#ifndef HATARI_AGENTHTTP_H
#define HATARI_AGENTHTTP_H

#include <stddef.h>
#include <stdbool.h>

#define AGENT_MAX_PARAMS 64

typedef struct agent_req {
	char method[8];
	char path[256];
	int nparams;
	char *keys[AGENT_MAX_PARAMS];
	char *vals[AGENT_MAX_PARAMS];
	char *body;		/* raw request body, NUL terminated */
	size_t body_len;

	/* response, filled by AgentHttp_Complete() */
	int status;
	const char *ctype;
	char *resp;
	size_t resp_len;
	bool done;

	struct agent_req *next;
} agent_req_t;

/* growable string buffer */
typedef struct {
	char *data;
	size_t len, size;
} sbuf_t;

extern void Sbuf_Init(sbuf_t *sb);
extern void Sbuf_Free(sbuf_t *sb);
extern void Sbuf_Add(sbuf_t *sb, const char *data, size_t len);
extern void Sbuf_Printf(sbuf_t *sb, const char *fmt, ...)
	__attribute__ ((format (printf, 2, 3)));
extern void Sbuf_JsonStr(sbuf_t *sb, const char *str, size_t len);

/* wake-up callback is called from the network threads whenever
 * a new request has been queued, to wake up the main thread
 */
extern const char *AgentHttp_Start(const char *bind, int port, void (*wakeup)(void));
extern void AgentHttp_Stop(void);

/* main thread: dequeue all newly arrived requests (linked by ->next) */
extern agent_req_t *AgentHttp_Take(void);
/* main thread: block until a request arrives or timeout_ms passes */
extern void AgentHttp_WaitIncoming(int timeout_ms);
/* main thread: finish request. Takes ownership of malloc()ed 'data'. */
extern void AgentHttp_Complete(agent_req_t *req, int status, const char *ctype,
                               char *data, size_t len);

extern const char *AgentHttp_Param(const agent_req_t *req, const char *key);

#endif /* HATARI_AGENTHTTP_H */
