/*
  Hatari - agenthttp.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  Minimal HTTP/1.1 server for the agent API.  Only what is needed for
  local tool use: one request per connection ("Connection: close"),
  Content-Length bodies, query string and urlencoded form parameters.
*/
const char AgentHttp_fileid[] = "Hatari agenthttp.c";

#include "config.h"

#if HAVE_UNIX_DOMAIN_SOCKETS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "agenthttp.h"

#define MAX_HEADER_LEN	(16*1024)
#define MAX_BODY_LEN	(32*1024*1024)

static int ListenFd = -1;
static pthread_t ListenThread;
static void (*WakeUp)(void);

static pthread_mutex_t QueueLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t QueueCond = PTHREAD_COND_INITIALIZER;   /* new request */
static pthread_cond_t DoneCond = PTHREAD_COND_INITIALIZER;    /* request completed */
static agent_req_t *Incoming;
static volatile bool Stopping;


/* ------------------------------------------------------------------ */
/* string buffer helpers */

void Sbuf_Init(sbuf_t *sb)
{
	sb->data = NULL;
	sb->len = sb->size = 0;
}

void Sbuf_Free(sbuf_t *sb)
{
	free(sb->data);
	Sbuf_Init(sb);
}

static void Sbuf_Reserve(sbuf_t *sb, size_t add)
{
	if (sb->len + add + 1 <= sb->size)
		return;
	size_t size = sb->size ? sb->size : 256;
	while (size < sb->len + add + 1)
		size *= 2;
	char *data = realloc(sb->data, size);
	if (!data)
	{
		perror("agenthttp: realloc");
		abort();
	}
	sb->data = data;
	sb->size = size;
}

void Sbuf_Add(sbuf_t *sb, const char *data, size_t len)
{
	Sbuf_Reserve(sb, len);
	memcpy(sb->data + sb->len, data, len);
	sb->len += len;
	sb->data[sb->len] = '\0';
}

void Sbuf_Printf(sbuf_t *sb, const char *fmt, ...)
{
	va_list ap;
	int len;

	va_start(ap, fmt);
	len = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (len < 0)
		return;
	Sbuf_Reserve(sb, len);
	va_start(ap, fmt);
	vsnprintf(sb->data + sb->len, len + 1, fmt, ap);
	va_end(ap);
	sb->len += len;
}

/**
 * Append given string as a quoted JSON string.  Bytes >= 0x80 that
 * don't form valid UTF-8 (e.g. Atari charset) are escaped as \u00XX.
 */
