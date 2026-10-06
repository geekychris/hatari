/*
  Hatari - agentapi.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  Agent control API.  An HTTP/JSON interface for driving the emulator
  from external tools: emulation control, input injection, screenshots,
  ROM selection, memory/register access and debugger control.

  Requests arrive on network threads (agenthttp.c) and are executed here
  on the emulator main thread, from three places:
  - AgentApi_Poll() called from the SDL event handler while emulation runs
    (several times per emulated frame) and while it is paused,
  - AgentApi_DebugStop() loop while the CPU is stopped in the debugger,
  so handlers can touch emulator state without locking.

  Requests that need emulated time to pass (typing, clicks, running N
  frames, waiting for a breakpoint) become "jobs" that are completed
  later from AgentApi_Poll().

  The protocol is documented in doc/agent-api.md.
*/
const char AgentApi_fileid[] = "Hatari agentapi.c";

#include "main.h"
#include "agentapi.h"

#if HAVE_UNIX_DOMAIN_SOCKETS

#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include <fcntl.h>

#include "agenthttp.h"
#include "change.h"
#include "configuration.h"
#include "conv_st.h"
#include "debugui.h"
#include "debugcpu.h"
#include "breakcond.h"
#include "evaluate.h"
#include "file.h"
#include "gui_event.h"
#include "ikbd.h"
#include "joy.h"
#include "m68000.h"
#include "68kDisass.h"
#include "memorySnapShot.h"
#include "reset.h"
#include "screenSnapShot.h"
#include "statusbar.h"
#include "stMemory.h"
#include "str.h"
#include "tos.h"
#include "video.h"

#define JSON "application/json"

/* configuration (from command line) */
static int Port;
static char Bind[64] = "127.0.0.1";
static char RomDir[FILENAME_MAX];
static bool OwnDebugger = true;
static bool Running;

/* emulated frame counter that survives resets (nVBLs doesn't) */
static uint64_t Frames;
static int LastVBL;

/* debugger stop state */
static bool InDebugStop;
static bool ResumeRequested;
static bool BreakRequested;
static int StopReason = REASON_NONE;
static uint32_t StopCount;
static uint32_t StopPC;

/* joystick override bits per ST joystick port */
static uint8_t JoyBits[2];

/* console output ring (append-only offsets, oldest data dropped) */
#define CONSOLE_SIZE (256*1024)
static char ConsoleBuf[CONSOLE_SIZE];
static uint64_t ConsoleEnd;	/* total bytes ever written */

/* ------------------------------------------------------------------ */
/* jobs */

typedef enum {
	ACT_KEY,	/* a = scancode, b = press */
	ACT_MBUTTON,	/* a = 0 left / 1 right, b = press */
	ACT_MMOVE,	/* a = dx, b = dy */
	ACT_DBLCLICK,
	ACT_JOY,	/* a = port, b = bits */
	ACT_MOUSE_TO,	/* a = x, b = y: steer GEM pointer there (repeats) */
} act_kind_t;

typedef struct {
	act_kind_t kind;
	int a, b;
	int delay;	/* frames to wait after this action */
	int tries;	/* for repeating actions */
} action_t;

typedef enum {
	JOB_INPUT,	/* run actions, then reply */
	JOB_FRAMES,	/* wait until frame, optionally pause, then reply */
	JOB_STOP,	/* wait for (next) debugger stop */
} job_kind_t;

typedef struct job {
	job_kind_t kind;
	agent_req_t *req;
	action_t *acts;
	int nacts, pos;
	uint64_t next_frame;
	bool pause_after;
	uint32_t stop_count;
	uint64_t deadline;	/* wall clock ms, 0 = none */
	struct job *next;
} job_t;

static job_t *Jobs;

static uint64_t now_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void job_add(job_t *job)
{
	job->next = Jobs;
	Jobs = job;
}


/* ------------------------------------------------------------------ */
/* option handling */

const char *AgentApi_SetPort(const char *arg)
{
	char *end;
	long port = strtol(arg, &end, 10);
	if (*end || port < 0 || port > 65535)
		return "invalid port number";
	Port = port;
	return NULL;
}

const char *AgentApi_SetBind(const char *arg)
{
	if (strlen(arg) >= sizeof(Bind))
		return "address too long";
	Str_Copy(Bind, arg, sizeof(Bind));
	return NULL;
}

const char *AgentApi_SetRomDir(const char *arg)
{
	if (!File_DirExists(arg))
		return "given ROM directory doesn't exist";
	Str_Copy(RomDir, arg, sizeof(RomDir));
	return NULL;
}

void AgentApi_SetOwnDebugger(bool own)
{
	OwnDebugger = own;
}

bool AgentApi_IsEnabled(void)
{
	return Running;
}

bool AgentApi_OwnsDebugger(void)
{
	return Running && OwnDebugger;
}


/* ------------------------------------------------------------------ */
/* response helpers */

static void reply_json(agent_req_t *req, int status, sbuf_t *sb)
{
	Sbuf_Add(sb, "\n", 1);
	AgentHttp_Complete(req, status, JSON, sb->data, sb->len);
}

static void reply_error(agent_req_t *req, int status, const char *fmt, ...)
	__attribute__ ((format (printf, 3, 4)));
static void reply_error(agent_req_t *req, int status, const char *fmt, ...)
{
	char msg[512];
	va_list ap;
	sbuf_t sb;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	Sbuf_Init(&sb);
	Sbuf_Add(&sb, "{\"ok\":false,\"error\":", 20);
	Sbuf_JsonStr(&sb, msg, strlen(msg));
	Sbuf_Add(&sb, "}", 1);
	reply_json(req, status, &sb);
}

static const char *state_name(void)
{
	if (InDebugStop && !ResumeRequested)
		return "stopped";
	if (InDebugStop)
		return "running";	/* leaving the debugger */
	return bEmulationActive ? "running" : "paused";
}

static const char *reason_name(int reason)
{
	switch (reason)
	{
	case REASON_CPU_EXCEPTION: return "cpu_exception";
	case REASON_DSP_EXCEPTION: return "dsp_exception";
	case REASON_CPU_BREAKPOINT: return "breakpoint";
	case REASON_DSP_BREAKPOINT: return "dsp_breakpoint";
	case REASON_CPU_STEPS: return "step";
	case REASON_DSP_STEPS: return "dsp_step";
	case REASON_PROGRAM: return "program";
	case REASON_USER: return "user";
	}
	return "none";
}

static const char *machine_name(int type)
{
	switch (type)
	{
	case MACHINE_ST: return "st";
	case MACHINE_MEGA_ST: return "megast";
	case MACHINE_STE: return "ste";
	case MACHINE_MEGA_STE: return "megaste";
	case MACHINE_TT: return "tt";
	case MACHINE_FALCON: return "falcon";
	}
	return "unknown";
}

/* disassemble 'count' instructions at 'addr' into sb as a JSON array */
static void add_disasm(sbuf_t *sb, uint32_t addr, int count)
{
	Sbuf_Add(sb, "[", 1);
	for (int i = 0; i < count; i++)
	{
		char *text = NULL;
		size_t len = 0;
		uaecptr next = addr;
		FILE *fp = open_memstream(&text, &len);
		if (!fp)
			break;
		Disasm(fp, addr, &next, 1);
		fclose(fp);
		while (len && (text[len-1] == '\n' || text[len-1] == ' '))
			len--;
		Sbuf_Printf(sb, "%s{\"addr\":\"0x%06x\",\"text\":", i ? "," : "", addr);
		Sbuf_JsonStr(sb, text, len);
		Sbuf_Add(sb, "}", 1);
		free(text);
		if (next == addr)
			break;
		addr = next;
	}
	Sbuf_Add(sb, "]", 1);
}

static void add_regs(sbuf_t *sb)
{
	static const char *names[] = {
		"D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7",
		"A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7",
		"USP", "ISP", "MSP", "VBR", "CACR", "CAAR", "SFC", "DFC"
	};
	Sbuf_Printf(sb, "{\"pc\":\"0x%06x\",\"sr\":\"0x%04x\"",
	            M68000_GetPC(), M68000_GetSR());
	for (unsigned i = 0; i < ARRAY_SIZE(names); i++)
	{
		uint32_t *addr;
		if (DebugCpu_GetRegisterAddress(names[i], &addr) == 32)
		{
			char lower[8];
			int j;
			for (j = 0; names[i][j]; j++)
				lower[j] = tolower((unsigned char)names[i][j]);
			lower[j] = '\0';
			Sbuf_Printf(sb, ",\"%s\":\"0x%08x\"", lower, *addr);
		}
	}
	Sbuf_Add(sb, "}", 1);
}

static void add_stop_info(sbuf_t *sb)
{
	Sbuf_Printf(sb, "\"stop\":{\"reason\":\"%s\",\"count\":%u,\"pc\":\"0x%06x\"}",
	            reason_name(StopReason), StopCount, StopPC);
}

/* common status fields, without surrounding braces */
static void add_status(sbuf_t *sb)
{
	Sbuf_Printf(sb, "\"state\":\"%s\",\"frame\":%llu,\"pc\":\"0x%06x\",",
	            state_name(), (unsigned long long)Frames, M68000_GetPC());
	add_stop_info(sb);
}

static void reply_ok_status(agent_req_t *req)
{
	sbuf_t sb;
	Sbuf_Init(&sb);
	Sbuf_Add(&sb, "{\"ok\":true,", 11);
	add_status(&sb);
	Sbuf_Add(&sb, "}", 1);
	reply_json(req, 200, &sb);
}


