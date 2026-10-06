/*
  Hatari - gdbstub.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  GDB remote serial protocol (RSP) stub for the emulated 68k CPU.

  Everything runs on the emulator main thread with non-blocking socket
  I/O: GdbStub_Poll() is called from the SDL event handler while the
  emulation runs (to accept clients and catch Ctrl-C), and from the
  shared debugger stop loop (AgentApi_DebugStop()) while the CPU is
  stopped, where GDB packets are processed.

  Supported: register/memory access, continue, single step, software
  breakpoints (Z0) and write watchpoints (Z2, as value change tracking),
  Ctrl-C, target description (qXfer target.xml), qOffsets for the
  currently running GEMDOS program, "monitor" passthrough to Hatari's
  debugger, no-ack mode.  See doc/agent-gdb.md.
*/
const char GdbStub_fileid[] = "Hatari gdbstub.c";

#include "main.h"
#include "gdbstub.h"

#if HAVE_UNIX_DOMAIN_SOCKETS

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "agentapi.h"
#include "configuration.h"
#include "debugui.h"
#include "debugcpu.h"
#include "debugInfo.h"
#include "breakcond.h"
#include "m68000.h"
#include "stMemory.h"
#include "str.h"

#define GDB_SIGINT	2
#define GDB_SIGTRAP	5
#define GDB_SIGBUS	10

#define MAX_PACKET	4096
#define MAX_BPS		64

static int Port;
static int ListenFd = -1;
static int ClientFd = -1;
static bool NoAck;
static bool WaitingStop;	/* GDB waits for a stop reply ('c', 's', '?') */
static bool Stopped;		/* CPU currently in debugger stop */
static int LastSignal = GDB_SIGTRAP;

/* receive buffer */
static char RxBuf[MAX_PACKET * 2];
static int RxLen;

/* breakpoints & watchpoints we added: expression strings */
static struct {
	char type;		/* '0' = sw break, '2' = write watch */
	uint32_t addr;
	int len;
	int hits;		/* last seen Hatari hit count */
} Bps[MAX_BPS];
static int NumBps;

static const char TargetXml[] =
	"<?xml version=\"1.0\"?>\n"
	"<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
	"<target version=\"1.0\">\n"
	"<architecture>m68k</architecture>\n"
	"<feature name=\"org.gnu.gdb.m68k.core\">\n"
	"<reg name=\"d0\" bitsize=\"32\"/>\n"
	"<reg name=\"d1\" bitsize=\"32\"/>\n"
	"<reg name=\"d2\" bitsize=\"32\"/>\n"
	"<reg name=\"d3\" bitsize=\"32\"/>\n"
	"<reg name=\"d4\" bitsize=\"32\"/>\n"
	"<reg name=\"d5\" bitsize=\"32\"/>\n"
	"<reg name=\"d6\" bitsize=\"32\"/>\n"
	"<reg name=\"d7\" bitsize=\"32\"/>\n"
	"<reg name=\"a0\" bitsize=\"32\" type=\"data_ptr\"/>\n"
	"<reg name=\"a1\" bitsize=\"32\" type=\"data_ptr\"/>\n"
	"<reg name=\"a2\" bitsize=\"32\" type=\"data_ptr\"/>\n"
	"<reg name=\"a3\" bitsize=\"32\" type=\"data_ptr\"/>\n"
	"<reg name=\"a4\" bitsize=\"32\" type=\"data_ptr\"/>\n"
	"<reg name=\"a5\" bitsize=\"32\" type=\"data_ptr\"/>\n"
	"<reg name=\"fp\" bitsize=\"32\" type=\"data_ptr\"/>\n"
	"<reg name=\"sp\" bitsize=\"32\" type=\"data_ptr\"/>\n"
	"<reg name=\"ps\" bitsize=\"32\"/>\n"
	"<reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\"/>\n"
	"</feature>\n"
	"</target>\n";

#define NUM_REGS 18	/* d0-d7, a0-a7, ps, pc */


/* ------------------------------------------------------------------ */
/* option & socket setup */

const char *GdbStub_SetPort(const char *arg)
{
	char *end;
	long port = strtol(arg, &end, 10);
	if (*end || port < 0 || port > 65535)
		return "invalid port number";
	Port = port;
	return NULL;
}

bool GdbStub_IsAttached(void)
{
	return ClientFd >= 0;
}