void Sbuf_JsonStr(sbuf_t *sb, const char *str, size_t len)
{
	Sbuf_Add(sb, "\"", 1);
	for (size_t i = 0; i < len; i++)
	{
		unsigned char c = str[i];
		switch (c)
		{
		case '"':  Sbuf_Add(sb, "\\\"", 2); continue;
		case '\\': Sbuf_Add(sb, "\\\\", 2); continue;
		case '\n': Sbuf_Add(sb, "\\n", 2); continue;
		case '\r': Sbuf_Add(sb, "\\r", 2); continue;
		case '\t': Sbuf_Add(sb, "\\t", 2); continue;
		}
		if (c < 0x20 || c == 0x7f)
		{
			Sbuf_Printf(sb, "\\u%04x", c);
			continue;
		}
		if (c >= 0x80)
		{
			/* pass through well-formed UTF-8 sequences */
			int n = (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 :
			        (c & 0xf8) == 0xf0 ? 3 : -1;
			bool ok = n > 0 && i + n < len;
			for (int j = 1; ok && j <= n; j++)
				ok = ((unsigned char)str[i+j] & 0xc0) == 0x80;
			if (ok)
			{
				Sbuf_Add(sb, str + i, n + 1);
				i += n;
			}
			else
				Sbuf_Printf(sb, "\\u%04x", c);
			continue;
		}
		Sbuf_Add(sb, (const char *)&c, 1);
	}
	Sbuf_Add(sb, "\"", 1);
}


/* ------------------------------------------------------------------ */
/* request parsing */

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	c = tolower(c);
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

/* in-place URL decoding ('+' -> space, %XX) */
static void url_decode(char *s)
{
	char *d = s;
	while (*s)
	{
		if (*s == '+')
		{
			*d++ = ' ';
			s++;
		}
		else if (*s == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0)
		{
			*d++ = hexval(s[1]) << 4 | hexval(s[2]);
			s += 3;
		}
		else
			*d++ = *s++;
	}
	*d = '\0';
}

/* parse "a=1&b=2" into request parameters (string is modified & kept) */
static void parse_params(agent_req_t *req, char *str)
{
	char *pair, *save = NULL;
	for (pair = strtok_r(str, "&", &save); pair; pair = strtok_r(NULL, "&", &save))
	{
		if (req->nparams >= AGENT_MAX_PARAMS)
			break;
		char *eq = strchr(pair, '=');
		if (eq)
			*eq++ = '\0';
		else
			eq = pair + strlen(pair);
		url_decode(pair);
		url_decode(eq);
		req->keys[req->nparams] = strdup(pair);
		req->vals[req->nparams] = strdup(eq);
		req->nparams++;
	}
}

const char *AgentHttp_Param(const agent_req_t *req, const char *key)
{
	/* last one wins, so that body overrides query string */
	for (int i = req->nparams - 1; i >= 0; i--)
	{
		if (strcmp(req->keys[i], key) == 0)
			return req->vals[i];
	}
	return NULL;
}

static void req_free(agent_req_t *req)
{
	for (int i = 0; i < req->nparams; i++)
	{
		free(req->keys[i]);
		free(req->vals[i]);
	}
	free(req->body);
	free(req->resp);
	free(req);
}

static bool send_all(int fd, const char *data, size_t len)
{
	while (len)
	{
		ssize_t n = send(fd, data, len, 0);
		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			return false;
		}
		data += n;
		len -= n;
	}
	return true;
}

static const char *status_text(int status)
{
	switch (status)
	{
	case 200: return "OK";
	case 400: return "Bad Request";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 408: return "Request Timeout";
	case 409: return "Conflict";
	case 413: return "Payload Too Large";
	case 500: return "Internal Server Error";
	case 503: return "Service Unavailable";
	}
	return "Unknown";
}

static void send_simple(int fd, int status, const char *msg)
{
	char buf[512];
	int len = snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}\n", msg);
	char hdr[256];
	int hlen = snprintf(hdr, sizeof(hdr),
	                    "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
	                    "Content-Length: %d\r\nConnection: close\r\n\r\n",
	                    status, status_text(status), len);
	send_all(fd, hdr, hlen);
	send_all(fd, buf, len);
}

/* find header value (case-insensitive name) within header block */
static const char *find_header(const char *hdrs, const char *name, size_t *vlen)
{
	size_t nlen = strlen(name);
	const char *line = strstr(hdrs, "\r\n");
	while (line && line[2])
	{
		line += 2;
		if (strncasecmp(line, name, nlen) == 0 && line[nlen] == ':')
		{
			const char *v = line + nlen + 1;
			while (*v == ' ' || *v == '\t')
				v++;
			const char *end = strstr(v, "\r\n");
			*vlen = end ? (size_t)(end - v) : strlen(v);
			return v;
		}
		line = strstr(line, "\r\n");
	}
	return NULL;
}

/**
 * Read & parse one request from the socket.
 * Returns NULL (after sending error response) on failure.
 */