/* ------------------------------------------------------------------ */
/* parameter helpers */

/* parse number/expression (debugger syntax: $hex, 0xhex, #dec,
 * register & symbol names, arithmetic).  Returns false on error.
 */
static bool param_value(agent_req_t *req, const char *key, uint32_t *value, bool required)
{
	const char *str = AgentHttp_Param(req, key);
	const char *err;
	int offset;

	if (!str || !*str)
	{
		if (required)
			reply_error(req, 400, "missing parameter '%s'", key);
		return !required;
	}
	/* plain decimal / 0x numbers without debugger expression parsing,
	 * as debugger defaults to hex for bare numbers
	 */
	char *end;
	unsigned long v = strtoul(str, &end, 0);
	if (!*end && end != str)
	{
		*value = v;
		return true;
	}
	err = Eval_Expression(str, value, &offset, false);
	if (err)
	{
		reply_error(req, 400, "bad value for '%s' (%s): %s", key, str, err);
		return false;
	}
	return true;
}

static int param_int(agent_req_t *req, const char *key, int def)
{
	const char *str = AgentHttp_Param(req, key);
	if (!str || !*str)
		return def;
	return (int)strtol(str, NULL, 0);
}

static bool param_bool(agent_req_t *req, const char *key, bool def)
{
	const char *str = AgentHttp_Param(req, key);
	if (!str || !*str)
		return def;
	return !(strcmp(str, "0") == 0 || strcasecmp(str, "false") == 0 ||
	         strcasecmp(str, "no") == 0 || strcasecmp(str, "off") == 0);
}

/* text from 'key' param or, if missing, the raw (non-form) body */
static const char *param_text(agent_req_t *req, const char *key)
{
	const char *str = AgentHttp_Param(req, key);
	if (str)
		return str;
	if (req->body_len && req->nparams == 0)
		return req->body;
	/* body with query params */
	if (req->body_len && !strchr(req->body, '='))
		return req->body;
	return NULL;
}

/* jobs needing emulated time can't progress when not running */
static bool need_running(agent_req_t *req)
{
	if (InDebugStop)
	{
		reply_error(req, 409, "CPU is stopped in debugger; use /debug/continue first");
		return false;
	}
	if (!bEmulationActive)
	{
		reply_error(req, 409, "emulation is paused; use /emu/resume (or /emu/run) first");
		return false;
	}
	return true;
}


/* ------------------------------------------------------------------ */
/* stdout/stderr capture for debugger commands */

static FILE *CapFile;
static int CapOut = -1, CapErr = -1;

static void capture_begin(void)
{
	fflush(stdout);
	fflush(stderr);
	CapFile = tmpfile();
	if (!CapFile)
		return;
	CapOut = dup(STDOUT_FILENO);
	CapErr = dup(STDERR_FILENO);
	dup2(fileno(CapFile), STDOUT_FILENO);
	dup2(fileno(CapFile), STDERR_FILENO);
}

/* returns malloc()ed captured text, sets *len */
static char *capture_end(size_t *len)
{
	char *text;

	*len = 0;
	if (!CapFile)
		return strdup("");
	fflush(stdout);
	fflush(stderr);
	dup2(CapOut, STDOUT_FILENO);
	dup2(CapErr, STDERR_FILENO);
	close(CapOut);
	close(CapErr);
	CapOut = CapErr = -1;

	long size = ftell(CapFile);
	if (size < 0)
		size = 0;
	text = malloc(size + 1);
	rewind(CapFile);
	*len = fread(text, 1, size, CapFile);
	text[*len] = '\0';
	fclose(CapFile);
	CapFile = NULL;
	return text;
}


/* ------------------------------------------------------------------ */
/* keyboard: US layout ASCII -> ST scancode */

#define SC_LSHIFT 0x2a
#define SC_CTRL   0x1d
#define SC_ALT    0x38

static const struct { char c; uint8_t sc; bool shift; } AsciiMap[] = {
	{'1',0x02,0},{'2',0x03,0},{'3',0x04,0},{'4',0x05,0},{'5',0x06,0},
	{'6',0x07,0},{'7',0x08,0},{'8',0x09,0},{'9',0x0a,0},{'0',0x0b,0},
	{'!',0x02,1},{'@',0x03,1},{'#',0x04,1},{'$',0x05,1},{'%',0x06,1},
	{'^',0x07,1},{'&',0x08,1},{'*',0x09,1},{'(',0x0a,1},{')',0x0b,1},
	{'-',0x0c,0},{'_',0x0c,1},{'=',0x0d,0},{'+',0x0d,1},
	{'q',0x10,0},{'w',0x11,0},{'e',0x12,0},{'r',0x13,0},{'t',0x14,0},
	{'y',0x15,0},{'u',0x16,0},{'i',0x17,0},{'o',0x18,0},{'p',0x19,0},
	{'[',0x1a,0},{'{',0x1a,1},{']',0x1b,0},{'}',0x1b,1},
	{'a',0x1e,0},{'s',0x1f,0},{'d',0x20,0},{'f',0x21,0},{'g',0x22,0},
	{'h',0x23,0},{'j',0x24,0},{'k',0x25,0},{'l',0x26,0},
	{';',0x27,0},{':',0x27,1},{'\'',0x28,0},{'"',0x28,1},
	{'`',0x29,0},{'~',0x29,1},{'\\',0x2b,0},{'|',0x2b,1},
	{'z',0x2c,0},{'x',0x2d,0},{'c',0x2e,0},{'v',0x2f,0},{'b',0x30,0},
	{'n',0x31,0},{'m',0x32,0},
	{',',0x33,0},{'<',0x33,1},{'.',0x34,0},{'>',0x34,1},{'/',0x35,0},{'?',0x35,1},
	{' ',0x39,0},{'\n',0x1c,0},{'\r',0x1c,0},{'\t',0x0f,0},{'\b',0x0e,0},
	{0x1b,0x01,0},
};

static bool ascii_to_scancode(char c, uint8_t *sc, bool *shift)
{
	char lc = tolower((unsigned char)c);
	for (unsigned i = 0; i < ARRAY_SIZE(AsciiMap); i++)
	{
		if (AsciiMap[i].c == lc)
		{
			*sc = AsciiMap[i].sc;
			*shift = AsciiMap[i].shift || (isalpha((unsigned char)c) && isupper((unsigned char)c));
			return true;
		}
	}
	return false;
}

static const struct { const char *name; uint8_t sc; } KeyNames[] = {
	{"esc",0x01},{"escape",0x01},{"backspace",0x0e},{"tab",0x0f},
	{"return",0x1c},{"enter",0x1c},{"ctrl",0x1d},{"control",0x1d},
	{"lshift",0x2a},{"shift",0x2a},{"rshift",0x36},{"alt",0x38},
	{"space",0x39},{"capslock",0x3a},
	{"f1",0x3b},{"f2",0x3c},{"f3",0x3d},{"f4",0x3e},{"f5",0x3f},
	{"f6",0x40},{"f7",0x41},{"f8",0x42},{"f9",0x43},{"f10",0x44},
	{"home",0x47},{"clrhome",0x47},{"up",0x48},{"left",0x4b},
	{"right",0x4d},{"down",0x50},{"insert",0x52},{"delete",0x53},
	{"undo",0x61},{"help",0x62},
	{"kp(",0x63},{"kp)",0x64},{"kp/",0x65},{"kp*",0x66},
	{"kp7",0x67},{"kp8",0x68},{"kp9",0x69},{"kp-",0x4a},
	{"kp4",0x6a},{"kp5",0x6b},{"kp6",0x6c},{"kp+",0x4e},
	{"kp1",0x6d},{"kp2",0x6e},{"kp3",0x6f},{"kp0",0x70},
	{"kp.",0x71},{"kpenter",0x72},
};

/* key name, single character, or numeric scancode */
static bool keyname_to_scancode(const char *name, uint8_t *sc, bool *shift)
{
	*shift = false;
	if (!name[0])
		return false;
	if (!name[1])
		return ascii_to_scancode(name[0], sc, shift);
	if (strcasecmp(name, "plus") == 0)
		return ascii_to_scancode('+', sc, shift);
	if (strcasecmp(name, "minus") == 0)
		return ascii_to_scancode('-', sc, shift);
	for (unsigned i = 0; i < ARRAY_SIZE(KeyNames); i++)
	{
		if (strcasecmp(KeyNames[i].name, name) == 0)
		{
			*sc = KeyNames[i].sc;
			return true;
		}
	}
	char *end;
	long v = strtol(name, &end, 0);
	if (!*end && v > 0 && v < 0x80)
	{
		*sc = v;
		return true;
	}
	return false;
}


/* ------------------------------------------------------------------ */
/* job execution */

/* IKBD drops mouse packets when its output buffer is full, so feed
 * movement in chunks of at most one packet (+-127) per frame
 */
#define MOVE_CHUNK 127

/* ------------------------------------------------------------------ */
/* GEM pointer position via Line-A variables */

static uint32_t LineABase;

static bool linea_valid(uint32_t b)
{
	uint16_t planes = STMemory_ReadWord(b);
	uint16_t linewr = STMemory_ReadWord(b + 2);
	uint16_t width = STMemory_ReadWord(b - 0xc);
	uint16_t height = STMemory_ReadWord(b - 4);
	uint16_t bytes = STMemory_ReadWord(b - 2);

	if (!(planes == 1 || planes == 2 || planes == 4 || planes == 8 || planes == 16))
		return false;
	if (width < 320 || width > 2048 || height < 200 || height > 2048)
		return false;
	return bytes == linewr && (uint32_t)width * planes / 8 == bytes;
}