bool GdbStub_IsEnabled(void)
{
	return ListenFd >= 0;
}

static void set_nonblock(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void GdbStub_Init(void)
{
	struct sockaddr_in addr;
	int one = 1;

	if (!Port || ListenFd >= 0)
		return;
	signal(SIGPIPE, SIG_IGN);
	ListenFd = socket(AF_INET, SOCK_STREAM, 0);
	if (ListenFd < 0)
		return;
	setsockopt(ListenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(Port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(ListenFd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(ListenFd, 1) < 0)
	{
		Log_Printf(LOG_ERROR, "GDB stub: can't listen on port %d\n", Port);
		close(ListenFd);
		ListenFd = -1;
		return;
	}
	set_nonblock(ListenFd);
	fprintf(stderr, "GDB stub: listening on 127.0.0.1:%d (target remote :%d)\n", Port, Port);
}


/* ------------------------------------------------------------------ */
/* packet I/O */

static const char HexChars[] = "0123456789abcdef";

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	c = tolower(c);
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

static bool send_raw(const char *data, int len)
{
	while (len > 0)
	{
		ssize_t n = send(ClientFd, data, len, 0);
		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
				fd_set wfds;
				FD_ZERO(&wfds);
				FD_SET(ClientFd, &wfds);
				select(ClientFd + 1, NULL, &wfds, NULL, NULL);
				continue;
			}
			return false;
		}
		data += n;
		len -= n;
	}
	return true;
}

static void send_packet(const char *payload)
{
	static char buf[MAX_PACKET * 2 + 8];
	int len = strlen(payload);
	uint8_t sum = 0;

	if (ClientFd < 0)
		return;
	if (len > (int)sizeof(buf) - 8)
		len = sizeof(buf) - 8;
	buf[0] = '$';
	memcpy(buf + 1, payload, len);
	for (int i = 0; i < len; i++)
		sum += (uint8_t)payload[i];
	buf[len + 1] = '#';
	buf[len + 2] = HexChars[sum >> 4];
	buf[len + 3] = HexChars[sum & 15];
	send_raw(buf, len + 4);
	/* acks are consumed (and ignored) on receive; on a reliable
	 * TCP stream retransmission requests don't happen in practice
	 */
}

static void hex_encode(char *dst, const char *src, int len)
{
	for (int i = 0; i < len; i++)
	{
		*dst++ = HexChars[(uint8_t)src[i] >> 4];
		*dst++ = HexChars[(uint8_t)src[i] & 15];
	}
	*dst = '\0';
}

static void disconnect(void);


/* ------------------------------------------------------------------ */
/* registers */

static uint32_t *reg_addr(int n)
{
	static const char *names[16] = {
		"D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7",
		"A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7"
	};
	uint32_t *addr;
	if (n < 16 && DebugCpu_GetRegisterAddress(names[n], &addr) == 32)
		return addr;
	return NULL;
}

static uint32_t reg_get(int n)
{
	if (n < 16)
	{
		uint32_t *addr = reg_addr(n);
		return addr ? *addr : 0;
	}
	if (n == 16)
		return M68000_GetSR();
	if (n == 17)
		return M68000_GetPC();
	return 0;
}

static bool reg_set(int n, uint32_t v)
{
	if (n < 16)
	{
		uint32_t *addr = reg_addr(n);
		if (!addr)
			return false;
		*addr = v;
		return true;
	}
	if (n == 16)
	{
		M68000_SetSR(v);
		return true;
	}
	if (n == 17)
	{
		M68000_SetPC(v);
		return true;
	}
	return false;
}

static void put_hex32(char *dst, uint32_t v)
{
	for (int i = 7; i >= 0; i--)
	{
		dst[i] = HexChars[v & 15];
		v >>= 4;
	}
	dst[8] = '\0';
}

static bool get_hex32(const char **p, uint32_t *v)
{
	int n = 0;
	*v = 0;
	while (n < 8 && hexval(**p) >= 0)
	{
		*v = *v << 4 | hexval(**p);
		(*p)++;
		n++;
	}
	return n > 0;
}

static bool parse_hex(const char **p, uint32_t *v)
{
	*v = 0;
	if (hexval(**p) < 0)
		return false;
	while (hexval(**p) >= 0)
	{
		*v = *v << 4 | hexval(**p);
		(*p)++;
	}
	return true;
}


/* ------------------------------------------------------------------ */
/* breakpoints */

static void bp_expression(char *buf, size_t size, char type, uint32_t addr, int len)
{
	if (type == '0')
		snprintf(buf, size, "pc=$%x", addr);
	else
	{
		char mode = len == 1 ? 'b' : len == 2 ? 'w' : 'l';
		/* identical sides with '!' = track value changes */
		snprintf(buf, size, "($%x).%c ! ($%x).%c", addr, mode, addr, mode);
	}
}

static bool bp_add(char type, uint32_t addr, int len)
{
	char expr[96], cmd[128];

	for (int i = 0; i < NumBps; i++)
	{
		if (Bps[i].type == type && Bps[i].addr == addr && Bps[i].len == len)
			return true;	/* GDB may re-insert */
	}
	if (NumBps >= MAX_BPS)
		return false;
	bp_expression(expr, sizeof(expr), type, addr, len);
	snprintf(cmd, sizeof(cmd), "b %s :quiet", expr);
	int before = BreakCond_CpuBreakPointCount();
	DebugUI_RemoteCommand(cmd);
	if (BreakCond_CpuBreakPointCount() <= before)
		return false;
	Bps[NumBps].type = type;
	Bps[NumBps].addr = addr;
	Bps[NumBps].len = len;
	Bps[NumBps].hits = 0;
	NumBps++;
	return true;
}

/* find Hatari breakpoint (0-based index) created for given
 * expression, return -1 if not found.  Sets *hits.
 */
static int bp_find_hatari(char type, uint32_t addr, int len, int *hits_out)
{
	char want[96];
	bp_expression(want, sizeof(want), type, addr, len);

	/* Hatari normalizes expression spacing, compare without blanks */
	char want_ns[96];
	int k = 0;
	for (const char *s = want; *s && k < (int)sizeof(want_ns) - 1; s++)
		if (!isspace((unsigned char)*s))
			want_ns[k++] = tolower((unsigned char)*s);
	want_ns[k] = '\0';

	for (int i = BreakCond_CpuBreakPointCount() - 1; i >= 0; i--)
	{
		const char *expr;
		int hits;
		bool once, trace;
		char ns[256];
		if (!BreakCond_GetCpuBreakPoint(i, &expr, &hits, &once, &trace))
			continue;
		k = 0;
		for (const char *s = expr; *s && k < (int)sizeof(ns) - 1; s++)
			if (!isspace((unsigned char)*s))
				ns[k++] = tolower((unsigned char)*s);
		ns[k] = '\0';
		if (strcmp(ns, want_ns) == 0)
		{
			if (hits_out)
				*hits_out = hits;
			return i;
		}
	}
	return -1;
}

static void bp_remove_hatari(char type, uint32_t addr, int len)
{
	char cmd[32];
	int i = bp_find_hatari(type, addr, len, NULL);
	if (i >= 0)
	{
		snprintf(cmd, sizeof(cmd), "b %d", i + 1);
		DebugUI_RemoteCommand(cmd);
	}
}

/* return watchpoint (index into Bps) whose hit count increased, or -1 */
static int bp_watch_hit(void)
{
	int found = -1;
	for (int i = 0; i < NumBps; i++)
	{
		int hits = 0;
		if (Bps[i].type != '2' || bp_find_hatari('2', Bps[i].addr, Bps[i].len, &hits) < 0)
			continue;
		if (hits != Bps[i].hits && found < 0)
			found = i;
		Bps[i].hits = hits;
	}
	return found;
}

static bool bp_del(char type, uint32_t addr, int len)
{
	for (int i = 0; i < NumBps; i++)
	{
		if (Bps[i].type == type && Bps[i].addr == addr && Bps[i].len == len)
		{
			bp_remove_hatari(type, addr, len);
			Bps[i] = Bps[--NumBps];
			return true;
		}
	}
	return true;	/* not ours / already gone: fine for GDB */
}

static void bp_remove_all(void)
{
	while (NumBps)
		bp_del(Bps[0].type, Bps[0].addr, Bps[0].len);
}


/* ------------------------------------------------------------------ */
/* execution control */

static int WatchHit = -1;	/* Bps index of watchpoint causing last stop */

static void send_stop_reply(void)
{
	char buf[96], watch[32] = "";
	/* T packet with PC (reg 17) & SP (reg 15) speeds up GDB */
	char pc[9], sp[9];
	put_hex32(pc, reg_get(17));
	put_hex32(sp, reg_get(15));
	if (WatchHit >= 0 && WatchHit < NumBps)
		snprintf(watch, sizeof(watch), "watch:%x;", Bps[WatchHit].addr);
	snprintf(buf, sizeof(buf), "T%02x11:%s;0f:%s;%s", LastSignal, pc, sp, watch);
	send_packet(buf);
}

void GdbStub_NotifyStop(int reason)
{
	Stopped = true;
	switch (reason)
	{
	case REASON_USER:
		LastSignal = GDB_SIGINT;
		break;
	case REASON_CPU_EXCEPTION:
		LastSignal = GDB_SIGBUS;
		break;
	default:
		LastSignal = GDB_SIGTRAP;
	}
	WatchHit = reason == REASON_CPU_BREAKPOINT ? bp_watch_hit() : -1;
	if (ClientFd >= 0 && WaitingStop)
	{
		WaitingStop = false;
		send_stop_reply();
	}
}

static void resume(bool step)
{
	if (step)
		DebugCpu_RequestBreak();	/* stop after next instruction */
	WaitingStop = true;
	Stopped = false;
	AgentApi_RequestResume();
}

/* stop a running CPU (Ctrl-C, attach).  Stop reply is sent only if
 * GDB is waiting for one (after 'c'/'s'/'?'), never unsolicited.
 */
static void request_break(void)
{
	if (Stopped)
		return;
	AgentApi_RequestBreak();
}


/* ------------------------------------------------------------------ */
/* packet handlers */

static void handle_monitor(const char *hexcmd)
{
	char cmd[MAX_PACKET / 2 + 1];
	int n = 0;

	while (hexcmd[0] && hexcmd[1] && n < (int)sizeof(cmd) - 1)
	{
		int hi = hexval(hexcmd[0]), lo = hexval(hexcmd[1]);
		if (hi < 0 || lo < 0)
			break;
		cmd[n++] = hi << 4 | lo;
		hexcmd += 2;
	}
	cmd[n] = '\0';

	char *out;
	size_t len;
	int ret = AgentApi_DebugCommand(cmd, &out, &len);
	if (ret == DEBUGGER_END && Stopped)
	{
		/* monitor commands mustn't resume, as GDB doesn't expect it */
		static const char msg[] = "(command would resume emulation, use GDB continue/step)\n";
		free(out);
		out = strdup(msg);
		len = strlen(msg);
		DebugCpu_SetDebugging();
	}
	/* send output as O packets, in chunks */
	for (size_t pos = 0; pos < len; )
	{
		char pkt[MAX_PACKET + 2];
		size_t chunk = len - pos;
		if (chunk > (MAX_PACKET - 2) / 2)
			chunk = (MAX_PACKET - 2) / 2;
		pkt[0] = 'O';
		hex_encode(pkt + 1, out + pos, chunk);
		send_packet(pkt);
		pos += chunk;
	}
	free(out);
	send_packet("OK");
}

static bool starts_with(const char *str, const char *prefix)
{
	return strncmp(str, prefix, strlen(prefix)) == 0;
}

static void handle_query(const char *pkt)
{
	char buf[MAX_PACKET];

	if (starts_with(pkt, "qSupported"))
	{
		snprintf(buf, sizeof(buf), "PacketSize=%x;qXfer:features:read+;QStartNoAckMode+", MAX_PACKET);
		send_packet(buf);
	}
	else if (strcmp(pkt, "QStartNoAckMode") == 0)
	{
		send_packet("OK");
		NoAck = true;
	}
	else if (starts_with(pkt, "qXfer:features:read:target.xml:"))
	{
		const char *p = pkt + strlen("qXfer:features:read:target.xml:");
		uint32_t off, len;
		if (!parse_hex(&p, &off) || *p++ != ',' || !parse_hex(&p, &len))
		{
			send_packet("E01");
			return;
		}
		size_t total = strlen(TargetXml);
		if (off >= total)
		{
			send_packet("l");
			return;
		}
		if (len > sizeof(buf) - 2)
			len = sizeof(buf) - 2;
		size_t n = total - off < len ? total - off : len;
		buf[0] = (off + n >= total) ? 'l' : 'm';
		memcpy(buf + 1, TargetXml + off, n);
		buf[n + 1] = '\0';
		send_packet(buf);
	}
	else if (strcmp(pkt, "qAttached") == 0)
		send_packet("1");
	else if (strcmp(pkt, "qC") == 0)
		send_packet("QC1");
	else if (strcmp(pkt, "qfThreadInfo") == 0)
		send_packet("m1");
	else if (strcmp(pkt, "qsThreadInfo") == 0)
		send_packet("l");
	else if (strcmp(pkt, "qOffsets") == 0)
	{
		/* relocation of an ELF linked at 0, as loaded by GEMDOS */
		uint32_t text = DebugInfo_GetTEXT();
		if (!text)
			send_packet("");
		else
		{
			snprintf(buf, sizeof(buf), "Text=%x;Data=%x;Bss=%x", text, text, text);
			send_packet(buf);
		}
	}
	else if (starts_with(pkt, "qRcmd,"))
		handle_monitor(pkt + 6);
	else
		send_packet("");
}

static void handle_packet(char *pkt)
{
	char buf[MAX_PACKET];
	const char *p = pkt + 1;
	uint32_t addr, len, val;

	switch (pkt[0])
	{
	case '?':
		if (Stopped)
			send_stop_reply();
		else
		{
			WaitingStop = true;
			request_break();
		}
		break;

	case 'g':
		for (int i = 0; i < NUM_REGS; i++)
			put_hex32(buf + i * 8, reg_get(i));
		send_packet(buf);
		break;

	case 'G':
		for (int i = 0; i < NUM_REGS && strlen(p) >= 8; i++)
		{
			get_hex32(&p, &val);
			reg_set(i, val);
		}
		send_packet("OK");
		break;

	case 'p':
		if (!parse_hex(&p, &val) || val >= NUM_REGS)
		{
			/* unknown (e.g. FPU) registers read as 0 */
			send_packet("00000000");
			break;
		}
		put_hex32(buf, reg_get(val));
		send_packet(buf);
		break;

	case 'P':
	{
		uint32_t n;
		if (!parse_hex(&p, &n) || *p++ != '=' || !get_hex32(&p, &val) || !reg_set(n, val))
			send_packet("E01");
		else
			send_packet("OK");
		break;
	}

	case 'm':
		if (!parse_hex(&p, &addr) || *p++ != ',' || !parse_hex(&p, &len))
		{
			send_packet("E01");
			break;
		}
		if (len > (MAX_PACKET - 1) / 2)
			len = (MAX_PACKET - 1) / 2;
		for (uint32_t i = 0; i < len; i++)
		{
			uint8_t b = STMemory_ReadByte(addr + i);
			buf[i * 2] = HexChars[b >> 4];
			buf[i * 2 + 1] = HexChars[b & 15];
		}
		buf[len * 2] = '\0';
		send_packet(buf);
		break;

	case 'M':
		if (!parse_hex(&p, &addr) || *p++ != ',' || !parse_hex(&p, &len) || *p++ != ':')
		{
			send_packet("E01");
			break;
		}
		for (uint32_t i = 0; i < len; i++)
		{
			int hi = hexval(p[0]), lo = hexval(p[1]);
			if (hi < 0 || lo < 0)
				break;
			STMemory_WriteByte(addr + i, hi << 4 | lo);
			p += 2;
		}
		send_packet("OK");
		break;

	case 'c':
	case 's':
		/* optional resume address */
		if (parse_hex(&p, &addr))
			M68000_SetPC(addr);
		if (!Stopped)
		{
			WaitingStop = true;
			break;
		}
		resume(pkt[0] == 's');
		break;

	case 'Z':
	case 'z':
	{
		char type = pkt[1];
		p = pkt + 2;
		if (*p++ != ',' || !parse_hex(&p, &addr) || *p++ != ',' || !parse_hex(&p, &len))
		{
			send_packet("E01");
			break;
		}
		if (type == '1')
			type = '0';	/* hardware breakpoints are the same for us */
		if (type != '0' && type != '2')
		{
			send_packet("");	/* read/access watchpoints unsupported */
			break;
		}
		if (type == '2' && len != 1 && len != 2 && len != 4)
		{
			send_packet("E02");
			break;
		}
		if (type == '0')
			len = 0;
		bool ok = pkt[0] == 'Z' ? bp_add(type, addr, len) : bp_del(type, addr, len);
		send_packet(ok ? "OK" : "E03");
		break;
	}

	case 'H':
		send_packet("OK");
		break;

	case 'T':
		send_packet("OK");	/* thread alive */
		break;

	case 'D':
		send_packet("OK");
		disconnect();
		break;

	case 'k':
		disconnect();
		break;

	case 'q':
	case 'Q':
		handle_query(pkt);
		break;

	case 'v':
		if (starts_with(pkt, "vMustReplyEmpty"))
			send_packet("");
		else if (starts_with(pkt, "vCont?"))
			send_packet("vCont;c;C;s;S");
		else if (starts_with(pkt, "vCont;"))
		{
			char action = pkt[6];
			if (action == 'c' || action == 's' || action == 'C' || action == 'S')
			{
				if (Stopped)
					resume(action == 's' || action == 'S');
				else
					WaitingStop = true;
			}
			else
				send_packet("E01");
		}
		else
			send_packet("");
		break;

	default:
		send_packet("");
	}
}


/* ------------------------------------------------------------------ */
/* connection handling */

static void disconnect(void)
{
	if (ClientFd < 0)
		return;
	bp_remove_all();
	close(ClientFd);
	ClientFd = -1;
	RxLen = 0;
	NoAck = false;
	WaitingStop = false;
	fprintf(stderr, "GDB stub: client disconnected\n");
	/* don't leave the CPU frozen for nobody */
	if (Stopped)
	{
		Stopped = false;
		AgentApi_RequestResume();
	}
}

/* parse complete packets from receive buffer */
static void process_rx(void)
{
	int start = 0;

	while (start < RxLen)
	{
		char c = RxBuf[start];
		if (c == '+' || c == '-')
		{
			start++;
			continue;
		}
		if (c == 0x03)
		{
			start++;
			request_break();
			continue;
		}
		if (c != '$')
		{
			start++;	/* garbage */
			continue;
		}
		/* need '#' and two checksum chars */
		char *hash = memchr(RxBuf + start, '#', RxLen - start);
		if (!hash || hash + 2 >= RxBuf + RxLen)
			break;
		int plen = hash - (RxBuf + start + 1);
		char pkt[MAX_PACKET + 1];
		if (plen > MAX_PACKET)
			plen = MAX_PACKET;
		memcpy(pkt, RxBuf + start + 1, plen);
		pkt[plen] = '\0';

		uint8_t sum = 0, want = hexval(hash[1]) << 4 | hexval(hash[2]);
		for (int i = 0; i < plen; i++)
			sum += (uint8_t)pkt[i];
		start = hash + 3 - RxBuf;

		if (!NoAck)
			send_raw(sum == want ? "+" : "-", 1);
		if (sum != want && !NoAck)
			continue;
		handle_packet(pkt);
		if (ClientFd < 0)
			return;
	}
	memmove(RxBuf, RxBuf + start, RxLen - start);
	RxLen -= start;
}

void GdbStub_Poll(bool stopped)
{
	if (ListenFd < 0)
		return;
	Stopped = stopped;

	if (ClientFd < 0)
	{
		int fd = accept(ListenFd, NULL, NULL);
		if (fd < 0)
			return;
		int one = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
		setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
		set_nonblock(fd);
		ClientFd = fd;
		RxLen = 0;
		NoAck = false;
		WaitingStop = false;
		fprintf(stderr, "GDB stub: client connected\n");
		/* GDB expects a halted target */
		request_break();
	}

	for (;;)
	{
		if (RxLen >= (int)sizeof(RxBuf))
			RxLen = 0;	/* overlong garbage */
		ssize_t n = recv(ClientFd, RxBuf + RxLen, sizeof(RxBuf) - RxLen, 0);
		if (n == 0)
		{
			disconnect();
			return;
		}
		if (n < 0)
		{
			if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
				disconnect();
			break;
		}
		RxLen += n;
		process_rx();
		if (ClientFd < 0)
			return;
	}
}

void GdbStub_UnInit(void)
{
	disconnect();
	if (ListenFd >= 0)
	{
		close(ListenFd);
		ListenFd = -1;
	}
}

#else	/* !HAVE_UNIX_DOMAIN_SOCKETS */

const char *GdbStub_SetPort(const char *arg) { return "GDB stub not supported on this platform"; }
void GdbStub_Init(void) { }
void GdbStub_UnInit(void) { }
bool GdbStub_IsAttached(void) { return false; }
bool GdbStub_IsEnabled(void) { return false; }
void GdbStub_Poll(bool stopped) { }
void GdbStub_NotifyStop(int reason) { }

#endif