static agent_req_t *read_request(int fd)
{
	char *buf = malloc(MAX_HEADER_LEN + 1);
	size_t got = 0;
	char *hend = NULL;

	if (!buf)
		return NULL;
	while (!hend)
	{
		if (got >= MAX_HEADER_LEN)
		{
			send_simple(fd, 413, "headers too large");
			free(buf);
			return NULL;
		}
		ssize_t n = recv(fd, buf + got, MAX_HEADER_LEN - got, 0);
		if (n <= 0)
		{
			if (n < 0 && errno == EINTR)
				continue;
			free(buf);
			return NULL;
		}
		got += n;
		buf[got] = '\0';
		hend = strstr(buf, "\r\n\r\n");
	}
	*hend = '\0';
	size_t hlen = hend - buf + 4;

	agent_req_t *req = calloc(1, sizeof(*req));
	char target[2048];
	if (!req || sscanf(buf, "%7s %2047s", req->method, target) != 2)
	{
		send_simple(fd, 400, "malformed request line");
		free(buf);
		free(req);
		return NULL;
	}

	/* body */
	size_t vlen, clen = 0;
	const char *v = find_header(buf, "Content-Length", &vlen);
	if (v)
		clen = strtoul(v, NULL, 10);
	if (clen > MAX_BODY_LEN)
	{
		send_simple(fd, 413, "body too large");
		free(buf);
		free(req);
		return NULL;
	}
	bool form = false;
	v = find_header(buf, "Content-Type", &vlen);
	if (v && vlen >= 33 && strncasecmp(v, "application/x-www-form-urlencoded", 33) == 0)
		form = true;

	req->body = malloc(clen + 1);
	size_t have = got - hlen;
	if (have > clen)
		have = clen;
	memcpy(req->body, buf + hlen, have);
	free(buf);
	while (have < clen)
	{
		ssize_t n = recv(fd, req->body + have, clen - have, 0);
		if (n <= 0)
		{
			if (n < 0 && errno == EINTR)
				continue;
			req_free(req);
			return NULL;
		}
		have += n;
	}
	req->body[clen] = '\0';
	req->body_len = clen;

	/* path & query string */
	char *query = strchr(target, '?');
	if (query)
		*query++ = '\0';
	url_decode(target);
	snprintf(req->path, sizeof(req->path), "%s", target);
	/* strip trailing slash (except root) */
	size_t plen = strlen(req->path);
	if (plen > 1 && req->path[plen-1] == '/')
		req->path[plen-1] = '\0';
	if (query)
		parse_params(req, query);
	if (form && clen)
	{
		char *copy = strdup(req->body);
		parse_params(req, copy);
		free(copy);
	}
	return req;
}


/* ------------------------------------------------------------------ */
/* threads */

static void *conn_thread(void *arg)
{
	int fd = (int)(intptr_t)arg;
	agent_req_t *req = read_request(fd);

	if (req)
	{
		pthread_mutex_lock(&QueueLock);
		if (Stopping)
		{
			pthread_mutex_unlock(&QueueLock);
			send_simple(fd, 503, "emulator shutting down");
			req_free(req);
			close(fd);
			return NULL;
		}
		/* append to keep FIFO order */
		agent_req_t **tail = &Incoming;
		while (*tail)
			tail = &(*tail)->next;
		*tail = req;
		pthread_cond_broadcast(&QueueCond);
		pthread_mutex_unlock(&QueueLock);

		if (WakeUp)
			WakeUp();

		pthread_mutex_lock(&QueueLock);
		while (!req->done)
			pthread_cond_wait(&DoneCond, &QueueLock);
		pthread_mutex_unlock(&QueueLock);

		char hdr[512];
		int hlen = snprintf(hdr, sizeof(hdr),
		                    "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
		                    "Content-Length: %zu\r\nCache-Control: no-store\r\n"
		                    "Connection: close\r\n\r\n",
		                    req->status, status_text(req->status),
		                    req->ctype ? req->ctype : "application/json",
		                    req->resp_len);
		if (send_all(fd, hdr, hlen) && strcmp(req->method, "HEAD") != 0)
			send_all(fd, req->resp, req->resp_len);
		req_free(req);
	}
	close(fd);
	return NULL;
}