/* find Line-A variable base by its screen geometry signature in low RAM */
static bool linea_find(void)
{
	if (LineABase && linea_valid(LineABase))
		return true;
	LineABase = 0;
	uint32_t end = 0x40000;
	if (end > (uint32_t)ConfigureParams.Memory.STRamSize_KB * 1024)
		end = ConfigureParams.Memory.STRamSize_KB * 1024;
	for (uint32_t b = 0x800; b < end; b += 2)
	{
		if (linea_valid(b))
		{
			LineABase = b;
			return true;
		}
	}
	return false;
}

static bool gem_mouse_pos(int *x, int *y)
{
	if (!linea_find())
		return false;
	*x = (int16_t)STMemory_ReadWord(LineABase - 0x25a);	/* GCURX */
	*y = (int16_t)STMemory_ReadWord(LineABase - 0x258);	/* GCURY */
	return true;
}

/* returns true when action is complete, false to repeat it later */
static bool act_run(action_t *a)
{
	switch (a->kind)
	{
	case ACT_KEY:
		IKBD_PressSTKey(a->a, a->b);
		break;
	case ACT_MBUTTON:
		if (a->a == 0)
		{
			if (a->b)
				Keyboard.bLButtonDown |= BUTTON_MOUSE;
			else
				Keyboard.bLButtonDown &= ~BUTTON_MOUSE;
		}
		else
		{
			if (a->b)
				Keyboard.bRButtonDown |= BUTTON_MOUSE;
			else
				Keyboard.bRButtonDown &= ~BUTTON_MOUSE;
		}
		break;
	case ACT_MMOVE:
		KeyboardProcessor.Mouse.dx += a->a;
		KeyboardProcessor.Mouse.dy += a->b;
		break;
	case ACT_DBLCLICK:
		Keyboard.LButtonDblClk = 1;
		break;
	case ACT_JOY:
		JoyBits[a->a] = a->b;
		break;
	case ACT_MOUSE_TO:
	{
		int x, y;
		if (!gem_mouse_pos(&x, &y) || ++a->tries > 64)
			return true;
		int dx = a->a - x, dy = a->b - y;
		if (!dx && !dy)
			return true;
		/* scale both components, to move along a straight line */
		int dist = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
		if (dist > MOVE_CHUNK)
		{
			dx = dx * MOVE_CHUNK / dist;
			dy = dy * MOVE_CHUNK / dist;
		}
		KeyboardProcessor.Mouse.dx += dx;
		KeyboardProcessor.Mouse.dy += dy;
		return false;
	}
	}
	return true;
}

/* run an action list as a job, replying when done */
static void start_input_job(agent_req_t *req, action_t *acts, int nacts)
{
	job_t *job = calloc(1, sizeof(*job));
	job->kind = JOB_INPUT;
	job->req = req;
	job->acts = acts;
	job->nacts = nacts;
	job->next_frame = Frames;
	job->deadline = now_ms() + 60000;
	job_add(job);
}

/* returns true when the job is finished (and replied) */
static bool job_tick(job_t *job)
{
	sbuf_t sb;

	if (job->deadline && now_ms() > job->deadline)
	{
		if (job->kind == JOB_STOP)
		{
			Sbuf_Init(&sb);
			Sbuf_Add(&sb, "{\"ok\":true,\"stopped\":false,", 27);
			add_status(&sb);
			Sbuf_Add(&sb, "}", 1);
			reply_json(job->req, 200, &sb);
		}
		else
			reply_error(job->req, 408, "timed out waiting for emulated frames (state: %s)",
			            state_name());
		return true;
	}

	switch (job->kind)
	{
	case JOB_INPUT:
		while (job->pos < job->nacts && Frames >= job->next_frame)
		{
			action_t *a = &job->acts[job->pos];
			if (!act_run(a))
			{
				/* not there yet, retry after mouse packets arrive */
				job->next_frame = Frames + 2;
				break;
			}
			job->pos++;
			job->next_frame = Frames + a->delay;
		}
		if (job->pos < job->nacts || Frames < job->next_frame)
			return false;
		reply_ok_status(job->req);
		return true;

	case JOB_FRAMES:
		if (Frames < job->next_frame)
			return false;
		if (job->pause_after && !InDebugStop)
			Main_PauseEmulation(true);
		reply_ok_status(job->req);
		return true;

	case JOB_STOP:
		if (StopCount == job->stop_count)
			return false;
		Sbuf_Init(&sb);
		Sbuf_Add(&sb, "{\"ok\":true,\"stopped\":true,", 26);
		add_status(&sb);
		Sbuf_Add(&sb, ",\"regs\":", 8);
		add_regs(&sb);
		Sbuf_Add(&sb, ",\"disasm\":", 10);
		add_disasm(&sb, M68000_GetPC(), 4);
		Sbuf_Add(&sb, "}", 1);
		reply_json(job->req, 200, &sb);
		return true;
	}
	return true;
}

static void jobs_tick(void)
{
	job_t **pp = &Jobs;
	while (*pp)
	{
		job_t *job = *pp;
		if (job_tick(job))
		{
			*pp = job->next;
			free(job->acts);
			free(job);
		}
		else
			pp = &job->next;
	}
}

static void start_stop_wait(agent_req_t *req, int timeout_ms)
{
	job_t *job = calloc(1, sizeof(*job));
	job->kind = JOB_STOP;
	job->req = req;
	job->stop_count = StopCount;
	job->deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : 10000);
	job_add(job);
}


/* ------------------------------------------------------------------ */
/* handlers: general & emulation control */

static void h_index(agent_req_t *req);

static void h_status(agent_req_t *req)
{
	sbuf_t sb;
	Sbuf_Init(&sb);
	Sbuf_Add(&sb, "{\"ok\":true,", 11);
	add_status(&sb);
	Sbuf_Printf(&sb, ",\"machine\":\"%s\",\"cpu_level\":%d,\"st_ram_kb\":%d,\"tt_ram_kb\":%d",
	            machine_name(ConfigureParams.System.nMachineType),
	            ConfigureParams.System.nCpuLevel,
	            ConfigureParams.Memory.STRamSize_KB,
	            ConfigureParams.Memory.TTRamSize_KB);
	Sbuf_Add(&sb, ",\"tos\":{\"path\":", 15);
	Sbuf_JsonStr(&sb, ConfigureParams.Rom.szTosImageFileName,
	             strlen(ConfigureParams.Rom.szTosImageFileName));
	Sbuf_Printf(&sb, ",\"version\":\"%x.%02x\",\"emutos\":%s,\"address\":\"0x%06x\"}",
	            TosVersion >> 8, TosVersion & 0xff, bIsEmuTOS ? "true" : "false",
	            TosAddress);
	Sbuf_Printf(&sb, ",\"fast_forward\":%s,\"debugger_owned\":%s,\"breakpoints\":%d",
	            ConfigureParams.System.bFastForward ? "true" : "false",
	            OwnDebugger ? "true" : "false", BreakCond_CpuBreakPointCount());
	Sbuf_Printf(&sb, ",\"console_bytes\":%llu,\"pending_jobs\":%d",
	            (unsigned long long)ConsoleEnd, Jobs ? 1 : 0);
	Sbuf_Add(&sb, ",\"rom_dir\":", 11);
	Sbuf_JsonStr(&sb, RomDir, strlen(RomDir));
	Sbuf_Add(&sb, "}", 1);
	reply_json(req, 200, &sb);
}

static void h_pause(agent_req_t *req)
{
	if (InDebugStop)
	{
		reply_error(req, 409, "CPU is stopped in debugger");
		return;
	}
	Main_PauseEmulation(true);
	reply_ok_status(req);
}

static void h_resume(agent_req_t *req)
{
	if (InDebugStop)
		ResumeRequested = true;
	else
		Main_UnPauseEmulation();
	reply_ok_status(req);
}

/* run given number of frames (from paused or running), then pause */
static void h_run(agent_req_t *req)
{
	int frames = param_int(req, "frames", 1);
	if (frames < 1)
		frames = 1;
	if (InDebugStop)
		ResumeRequested = true;
	else
		Main_UnPauseEmulation();

	job_t *job = calloc(1, sizeof(*job));
	job->kind = JOB_FRAMES;
	job->req = req;
	job->next_frame = Frames + frames;
	job->pause_after = param_bool(req, "pause", true);
	/* generous deadline: 1s + 100ms / frame (fast forward makes it quicker) */
	job->deadline = now_ms() + 1000 + 100 * (uint64_t)frames;
	job_add(job);
}

static void h_reset(agent_req_t *req)
{
	const char *type = AgentHttp_Param(req, "type");
	bool cold = !(type && (strcmp(type, "warm") == 0 || strcmp(type, "soft") == 0));

	if (InDebugStop)
		ResumeRequested = true;
	Main_UnPauseEmulation();
	if (cold)
		Reset_Cold();
	else
		Reset_Warm();
	Statusbar_UpdateInfo();
	reply_ok_status(req);
}

static void h_fastforward(agent_req_t *req)
{
	ConfigureParams.System.bFastForward = param_bool(req, "on", true);
	reply_ok_status(req);
}

static void h_quit(agent_req_t *req)
{
	ConfigureParams.Log.bConfirmQuit = false;
	reply_ok_status(req);
	if (InDebugStop)
		ResumeRequested = true;
	Main_UnPauseEmulation();
	Main_RequestQuit(param_int(req, "code", 0));
}

static bool apply_cmdline(char *cmdline, char **out, size_t *outlen);

static void h_config(agent_req_t *req)
{
	const char *args = param_text(req, "args");
	if (!args || !*args)
	{
		reply_error(req, 400, "missing 'args' (Hatari command line options)");
		return;
	}
	char *copy = strdup(args);
	size_t len;
	char *out;
	bool ok = apply_cmdline(copy, &out, &len);
	free(copy);

	sbuf_t sb;
	Sbuf_Init(&sb);
	Sbuf_Printf(&sb, "{\"ok\":%s,\"output\":", ok ? "true" : "false");
	Sbuf_JsonStr(&sb, out, len);
	Sbuf_Add(&sb, ",", 1);
	add_status(&sb);
	Sbuf_Add(&sb, "}", 1);
	free(out);
	reply_json(req, ok ? 200 : 400, &sb);
}

/* apply command line options without interactive confirmation
 * dialogs (e.g. "reset needed"), output captured to *out
 */
static bool apply_cmdline(char *cmdline, char **out, size_t *outlen)
{
	int dlglevel = ConfigureParams.Log.nAlertDlgLogLevel;
	int level = Log_SetAlertLevel(LOG_FATAL);
	bool ok;

	ConfigureParams.Log.nAlertDlgLogLevel = LOG_FATAL;
	capture_begin();
	ok = Change_ApplyCommandline(cmdline);
	*out = capture_end(outlen);
	ConfigureParams.Log.nAlertDlgLogLevel = dlglevel;
	Log_SetAlertLevel(level);
	return ok;
}

/* append option value, escaping white-space with backslash as
 * expected by Change_ApplyCommandline() argument splitting
 */
static void add_arg(sbuf_t *sb, const char *value)
{
	for (; *value; value++)
	{
		if (isspace((unsigned char)*value))
			Sbuf_Add(sb, "\\", 1);
		Sbuf_Add(sb, value, 1);
	}
}

/* convenience wrapper: build option string from fixed option & path */
static void apply_option(agent_req_t *req, const char *opt, const char *value, bool reset)
{
	char *cmd = NULL;
	size_t len;
	sbuf_t sb;

	Sbuf_Init(&sb);
	Sbuf_Printf(&sb, "%s ", opt);
	add_arg(&sb, value);
	cmd = sb.data;
	char *out;
	bool ok = apply_cmdline(cmd, &out, &len);
	Sbuf_Free(&sb);
	if (ok && reset)
	{
		if (InDebugStop)
			ResumeRequested = true;
		Main_UnPauseEmulation();
		Reset_Cold();
		Statusbar_UpdateInfo();
	}
	Sbuf_Init(&sb);
	Sbuf_Printf(&sb, "{\"ok\":%s,\"output\":", ok ? "true" : "false");
	Sbuf_JsonStr(&sb, out, len);
	Sbuf_Add(&sb, ",", 1);
	add_status(&sb);
	Sbuf_Add(&sb, "}", 1);
	free(out);
	reply_json(req, ok ? 200 : 400, &sb);
}

static void h_floppy(agent_req_t *req)
{
	const char *drive = AgentHttp_Param(req, "drive");
	const char *path = AgentHttp_Param(req, "path");
	if (!path)
	{
		reply_error(req, 400, "missing 'path' (disk image)");
		return;
	}
	bool b = drive && (drive[0] == 'b' || drive[0] == 'B' || drive[0] == '1');
	apply_option(req, b ? "--disk-b" : "--disk-a", path, param_bool(req, "reset", false));
}

static void h_harddrive(agent_req_t *req)
{
	const char *path = AgentHttp_Param(req, "path");
	if (!path)
	{
		reply_error(req, 400, "missing 'path' (host directory for GEMDOS drive)");
		return;
	}
	apply_option(req, "--harddrive", path, param_bool(req, "reset", true));
}

static void h_autostart(agent_req_t *req)
{
	const char *prog = AgentHttp_Param(req, "program");
	if (!prog)
	{
		reply_error(req, 400, "missing 'program' (e.g. C:\\TEST.PRG)");
		return;
	}
	apply_option(req, "--auto", prog, param_bool(req, "reset", true));
}


/* ------------------------------------------------------------------ */
/* handlers: ROMs */

typedef struct {
	char *rel;
	off_t size;
	uint16_t version;
	uint16_t conf;
	bool emutos;
	uint32_t etos_version;
} rom_info_t;

static bool rom_probe(const char *path, rom_info_t *info)
{
	uint8_t hdr[0x40];
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return false;
	size_t n = fread(hdr, 1, sizeof(hdr), fp);
	fclose(fp);
	if (n < sizeof(hdr))
		return false;
	/* TOS ROMs start with a bra.s instruction */
	if (hdr[0] != 0x60)
		return false;
	info->version = hdr[2] << 8 | hdr[3];
	info->conf = hdr[0x1c] << 8 | hdr[0x1d];
	info->emutos = memcmp(hdr + 0x2c, "ETOS", 4) == 0;
	info->etos_version = info->emutos ?
		(uint32_t)hdr[0x3c] << 24 | hdr[0x3d] << 16 | hdr[0x3e] << 8 | hdr[0x3f] : 0;
	return true;
}