static void *listen_thread(void *arg)
{
	(void)arg;
	while (!Stopping)
	{
		int fd = accept(ListenFd, NULL, NULL);
		if (fd < 0)
		{
			if (errno == EINTR)
				continue;
			if (!Stopping)
				perror("agenthttp: accept");
			break;
		}
		int one = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
		setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
		/* don't let a stalled client keep a thread forever while reading */
		struct timeval tv = { 30, 0 };
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

		pthread_t th;
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		if (pthread_create(&th, &attr, conn_thread, (void *)(intptr_t)fd) != 0)
		{
			send_simple(fd, 503, "out of threads");
			close(fd);
		}
		pthread_attr_destroy(&attr);
	}
	return NULL;
}

const char *AgentHttp_Start(const char *bind_addr, int port, void (*wakeup)(void))
{
	struct sockaddr_in addr;
	int one = 1;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1)
		return "invalid bind address (IPv4 dotted quad expected)";

	/* a peer closing its connection early must not kill the emulator */
	signal(SIGPIPE, SIG_IGN);

	ListenFd = socket(AF_INET, SOCK_STREAM, 0);
	if (ListenFd < 0)
		return "socket() failed";
	setsockopt(ListenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (bind(ListenFd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{
		close(ListenFd);
		ListenFd = -1;
		return "bind() failed (port in use?)";
	}
	if (listen(ListenFd, 16) < 0)
	{
		close(ListenFd);
		ListenFd = -1;
		return "listen() failed";
	}
	WakeUp = wakeup;
	Stopping = false;
	if (pthread_create(&ListenThread, NULL, listen_thread, NULL) != 0)
	{
		close(ListenFd);
		ListenFd = -1;
		return "pthread_create() failed";
	}
	return NULL;
}

void AgentHttp_Stop(void)
{
	agent_req_t *req;

	if (ListenFd < 0)
		return;
	Stopping = true;
	shutdown(ListenFd, SHUT_RDWR);
	close(ListenFd);
	ListenFd = -1;
	pthread_join(ListenThread, NULL);

	/* fail anything still queued */
	while ((req = AgentHttp_Take()))
	{
		while (req)
		{
			agent_req_t *next = req->next;
			AgentHttp_Complete(req, 503, NULL,
			                   strdup("{\"ok\":false,\"error\":\"emulator shutting down\"}\n"), 0);
			req = next;
		}
	}
}

agent_req_t *AgentHttp_Take(void)
{
	agent_req_t *list;

	if (!Incoming)	/* racy peek is fine, we come back soon */
		return NULL;
	pthread_mutex_lock(&QueueLock);
	list = Incoming;
	Incoming = NULL;
	pthread_mutex_unlock(&QueueLock);
	return list;
}

void AgentHttp_WaitIncoming(int timeout_ms)
{
	struct timeval now;
	struct timespec ts;

	gettimeofday(&now, NULL);
	long nsec = now.tv_usec * 1000L + (timeout_ms % 1000) * 1000000L;
	ts.tv_sec = now.tv_sec + timeout_ms / 1000 + nsec / 1000000000L;
	ts.tv_nsec = nsec % 1000000000L;

	pthread_mutex_lock(&QueueLock);
	if (!Incoming)
		pthread_cond_timedwait(&QueueCond, &QueueLock, &ts);
	pthread_mutex_unlock(&QueueLock);
}

void AgentHttp_Complete(agent_req_t *req, int status, const char *ctype,
                        char *data, size_t len)
{
	if (data && !len)
		len = strlen(data);
	req->next = NULL;
	pthread_mutex_lock(&QueueLock);
	req->status = status;
	req->ctype = ctype;
	req->resp = data;
	req->resp_len = data ? len : 0;
	req->done = true;
	pthread_cond_broadcast(&DoneCond);
	pthread_mutex_unlock(&QueueLock);
}

#endif /* HAVE_UNIX_DOMAIN_SOCKETS */