static void rom_scan(const char *base, const char *rel, rom_info_t **list, int *count, int depth)
{
	char path[FILENAME_MAX];
	struct dirent *de;
	DIR *dir;

	snprintf(path, sizeof(path), "%s%s%s", base, *rel ? "/" : "", rel);
	if (depth > 4 || !(dir = opendir(path)))
		return;
	while ((de = readdir(dir)))
	{
		char sub[FILENAME_MAX], full[FILENAME_MAX];
		struct stat st;

		if (de->d_name[0] == '.')
			continue;
		snprintf(sub, sizeof(sub), "%s%s%s", rel, *rel ? "/" : "", de->d_name);
		snprintf(full, sizeof(full), "%s/%s", base, sub);
		if (stat(full, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
		{
			rom_scan(base, sub, list, count, depth + 1);
			continue;
		}
		if (!(File_DoesFileExtensionMatch(de->d_name, ".img") ||
		      File_DoesFileExtensionMatch(de->d_name, ".rom") ||
		      File_DoesFileExtensionMatch(de->d_name, ".tos") ||
		      File_DoesFileExtensionMatch(de->d_name, ".bin")))
			continue;
		rom_info_t info = { 0 };
		if (!rom_probe(full, &info))
			continue;
		info.rel = strdup(sub);
		info.size = st.st_size;
		*list = realloc(*list, (*count + 1) * sizeof(**list));
		(*list)[(*count)++] = info;
	}
	closedir(dir);
}

static int rom_cmp(const void *a, const void *b)
{
	return strcmp(((const rom_info_t *)a)->rel, ((const rom_info_t *)b)->rel);
}

static void h_roms(agent_req_t *req)
{
	rom_info_t *list = NULL;
	int count = 0;
	sbuf_t sb;

	if (!RomDir[0])
	{
		reply_error(req, 409, "no ROM directory configured (--agent-rom-dir)");
		return;
	}
	rom_scan(RomDir, "", &list, &count, 0);
	qsort(list, count, sizeof(*list), rom_cmp);

	Sbuf_Init(&sb);
	Sbuf_Add(&sb, "{\"ok\":true,\"current\":", 21);
	Sbuf_JsonStr(&sb, ConfigureParams.Rom.szTosImageFileName,
	             strlen(ConfigureParams.Rom.szTosImageFileName));
	Sbuf_Add(&sb, ",\"roms\":[", 9);
	for (int i = 0; i < count; i++)
	{
		int country = list[i].conf >> 1;
		const char *lang = country == 127 ? "multi" : TOS_LanguageName(country);
		Sbuf_Add(&sb, i ? ",{\"name\":" : "{\"name\":", i ? 9 : 8);
		Sbuf_JsonStr(&sb, list[i].rel, strlen(list[i].rel));
		Sbuf_Printf(&sb, ",\"size_kb\":%lld,\"tos_version\":\"%x.%02x\",\"country\":%d,",
		            (long long)list[i].size / 1024,
		            list[i].version >> 8, list[i].version & 0xff, country);
		Sbuf_Add(&sb, "\"language\":", 11);
		Sbuf_JsonStr(&sb, lang ? lang : "?", strlen(lang ? lang : "?"));
		Sbuf_Printf(&sb, ",\"emutos\":%s", list[i].emutos ? "true" : "false");
		if (list[i].emutos)
			Sbuf_Printf(&sb, ",\"emutos_version\":\"%d.%d\"",
			            list[i].etos_version >> 24, (list[i].etos_version >> 16) & 0xff);
		Sbuf_Add(&sb, "}", 1);
		free(list[i].rel);
	}
	free(list);
	Sbuf_Add(&sb, "]}", 2);
	reply_json(req, 200, &sb);
}

static void h_rom_select(agent_req_t *req)
{
	const char *name = AgentHttp_Param(req, "name");
	const char *machine = AgentHttp_Param(req, "machine");
	char path[FILENAME_MAX];
	sbuf_t cmd;

	if (!name || !*name)
	{
		reply_error(req, 400, "missing 'name' (as listed by /roms, or absolute path)");
		return;
	}
	if (name[0] == '/' || !RomDir[0])
		Str_Copy(path, name, sizeof(path));
	else
	{
		if (strstr(name, ".."))
		{
			reply_error(req, 400, "'..' not allowed in ROM name");
			return;
		}
		snprintf(path, sizeof(path), "%s/%s", RomDir, name);
	}
	if (!File_Exists(path))
	{
		reply_error(req, 404, "ROM '%s' not found", path);
		return;
	}
	Sbuf_Init(&cmd);
	Sbuf_Add(&cmd, "--tos ", 6);
	add_arg(&cmd, path);
	if (machine && *machine)
		Sbuf_Printf(&cmd, " --machine %s", machine);
	const char *mem = AgentHttp_Param(req, "memory");
	if (mem && *mem)
		Sbuf_Printf(&cmd, " --memsize %s", mem);

	size_t len;
	char *out;
	bool ok = apply_cmdline(cmd.data, &out, &len);
	Sbuf_Free(&cmd);
	if (ok)
	{
		/* Change_ApplyCommandline() resets when TOS changes, but
		 * re-selecting the same ROM should give a fresh boot too
		 */
		if (InDebugStop)
			ResumeRequested = true;
		Main_UnPauseEmulation();
		Reset_Cold();
		Statusbar_UpdateInfo();
	}
	sbuf_t sb;
	Sbuf_Init(&sb);
	Sbuf_Printf(&sb, "{\"ok\":%s,\"output\":", ok ? "true" : "false");
	Sbuf_JsonStr(&sb, out, len);
	Sbuf_Printf(&sb, ",\"machine\":\"%s\",", machine_name(ConfigureParams.System.nMachineType));
	add_status(&sb);
	Sbuf_Add(&sb, "}", 1);
	free(out);
	reply_json(req, ok ? 200 : 400, &sb);
}


/* ------------------------------------------------------------------ */
/* handlers: screen */

static void h_screen(agent_req_t *req)
{
	const char *fmt = AgentHttp_Param(req, "format");
	char tmpl[] = "/tmp/hatari-agent-XXXXXX";
	char path[64];
	const char *ext = "png", *ctype = "image/png";

	if (fmt && strcmp(fmt, "bmp") == 0)
	{
		ext = "bmp";
		ctype = "image/bmp";
	}
	else if (fmt && strcmp(fmt, "neo") == 0)
	{
		ext = "neo";
		ctype = "application/octet-stream";
	}
	else if (fmt && strcmp(fmt, "ximg") == 0)
	{
		ext = "ximg";
		ctype = "application/octet-stream";
	}
	int fd = mkstemp(tmpl);
	if (fd < 0)
	{
		reply_error(req, 500, "mkstemp failed");
		return;
	}
	close(fd);
	unlink(tmpl);
	snprintf(path, sizeof(path), "%s.%s", tmpl, ext);

	size_t olen;
	capture_begin();
#if HAVE_LIBPNG
	if (strcmp(ext, "png") == 0 && !param_bool(req, "full", false))
	{
		/* native emulated resolution, coordinates match mouse x/y */
		if (ScreenSnapShot_SavePNG_Native(path) <= 0)
			ScreenSnapShot_SaveToFile(path);
	}
	else
#endif
		ScreenSnapShot_SaveToFile(path);
	char *out = capture_end(&olen);

	long size;
	uint8_t *data = File_ReadAsIs(path, &size);
	unlink(path);
	if (!data)
	{
		reply_error(req, 500, "screenshot failed: %s", out);
		free(out);
		return;
	}
	free(out);
	AgentHttp_Complete(req, 200, ctype, (char *)data, size);
}


/* ------------------------------------------------------------------ */
/* handlers: input */

static void h_key(agent_req_t *req)
{
	const char *keys = AgentHttp_Param(req, "key");
	const char *action = AgentHttp_Param(req, "action");
	int hold = param_int(req, "frames", 2);
	uint8_t sc[8];
	bool shift = false;
	int n = 0;

	if (!keys || !*keys)
	{
		reply_error(req, 400, "missing 'key' (name, character or scancode, '+' for combos e.g. ctrl+c)");
		return;
	}
	/* split combo "ctrl+shift+x" (or "ctrl shift x", as '+' decodes
	 * to space in query strings); a lone "+" is the plus key
	 */
	char *copy = strdup(keys), *save = NULL;
	char *tok = strcmp(copy, "+") == 0 ? copy : strtok_r(copy, "+ ", &save);
	while (tok && n < (int)ARRAY_SIZE(sc) - 1)
	{
		bool s;
		if (!keyname_to_scancode(tok, &sc[n], &s))
		{
			reply_error(req, 400, "unknown key '%s'", tok);
			free(copy);
			return;
		}
		shift |= s;
		n++;
		tok = tok == copy && strcmp(copy, "+") == 0 ? NULL : strtok_r(NULL, "+ ", &save);
	}
	free(copy);
	if (shift)
	{
		memmove(sc + 1, sc, n);
		sc[0] = SC_LSHIFT;
		n++;
	}

	bool down = true, up = true;
	if (action && strcmp(action, "down") == 0)
		up = false;
	else if (action && strcmp(action, "up") == 0)
		down = false;

	/* down/up only: apply immediately, no emulated time needed */
	if (!(down && up))
	{
		for (int i = 0; i < n; i++)
			IKBD_PressSTKey(sc[down ? i : n - 1 - i], down);
		reply_ok_status(req);
		return;
	}
	if (!need_running(req))
		return;
	action_t *acts = calloc(2 * n, sizeof(*acts));
	int k = 0;
	for (int i = 0; i < n; i++)
		acts[k++] = (action_t){ ACT_KEY, sc[i], true, i == n - 1 ? hold : 1 };
	for (int i = n - 1; i >= 0; i--)
		acts[k++] = (action_t){ ACT_KEY, sc[i], false, 1 };
	start_input_job(req, acts, k);
}

static void h_type(agent_req_t *req)
{
	const char *text = param_text(req, "text");
	int frames = param_int(req, "frames", 1);
	if (frames < 1)
		frames = 1;

	if (!text || !*text)
	{
		reply_error(req, 400, "missing 'text' (or raw body)");
		return;
	}
	if (!need_running(req))
		return;

	size_t len = strlen(text);
	action_t *acts = calloc(4 * len, sizeof(*acts));
	int k = 0;
	for (size_t i = 0; i < len; i++)
	{
		uint8_t sc;
		bool shift;
		if (!ascii_to_scancode(text[i], &sc, &shift))
		{
			free(acts);
			reply_error(req, 400, "can't type character 0x%02x at offset %zu (US layout)",
			            (unsigned char)text[i], i);
			return;
		}
		if (shift)
			acts[k++] = (action_t){ ACT_KEY, SC_LSHIFT, true, 0 };
		acts[k++] = (action_t){ ACT_KEY, sc, true, frames };
		acts[k++] = (action_t){ ACT_KEY, sc, false, shift ? 0 : frames };
		if (shift)
			acts[k++] = (action_t){ ACT_KEY, SC_LSHIFT, false, frames };
	}
	start_input_job(req, acts, k);
}


static int move_actions_needed(int dx, int dy)
{
	int n = (abs(dx) > abs(dy) ? abs(dx) : abs(dy));
	return n / MOVE_CHUNK + 2;
}

static void add_move(action_t *acts, int *k, int dx, int dy, int settle)
{
	while (dx || dy)
	{
		int cx = dx > MOVE_CHUNK ? MOVE_CHUNK : dx < -MOVE_CHUNK ? -MOVE_CHUNK : dx;
		int cy = dy > MOVE_CHUNK ? MOVE_CHUNK : dy < -MOVE_CHUNK ? -MOVE_CHUNK : dy;
		acts[(*k)++] = (action_t){ ACT_MMOVE, cx, cy, 1 };
		dx -= cx;
		dy -= cy;
	}
	if (*k)
		acts[*k - 1].delay = settle;
}

/* absolute positioning: pin pointer to top-left corner (the OS clamps
 * it there), then move by the wanted coordinates.  Returns actions.
 */
static action_t *absolute_move(int x, int y, int settle, int extra, int *k)
{
	int ax, ay, w, h, zx, zy, cx, cy;

	if (gem_mouse_pos(&cx, &cy))
	{
		/* steer along a straight path, so that e.g. open menus stay open */
		action_t *acts = calloc(1 + extra, sizeof(*acts));
		acts[0] = (action_t){ ACT_MOUSE_TO, x, y, settle, 0 };
		*k = 1;
		return acts;
	}
	ConvST_GetDisplayArea(&ax, &ay, &w, &h, &zx, &zy);
	int pin = (w > h ? w : h) + 16;
	action_t *acts = calloc(move_actions_needed(pin, pin) +
	                        move_actions_needed(x, y) + extra, sizeof(*acts));
	*k = 0;
	add_move(acts, k, -pin, -pin, 1);
	add_move(acts, k, x, y, settle);
	return acts;
}

static void h_mouse_pos(agent_req_t *req)
{
	int x, y;
	sbuf_t sb;

	if (!gem_mouse_pos(&x, &y))
	{
		reply_error(req, 409, "GEM pointer position unknown (Line-A variables not found)");
		return;
	}
	Sbuf_Init(&sb);
	Sbuf_Printf(&sb, "{\"ok\":true,\"x\":%d,\"y\":%d,\"buttons\":{\"left\":%s,\"right\":%s},\"linea\":\"0x%06x\"}",
	            x, y, Keyboard.bLButtonDown ? "true" : "false",
	            Keyboard.bRButtonDown ? "true" : "false", LineABase);
	reply_json(req, 200, &sb);
}

static void h_mouse(agent_req_t *req)
{
	const char *x = AgentHttp_Param(req, "x");
	const char *y = AgentHttp_Param(req, "y");
	int settle = param_int(req, "frames", 4);
	action_t *acts;
	int k = 0;

	if (!need_running(req))
		return;
	if (x && y)
		acts = absolute_move(atoi(x), atoi(y), settle, 0, &k);
	else
	{
		int dx = param_int(req, "dx", 0), dy = param_int(req, "dy", 0);
		acts = calloc(move_actions_needed(dx, dy), sizeof(*acts));
		add_move(acts, &k, dx, dy, settle);
	}
	start_input_job(req, acts, k);
}

static void h_click(agent_req_t *req)
{
	const char *button = AgentHttp_Param(req, "button");
	const char *action = AgentHttp_Param(req, "action");
	const char *x = AgentHttp_Param(req, "x");
	const char *y = AgentHttp_Param(req, "y");
	int hold = param_int(req, "frames", 3);
	int b = button && strcmp(button, "right") == 0 ? 1 : 0;

	if (!action)
		action = "click";
	if (strcmp(action, "down") == 0 || strcmp(action, "up") == 0)
	{
		action_t a = { ACT_MBUTTON, b, action[0] == 'd', 0, 0 };
		act_run(&a);
		reply_ok_status(req);
		return;
	}
	if (!need_running(req))
		return;
	action_t *acts;
	int k = 0;
	if (x && y)
		acts = absolute_move(atoi(x), atoi(y), 4, 4, &k);
	else
		acts = calloc(4, sizeof(*acts));
	if (strcmp(action, "double") == 0)
	{
		if (b == 0)
			acts[k++] = (action_t){ ACT_DBLCLICK, 0, 0, 12 };
		else
		{
			for (int i = 0; i < 2; i++)
			{
				acts[k++] = (action_t){ ACT_MBUTTON, b, true, 2 };
				acts[k++] = (action_t){ ACT_MBUTTON, b, false, 2 };
			}
		}
	}
	else if (strcmp(action, "click") == 0)
	{
		acts[k++] = (action_t){ ACT_MBUTTON, b, true, hold };
		acts[k++] = (action_t){ ACT_MBUTTON, b, false, hold };
	}
	else
	{
		free(acts);
		reply_error(req, 400, "unknown action '%s' (click, double, down, up)", action);
		return;
	}
	start_input_job(req, acts, k);
}

static void h_joystick(agent_req_t *req)
{
	int port = param_int(req, "port", 1);
	const char *dirs = AgentHttp_Param(req, "dirs");
	int frames = param_int(req, "frames", 0);
	uint8_t bits = 0;

	if (port < 0 || port > 1)
	{
		reply_error(req, 400, "port must be 0 (mouse port) or 1 (joystick port)");
		return;
	}
	if (dirs)
	{
		char *copy = strdup(dirs), *save = NULL;
		for (char *t = strtok_r(copy, ",+ ", &save); t; t = strtok_r(NULL, ",+ ", &save))
		{
			if (strcmp(t, "up") == 0)
				bits |= ATARIJOY_BITMASK_UP;
			else if (strcmp(t, "down") == 0)
				bits |= ATARIJOY_BITMASK_DOWN;
			else if (strcmp(t, "left") == 0)
				bits |= ATARIJOY_BITMASK_LEFT;
			else if (strcmp(t, "right") == 0)
				bits |= ATARIJOY_BITMASK_RIGHT;
			else if (strcmp(t, "fire") == 0)
				bits |= ATARIJOY_BITMASK_FIRE;
			else if (strcmp(t, "none") != 0)
			{
				reply_error(req, 400, "unknown direction '%s' (up,down,left,right,fire,none)", t);
				free(copy);
				return;
			}
		}
		free(copy);
	}
	if (param_bool(req, "fire", false))
		bits |= ATARIJOY_BITMASK_FIRE;

	/* frames=0: latch state until changed */
	if (frames <= 0)
	{
		JoyBits[port] = bits;
		reply_ok_status(req);
		return;
	}
	if (!need_running(req))
		return;
	action_t *acts = calloc(2, sizeof(*acts));
	acts[0] = (action_t){ ACT_JOY, port, bits, frames };
	acts[1] = (action_t){ ACT_JOY, port, 0, 1 };
	start_input_job(req, acts, 2);
}

uint8_t AgentApi_JoystickBits(int port)
{
	return (port >= 0 && port < 2) ? JoyBits[port] : 0;
}


/* ------------------------------------------------------------------ */
/* handlers: memory & CPU */

static void h_mem_read(agent_req_t *req)
{
	uint32_t addr, len = 256;
	const char *fmt = AgentHttp_Param(req, "format");

	if (!param_value(req, "addr", &addr, true) ||
	    !param_value(req, "len", &len, false))
		return;
	if (len > 16*1024*1024)
	{
		reply_error(req, 400, "len too large (max 16MB)");
		return;
	}
	if (fmt && strcmp(fmt, "bin") == 0)
	{
		char *data = malloc(len ? len : 1);
		for (uint32_t i = 0; i < len; i++)
			data[i] = STMemory_ReadByte(addr + i);
		AgentHttp_Complete(req, 200, "application/octet-stream", data, len);
		return;
	}
	sbuf_t sb;
	Sbuf_Init(&sb);
	Sbuf_Printf(&sb, "{\"ok\":true,\"addr\":\"0x%06x\",\"len\":%u,\"hex\":\"", addr, len);
	for (uint32_t i = 0; i < len; i++)
		Sbuf_Printf(&sb, "%02x", STMemory_ReadByte(addr + i));
	Sbuf_Add(&sb, "\"}", 2);
	reply_json(req, 200, &sb);
}

static void h_mem_write(agent_req_t *req)
{
	uint32_t addr;
	const char *hex = param_text(req, "hex");
	size_t n = 0;

	if (!param_value(req, "addr", &addr, true))
		return;
	if (!hex)
	{
		reply_error(req, 400, "missing 'hex' (or raw hex body)");
		return;
	}
	for (const char *p = hex; *p; )
	{
		if (isspace((unsigned char)*p))
		{
			p++;
			continue;
		}
		int hi = isxdigit((unsigned char)p[0]) ? p[0] : -1;
		int lo = isxdigit((unsigned char)p[1]) ? p[1] : -1;
		if (hi < 0 || lo < 0)
		{
			reply_error(req, 400, "invalid hex at offset %zu", (size_t)(p - hex));
			return;
		}
		char byte[3] = { (char)hi, (char)lo, 0 };
		STMemory_WriteByte(addr + n, strtoul(byte, NULL, 16));
		n++;
		p += 2;
	}
	sbuf_t sb;
	Sbuf_Init(&sb);
	Sbuf_Printf(&sb, "{\"ok\":true,\"addr\":\"0x%06x\",\"written\":%zu}", addr, n);
	reply_json(req, 200, &sb);
}

static void h_regs(agent_req_t *req)
{
	sbuf_t sb;
	Sbuf_Init(&sb);
	Sbuf_Add(&sb, "{\"ok\":true,\"regs\":", 18);
	add_regs(&sb);
	Sbuf_Add(&sb, ",", 1);
	add_status(&sb);
	Sbuf_Add(&sb, "}", 1);
	reply_json(req, 200, &sb);
}

static void h_regs_set(agent_req_t *req)
{
	if (bEmulationActive && !InDebugStop)
	{
		reply_error(req, 409, "pause or stop emulation before changing registers");
		return;
	}
	for (int i = 0; i < req->nparams; i++)
	{
		const char *name = req->keys[i];
		uint32_t value, *addr;
		if (!param_value(req, name, &value, true))
			return;
		if (strcasecmp(name, "pc") == 0)
			M68000_SetPC(value);
		else if (strcasecmp(name, "sr") == 0)
			M68000_SetSR(value);
		else if (DebugCpu_GetRegisterAddress(name, &addr) == 32)
			*addr = value;
		else
		{
			reply_error(req, 400, "unknown register '%s'", name);
			return;
		}
	}
	h_regs(req);
}

static void h_disasm(agent_req_t *req)
{
	uint32_t addr = M68000_GetPC();
	int count = param_int(req, "count", 16);

	if (!param_value(req, "addr", &addr, false))
		return;
	if (count < 1)
		count = 1;
	if (count > 1000)
		count = 1000;
	sbuf_t sb;
	Sbuf_Init(&sb);
	Sbuf_Add(&sb, "{\"ok\":true,\"lines\":", 19);
	add_disasm(&sb, addr, count);
	Sbuf_Add(&sb, "}", 1);
	reply_json(req, 200, &sb);
}


/* ------------------------------------------------------------------ */
/* handlers: debugger */

/* run debugger command with captured output.  Returns DEBUGGER_* code. */
static int run_debug_cmd(const char *cmd, char **out, size_t *len)
{
	int ret;
	capture_begin();
	ret = DebugUI_RemoteCommand(cmd);
	*out = capture_end(len);
	return ret;
}

static void h_debug_cmd(agent_req_t *req)
{
	const char *cmd = param_text(req, "cmd");
	char *out;
	size_t len;

	if (!cmd || !*cmd)
	{
		reply_error(req, 400, "missing 'cmd' (debugger command line)");
		return;
	}
	int ret = run_debug_cmd(cmd, &out, &len);
	bool resumes = (ret == DEBUGGER_END);
	if (resumes)
	{
		if (InDebugStop)
			ResumeRequested = true;
		else
			resumes = false;	/* nothing to resume from */
	}
	sbuf_t sb;
	Sbuf_Init(&sb);
	Sbuf_Add(&sb, "{\"ok\":true,\"output\":", 20);
	Sbuf_JsonStr(&sb, out, len);
	free(out);
	Sbuf_Printf(&sb, ",\"resumed\":%s,", resumes ? "true" : "false");
	add_status(&sb);
	Sbuf_Add(&sb, "}", 1);

	/* when command resumes emulation (c/s/n...), optionally wait for next stop */
	if (resumes && param_bool(req, "wait", false))
	{
		Sbuf_Free(&sb);
		start_stop_wait(req, param_int(req, "timeout_ms", 10000));
		return;
	}
	reply_json(req, 200, &sb);
}

static void h_break(agent_req_t *req)
{
	if (InDebugStop)
	{
		reply_ok_status(req);
		return;
	}
	BreakRequested = true;
	DebugCpu_RequestBreak();
	Main_UnPauseEmulation();
	start_stop_wait(req, param_int(req, "timeout_ms", 5000));
}

static void h_continue(agent_req_t *req)
{
	if (!InDebugStop)
	{
		Main_UnPauseEmulation();
		reply_ok_status(req);
		return;
	}
	ResumeRequested = true;
	if (param_bool(req, "wait", false))
		start_stop_wait(req, param_int(req, "timeout_ms", 10000));
	else
		reply_ok_status(req);
}

/* step / next: resume through debugger command, reply on next stop */
static void step_cmd(agent_req_t *req, const char *cmd)
{
	char *out;
	size_t len;

	if (!InDebugStop)
	{
		reply_error(req, 409, "CPU is not stopped in debugger (use /debug/break)");
		return;
	}
	int ret = run_debug_cmd(cmd, &out, &len);
	if (ret != DEBUGGER_END)
	{
		reply_error(req, 400, "'%s' failed: %s", cmd, out);
		free(out);
		return;
	}
	free(out);
	ResumeRequested = true;
	start_stop_wait(req, param_int(req, "timeout_ms", 10000));
}

static void h_step(agent_req_t *req)
{
	int count = param_int(req, "count", 1);
	char cmd[32];
	if (count <= 1)
		Str_Copy(cmd, "s", sizeof(cmd));
	else
		snprintf(cmd, sizeof(cmd), "c %d", count);
	step_cmd(req, cmd);
}

static void h_next(agent_req_t *req)
{
	const char *type = AgentHttp_Param(req, "type");
	char cmd[64];
	snprintf(cmd, sizeof(cmd), "n%s%s", type ? " " : "", type ? type : "");
	step_cmd(req, cmd);
}

static void h_wait(agent_req_t *req)
{
	if (InDebugStop && param_bool(req, "current", true))
	{
		/* already stopped: report immediately */
		job_t job = { .kind = JOB_STOP, .req = req, .stop_count = StopCount - 1 };
		job_tick(&job);
		return;
	}
	start_stop_wait(req, param_int(req, "timeout_ms", 10000));
}

static void h_breakpoints(agent_req_t *req)
{
	sbuf_t sb;
	Sbuf_Init(&sb);
	Sbuf_Add(&sb, "{\"ok\":true,\"breakpoints\":[", 26);
	int count = BreakCond_CpuBreakPointCount();
	for (int i = 0; i < count; i++)
	{
		const char *expr;
		int hits;
		bool once, trace;
		if (!BreakCond_GetCpuBreakPoint(i, &expr, &hits, &once, &trace))
			continue;
		Sbuf_Printf(&sb, "%s{\"index\":%d,\"expression\":", i ? "," : "", i + 1);
		Sbuf_JsonStr(&sb, expr, strlen(expr));
		Sbuf_Printf(&sb, ",\"hits\":%d,\"once\":%s,\"trace\":%s}", hits,
		            once ? "true" : "false", trace ? "true" : "false");
	}
	Sbuf_Add(&sb, "]}", 2);
	reply_json(req, 200, &sb);
}

static void h_breakpoint_add(agent_req_t *req)
{
	const char *addr = AgentHttp_Param(req, "addr");
	const char *cond = AgentHttp_Param(req, "cond");
	const char *opts = AgentHttp_Param(req, "options");
	char cmd[MAX_DEBUG_CMD_LEN];
	char *out;
	size_t len;

	if (addr && *addr)
		snprintf(cmd, sizeof(cmd), "a %s%s%s", addr, opts ? " :" : "", opts ? opts : "");
	else if (cond && *cond)
		snprintf(cmd, sizeof(cmd), "b %s%s%s", cond, opts ? " :" : "", opts ? opts : "");
	else
	{
		reply_error(req, 400, "give 'addr' (PC address/symbol) or 'cond' (condition expression)");
		return;
	}
	int before = BreakCond_CpuBreakPointCount();
	run_debug_cmd(cmd, &out, &len);
	int after = BreakCond_CpuBreakPointCount();
	if (after <= before)
	{
		reply_error(req, 400, "breakpoint not added: %s", out);
		free(out);
		return;
	}
	free(out);
	h_breakpoints(req);
}

static void h_breakpoint_del(agent_req_t *req)
{
	const char *index = AgentHttp_Param(req, "index");
	char cmd[64];
	char *out;
	size_t len;

	if (!index || !*index)
	{
		reply_error(req, 400, "missing 'index' (1-based, or 'all')");
		return;
	}
	if (strcmp(index, "all") == 0)
		Str_Copy(cmd, "b all", sizeof(cmd));
	else
		snprintf(cmd, sizeof(cmd), "b %d", atoi(index));
	int before = BreakCond_CpuBreakPointCount();
	run_debug_cmd(cmd, &out, &len);
	if (BreakCond_CpuBreakPointCount() >= before && before > 0)
	{
		reply_error(req, 400, "breakpoint not removed: %s", out);
		free(out);
		return;
	}
	free(out);
	h_breakpoints(req);
}


/* ------------------------------------------------------------------ */
/* handlers: snapshots & console */

static void h_state_save(agent_req_t *req)
{
	const char *path = AgentHttp_Param(req, "path");
	if (!path || !*path)
	{
		reply_error(req, 400, "missing 'path'");
		return;
	}
	size_t len;
	capture_begin();
	/* event handler & debugger both run between CPU instructions */
	MemorySnapShot_Capture_Immediate(path, false);
	char *out = capture_end(&len);
	bool ok = File_Exists(path);
	sbuf_t sb;
	Sbuf_Init(&sb);
	Sbuf_Printf(&sb, "{\"ok\":%s,\"output\":", ok ? "true" : "false");
	Sbuf_JsonStr(&sb, out, len);
	Sbuf_Add(&sb, "}", 1);
	free(out);
	reply_json(req, ok ? 200 : 500, &sb);
}

static void h_state_load(agent_req_t *req)
{
	const char *path = AgentHttp_Param(req, "path");
	if (!path || !File_Exists(path))
	{
		reply_error(req, 400, "missing or non-existing 'path'");
		return;
	}
	bool was_paused = !bEmulationActive && !InDebugStop;
	MemorySnapShot_Restore(path, false);
	/* restore happens when CPU loop continues, give it a frame */
	if (InDebugStop)
		ResumeRequested = true;
	Main_UnPauseEmulation();
	job_t *job = calloc(1, sizeof(*job));
	job->kind = JOB_FRAMES;
	job->req = req;
	job->next_frame = Frames + 1;
	job->pause_after = was_paused || param_bool(req, "pause", false);
	job->deadline = now_ms() + 5000;
	job_add(job);
}

void AgentApi_ConsoleWrite(const char *buf, int len)
{
	for (int i = 0; i < len; i++)
		ConsoleBuf[(ConsoleEnd + i) % CONSOLE_SIZE] = buf[i];
	ConsoleEnd += len;
}

static void h_console(agent_req_t *req)
{
	const char *since_str = AgentHttp_Param(req, "since");
	uint64_t since = since_str ? strtoull(since_str, NULL, 0) : 0;
	uint64_t start = ConsoleEnd > CONSOLE_SIZE ? ConsoleEnd - CONSOLE_SIZE : 0;
	bool truncated = false;

	if (since < start)
	{
		since = start;
		truncated = since_str != NULL;
	}
	if (since > ConsoleEnd)
		since = ConsoleEnd;
	size_t len = ConsoleEnd - since;
	char *text = malloc(len + 1);
	for (size_t i = 0; i < len; i++)
		text[i] = ConsoleBuf[(since + i) % CONSOLE_SIZE];

	sbuf_t sb;
	Sbuf_Init(&sb);
	Sbuf_Add(&sb, "{\"ok\":true,\"text\":", 18);
	Sbuf_JsonStr(&sb, text, len);
	Sbuf_Printf(&sb, ",\"from\":%llu,\"next\":%llu,\"truncated\":%s}",
	            (unsigned long long)since, (unsigned long long)ConsoleEnd,
	            truncated ? "true" : "false");
	free(text);
	reply_json(req, 200, &sb);
}


/* ------------------------------------------------------------------ */
/* routing */

typedef struct {
	const char *method;	/* NULL = any */
	const char *path;
	void (*handler)(agent_req_t *req);
	const char *help;
} route_t;

static const route_t Routes[] = {
	{ "GET",  "/",                   h_index,     "this endpoint list" },
	{ "GET",  "/status",             h_status,    "emulator state, machine, TOS, frame counter" },
	{ "POST", "/emu/pause",          h_pause,     "pause emulation" },
	{ "POST", "/emu/resume",         h_resume,    "resume emulation (also leaves debugger stop)" },
	{ "POST", "/emu/run",            h_run,       "run frames=N emulated frames, then pause (pause=0 keeps running)" },
	{ "POST", "/emu/reset",          h_reset,     "reset, type=cold|warm" },
	{ "POST", "/emu/fastforward",    h_fastforward, "on=1|0 run as fast as possible" },
	{ "POST", "/emu/quit",           h_quit,      "quit Hatari, code=exit code" },
	{ "POST", "/config",             h_config,    "apply Hatari command line options, args=... (may reset)" },
	{ "GET",  "/roms",               h_roms,      "list TOS ROM images in --agent-rom-dir" },
	{ "POST", "/roms/select",        h_rom_select,"boot ROM name=..., optional machine=st|ste|megast|megaste|tt|falcon, memory=KB" },
	{ "POST", "/media/floppy",       h_floppy,    "insert disk image, drive=a|b path=..., reset=0|1" },
	{ "POST", "/media/harddrive",    h_harddrive, "GEMDOS drive from host dir path=..., reset=1|0" },
	{ "POST", "/media/autostart",    h_autostart, "autostart program=C:\\X.PRG after reset (reset=1|0)" },
	{ "GET",  "/screen",             h_screen,    "PNG screenshot at native emulated resolution (full=1: host window incl. borders), format=png|bmp|neo|ximg" },
	{ "POST", "/input/key",          h_key,       "key=name|char|scancode (combos ctrl+c), action=press|down|up, frames=hold" },
	{ "POST", "/input/type",         h_type,      "type text=... (or raw body), US layout, frames=per event" },
	{ "GET",  "/input/mouse",        h_mouse_pos, "current GEM pointer position (from Line-A variables)" },
	{ "POST", "/input/mouse",        h_mouse,     "move dx=,dy= (relative) or x=,y= (absolute via corner pinning)" },
	{ "POST", "/input/click",        h_click,     "button=left|right action=click|double|down|up, optional x=,y=" },
	{ "POST", "/input/joystick",     h_joystick,  "port=1 dirs=up,left,fire frames=N (0 = latch until changed)" },
	{ "GET",  "/mem",                h_mem_read,  "read memory addr=,len=, format=hex|bin" },
	{ "POST", "/mem",                h_mem_write, "write memory addr=, hex=... (or raw hex body)" },
	{ "GET",  "/cpu/regs",           h_regs,      "CPU registers" },
	{ "POST", "/cpu/regs",           h_regs_set,  "set registers, e.g. d0=0x10&pc=0xfc0030 (paused/stopped only)" },
	{ "GET",  "/cpu/disasm",         h_disasm,    "disassemble addr= (default PC), count=" },
	{ "POST", "/debug/break",        h_break,     "stop CPU in debugger now; replies when stopped" },
	{ "POST", "/debug/continue",     h_continue,  "leave debugger stop, wait=1 to block until next stop" },
	{ "POST", "/debug/step",         h_step,      "single-step count=N instructions; replies with new state" },
	{ "POST", "/debug/next",         h_next,      "step over subroutine calls (type= opcode type, see debugger 'next')" },
	{ "POST", "/debug/wait",         h_wait,      "block until CPU stops (breakpoint etc.), timeout_ms=" },
	{ "GET",  "/debug/breakpoints",  h_breakpoints, "list CPU breakpoints" },
	{ "POST", "/debug/breakpoints",  h_breakpoint_add, "add addr=<address/symbol> or cond=<expression>, options=once|trace|..." },
	{ "DELETE", "/debug/breakpoints",h_breakpoint_del, "remove index=N|all" },
	{ "POST", "/debug/cmd",          h_debug_cmd, "run any Hatari debugger command cmd=... (output captured), wait=1" },
	{ "POST", "/state/save",         h_state_save,"save memory snapshot path=..." },
	{ "POST", "/state/load",         h_state_load,"restore memory snapshot path=..." },
	{ "GET",  "/console",            h_console,   "program console output (--conout / NatFeats), since=offset" },
};

static void h_index(agent_req_t *req)
{
	sbuf_t sb;
	Sbuf_Init(&sb);
	Sbuf_Add(&sb, "{\"ok\":true,\"name\":\"Hatari agent API\",\"version\":1,\"doc\":\"doc/agent-api.md\",\"endpoints\":[", 89);
	for (unsigned i = 0; i < ARRAY_SIZE(Routes); i++)
	{
		Sbuf_Printf(&sb, "%s{\"method\":\"%s\",\"path\":\"%s\",\"help\":", i ? "," : "",
		            Routes[i].method, Routes[i].path);
		Sbuf_JsonStr(&sb, Routes[i].help, strlen(Routes[i].help));
		Sbuf_Add(&sb, "}", 1);
	}
	Sbuf_Add(&sb, "]}", 2);
	reply_json(req, 200, &sb);
}

static void dispatch(agent_req_t *req)
{
	bool path_found = false;
	const char *method = req->method;

	/* GET handlers serve HEAD too */
	if (strcmp(method, "HEAD") == 0)
		method = "GET";
	for (unsigned i = 0; i < ARRAY_SIZE(Routes); i++)
	{
		if (strcmp(Routes[i].path, req->path) != 0)
			continue;
		path_found = true;
		if (strcmp(Routes[i].method, method) == 0)
		{
			Routes[i].handler(req);
			return;
		}
	}
	if (path_found)
		reply_error(req, 405, "method %s not allowed for %s", req->method, req->path);
	else
		reply_error(req, 404, "unknown endpoint %s (GET / lists endpoints)", req->path);
}


/* ------------------------------------------------------------------ */
/* main thread entry points */

static void update_frames(void)
{
	if (nVBLs != LastVBL)
	{
		int diff = nVBLs - LastVBL;
		/* nVBLs resets on reset / snapshot restore */
		Frames += diff > 0 ? diff : 1;
		LastVBL = nVBLs;
	}
}

void AgentApi_Poll(void)
{
	static bool polling;
	agent_req_t *req;

	if (!Running || polling)
		return;
	polling = true;

	update_frames();
	while ((req = AgentHttp_Take()))
	{
		while (req)
		{
			agent_req_t *next = req->next;
			dispatch(req);
			req = next;
		}
	}
	if (Jobs)
		jobs_tick();

	polling = false;
}

void AgentApi_DebugStop(int reason)
{
	if (BreakRequested && reason == REASON_CPU_STEPS)
		reason = REASON_USER;
	BreakRequested = false;

	InDebugStop = true;
	ResumeRequested = false;
	StopReason = reason;
	StopPC = M68000_GetPC();
	StopCount++;
	fprintf(stderr, "Agent API: CPU stopped (%s) at $%06x, waiting for remote commands\n",
	        reason_name(reason), StopPC);

	while (!ResumeRequested && !bQuitProgram)
	{
		AgentApi_Poll();
		if (ResumeRequested)
			break;
		AgentHttp_WaitIncoming(Jobs ? 20 : 100);
	}
	InDebugStop = false;
}

static void wakeup(void)
{
	GuiEvent_WakeUp();
}

void AgentApi_Init(void)
{
	const char *err;

	if (!Port || Running)
		return;
	err = AgentHttp_Start(Bind, Port, wakeup);
	if (err)
	{
		Log_Printf(LOG_ERROR, "Agent API: can't listen on %s:%d: %s\n", Bind, Port, err);
		return;
	}
	Running = true;
	LastVBL = nVBLs;
	/* a remote agent can't click away modal host dialogs, so log
	 * non-fatal alerts instead of showing them, and don't ask
	 * for confirmation on quit
	 */
	ConfigureParams.Log.nAlertDlgLogLevel = LOG_FATAL;
	Log_SetAlertLevel(LOG_FATAL);
	ConfigureParams.Log.bConfirmQuit = false;
	Log_Printf(LOG_INFO, "Agent API: listening on http://%s:%d/\n", Bind, Port);
	fprintf(stderr, "Agent API: listening on http://%s:%d/\n", Bind, Port);
}

void AgentApi_UnInit(void)
{
	if (!Running)
		return;
	/* fail pending jobs */
	while (Jobs)
	{
		job_t *job = Jobs;
		Jobs = job->next;
		reply_error(job->req, 503, "emulator shutting down");
		free(job->acts);
		free(job);
	}
	Running = false;
	AgentHttp_Stop();
}

#else	/* !HAVE_UNIX_DOMAIN_SOCKETS */

const char *AgentApi_SetPort(const char *arg) { return "agent API not supported on this platform"; }
const char *AgentApi_SetBind(const char *arg) { return NULL; }
const char *AgentApi_SetRomDir(const char *arg) { return NULL; }
void AgentApi_SetOwnDebugger(bool own) { }
void AgentApi_Init(void) { }
void AgentApi_UnInit(void) { }
bool AgentApi_IsEnabled(void) { return false; }
void AgentApi_Poll(void) { }
bool AgentApi_OwnsDebugger(void) { return false; }
void AgentApi_DebugStop(int reason) { }
void AgentApi_ConsoleWrite(const char *buf, int len) { }
uint8_t AgentApi_JoystickBits(int port) { return 0; }

#endif /* HAVE_UNIX_DOMAIN_SOCKETS */
