/** @file
 *
 * llm implementation: a private worker drains a FIFO of prepared
 * HTTP requests, one blocking round-trip per task.  The request is
 * built whole on the asking thread (fail fast, nothing to copy
 * later); the worker dials, sends, reads to EOF (Connection: close),
 * parses and extracts, then reports through the callback.
 * Cancellation shuts the in-flight socket down from outside, which
 * pops the worker out of poll/recv.
 *
 * Constraints: plain C23 + POSIX (sockets, pthread).  No Qt, no C++;
 * the C++ layer consumes llm.h only.
 */
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "list_priv.h"
#include "llm_priv.h"

static constexpr int    kConnectMs = 5000;
static constexpr size_t kMaxReply  = (size_t)1 << 26;

/* ---- growable buffer ------------------------------------------- */

bool
llm_buf_put (struct llm_buf *b,
             void const     *data,
             size_t          n)
{
	if (!n)
		return true;
	if (n > b->cap - b->size) {
		size_t cap = b->cap ? b->cap : 256U;
		char *p;
		while (cap - b->size < n) {
			if (cap > SIZE_MAX / 2)
				return false;
			cap *= 2;
		}
		p = realloc(b->data, cap);
		if (!p)
			return false;
		b->data = p;
		b->cap = cap;
	}
	memcpy(b->data + b->size, data, n);
	b->size += n;
	return true;
}

bool
llm_buf_str (struct llm_buf *b,
             char const     *s)
{
	return llm_buf_put(b, s, strlen(s));
}

void
llm_buf_free (struct llm_buf *b)
{
	free(b->data);
	*b = (struct llm_buf){};
}

/* ---- JSON out: escaping ---------------------------------------- */

static char const kHexDig[] = "0123456789abcdef";

/* Control bytes with a short named escape; the zero-filled rest of
 * the table takes the \u00xx form.
 */
static char const kNamedEsc[0x20] = {
	['\b'] = 'b', ['\f'] = 'f', ['\n'] = 'n',
	['\r'] = 'r', ['\t'] = 't',
};

/* The escape sequence for a byte, or 0 length for "pass through".
 * UTF-8 lead and continuation bytes pass through: JSON strings are
 * raw UTF-8 on the wire.
 */
static size_t
json_esc (unsigned char ch, char esc[static 6])
{
	if (ch == '"' || ch == '\\') {
		esc[0] = '\\';
		esc[1] = (char)ch;
		return 2;
	}
	if (ch >= 0x20)
		return 0;
	esc[0] = '\\';
	if (kNamedEsc[ch]) {
		esc[1] = kNamedEsc[ch];
		return 2;
	}
	memcpy(esc + 1, "u00", 3);
	esc[4] = kHexDig[ch >> 4];
	esc[5] = kHexDig[ch & 15];
	return 6;
}

bool
llm_buf_json (struct llm_buf *b,
              char const     *s)
{
	if (!llm_buf_put(b, "\"", 1))
		return false;
	while (*s) {
		char const *run = s;
		char esc[6];
		size_t n = 0;
		while (*s && !(n = json_esc((unsigned char)*s, esc)))
			++s;
		if (s > run && !llm_buf_put(b, run, (size_t)(s - run)))
			return false;
		if (n && !llm_buf_put(b, esc, n))
			return false;
		s += !!n;
	}
	return llm_buf_put(b, "\"", 1);
}

/* ---- JSON in: cursor extraction -------------------------------- */

enum {
	CWS  = 1, /* JSON whitespace */
	CSEP = 2, /* ends a bare scalar */
};

static unsigned char const kClass[256] = {
	[' ']  = CWS | CSEP, ['\t'] = CWS | CSEP,
	['\n'] = CWS | CSEP, ['\r'] = CWS | CSEP,
	[',']  = CSEP, [']'] = CSEP, ['}'] = CSEP, ['"'] = CSEP,
};

static void
ws (struct llm_cur *c)
{
	while (c->p < c->end && (kClass[(unsigned char)*c->p] & CWS))
		++c->p;
}

/* Advance past a string, escapes respected; cursor at the quote. */
static bool
skip_str (struct llm_cur *c)
{
	if (c->p >= c->end || *c->p != '"')
		return false;
	for (++c->p; c->p < c->end; ++c->p) {
		if (*c->p == '\\') {
			++c->p;
			continue;
		}
		if (*c->p == '"') {
			++c->p;
			return true;
		}
	}
	return false;
}

/* Advance past one complete value of any type. */
static bool
skip_value (struct llm_cur *c)
{
	size_t depth = 0;
	do {
		ws(c);
		if (c->p >= c->end)
			return false;
		switch (*c->p) {
		case '"':
			if (!skip_str(c))
				return false;
			break;
		case '{': case '[':
			if (++depth > 64)
				return false;
			++c->p;
			break;
		case '}': case ']':
			if (!depth--)
				return false;
			++c->p;
			break;
		case ',': case ':':
			if (!depth)
				return false;
			++c->p;
			break;
		default: {
			char const *at = c->p;
			while (c->p < c->end &&
			       !(kClass[(unsigned char)*c->p] & CSEP))
				++c->p;
			if (c->p == at)
				return false;
		}
		}
	} while (depth);
	return true;
}

bool
llm_json_get (struct llm_cur *c,
              char const     *key)
{
	size_t klen = strlen(key);
	ws(c);
	if (c->p >= c->end || *c->p != '{')
		return false;
	++c->p;
	for (;;) {
		char const *kstart, *kend;
		ws(c);
		if (c->p < c->end && *c->p == ',') {
			++c->p;
			continue;
		}
		if (c->p >= c->end || *c->p != '"')
			return false;
		kstart = c->p + 1;
		if (!skip_str(c))
			return false;
		kend = c->p - 1;
		ws(c);
		if (c->p >= c->end || *c->p != ':')
			return false;
		++c->p;
		ws(c);
		if ((size_t)(kend - kstart) == klen &&
		    !memcmp(kstart, key, klen))
			return true;
		if (!skip_value(c))
			return false;
	}
}

bool
llm_json_first (struct llm_cur *c)
{
	ws(c);
	if (c->p >= c->end || *c->p != '[')
		return false;
	++c->p;
	ws(c);
	return c->p < c->end && *c->p != ']';
}

/* Hex digit values offset by one so the zero-filled rest of the
 * table reads as invalid.
 */
static signed char const kHex[256] = {
	['0'] = 1,  ['1'] = 2,  ['2'] = 3,  ['3'] = 4,  ['4'] = 5,
	['5'] = 6,  ['6'] = 7,  ['7'] = 8,  ['8'] = 9,  ['9'] = 10,
	['a'] = 11, ['b'] = 12, ['c'] = 13,
	['d'] = 14, ['e'] = 15, ['f'] = 16,
	['A'] = 11, ['B'] = 12, ['C'] = 13,
	['D'] = 14, ['E'] = 15, ['F'] = 16,
};

static int
hexval (unsigned char ch)
{
	return kHex[ch] - 1;
}

static int
hex4 (char const *p)
{
	int v = 0;
	for (int i = 0; i < 4; ++i) {
		int d = hexval(p[i]);
		if (d < 0)
			return -1;
		v = v << 4 | d;
	}
	return v;
}

static bool
put_utf8 (struct llm_buf *out, uint32_t cp)
{
	static unsigned char const kLead[] = { 0x00, 0xc0, 0xe0, 0xf0 };
	size_t n = (cp >= 0x80) + (cp >= 0x800) + (cp >= 0x10000);
	char u[4];
	for (size_t i = n; i; --i) {
		u[i] = (char)(0x80 | (cp & 0x3f));
		cp >>= 6;
	}
	u[0] = (char)(kLead[n] | cp);
	return llm_buf_put(out, u, n + 1);
}

/* \uXXXX after the cursor (past the "\u"), surrogate pairs joined. */
static bool
unescape_u (struct llm_cur *c, struct llm_buf *out)
{
	uint32_t cp;
	int v;
	if (c->end - c->p < 4 || (v = hex4(c->p)) < 0)
		return false;
	c->p += 4;
	cp = (uint32_t)v;
	if (cp >= 0xdc00 && cp <= 0xdfff)
		return false;
	if (cp >= 0xd800 && cp <= 0xdbff) {
		if (c->end - c->p < 6 || c->p[0] != '\\' || c->p[1] != 'u')
			return false;
		v = hex4(c->p + 2);
		if (v < 0xdc00 || v > 0xdfff)
			return false;
		c->p += 6;
		cp = 0x10000 + ((cp - 0xd800) << 10) + ((uint32_t)v - 0xdc00);
	}
	return put_utf8(out, cp);
}

/* Escape letter to produced byte; 0 = invalid ('u' handled apart). */
static char const kUnesc[256] = {
	['"'] = '"',  ['\\'] = '\\', ['/'] = '/',  ['b'] = '\b',
	['f'] = '\f', ['n']  = '\n', ['r'] = '\r', ['t'] = '\t',
};

/* One escape sequence at the cursor (at the backslash). */
static bool
unescape (struct llm_cur *c, struct llm_buf *out)
{
	char ch;
	if (c->end - c->p < 2)
		return false;
	ch = c->p[1];
	c->p += 2;
	if (ch == 'u')
		return unescape_u(c, out);
	ch = kUnesc[(unsigned char)ch];
	return ch && llm_buf_put(out, &ch, 1);
}

bool
llm_json_str (struct llm_cur *c,
              struct llm_buf *out)
{
	ws(c);
	if (c->p >= c->end || *c->p != '"')
		return false;
	++c->p;
	while (c->p < c->end && *c->p != '"') {
		char const *run = c->p;
		while (c->p < c->end && *c->p != '"' && *c->p != '\\')
			++c->p;
		if (c->p > run && !llm_buf_put(out, run, (size_t)(c->p - run)))
			return false;
		if (c->p < c->end && *c->p == '\\' && !unescape(c, out))
			return false;
	}
	if (c->p >= c->end)
		return false;
	++c->p;
	return true;
}

/* ---- HTTP response parsing ------------------------------------- */

static size_t
hdr_end (char const *raw, size_t size)
{
	for (size_t i = 0; i + 3 < size; ++i)
		if (!memcmp(raw + i, "\r\n\r\n", 4))
			return i + 4;
	return 0;
}

static int
status_code (char const *raw, size_t n)
{
	size_t i = 0;
	int code = 0;
	while (i < n && raw[i] != ' ' && raw[i] != '\r')
		++i;
	if (i >= n || raw[i] != ' ')
		return 0;
	for (++i; i < n && raw[i] >= '0' && raw[i] <= '9'; ++i) {
		if (code > 99)
			return 0;
		code = code * 10 + (raw[i] - '0');
	}
	return code;
}

static char
low (char ch)
{
	return ch >= 'A' && ch <= 'Z' ? (char)(ch + 32) : ch;
}

/* Case-insensitive prefix match; pfx must already be lowercase. */
static bool
ipfx (char const *s, size_t n, char const *pfx)
{
	size_t m = strlen(pfx);
	if (n < m)
		return false;
	for (size_t i = 0; i < m; ++i)
		if (low(s[i]) != pfx[i])
			return false;
	return true;
}

static bool
ihas (char const *s, size_t n, char const *sub)
{
	size_t m = strlen(sub);
	for (size_t i = 0; i + m <= n; ++i)
		if (ipfx(s + i, n - i, sub))
			return true;
	return false;
}

static size_t
dec_after (char const *s, size_t n, size_t at)
{
	size_t v = 0;
	bool any = false;
	while (at < n && s[at] == ' ')
		++at;
	for (; at < n && s[at] >= '0' && s[at] <= '9'; ++at) {
		if (v > (SIZE_MAX - 9) / 10)
			return SIZE_MAX;
		v = v * 10 + (size_t)(s[at] - '0');
		any = true;
	}
	return any ? v : SIZE_MAX;
}

static void
header_line (char const *s, size_t n, size_t *cl, bool *chunked)
{
	if (ipfx(s, n, "content-length:"))
		*cl = dec_after(s, n, 15);
	else if (ipfx(s, n, "transfer-encoding:") &&
	         ihas(s + 18, n - 18, "chunked"))
		*chunked = true;
}

/* The status line falls through header_line unharmed: it matches
 * neither header name.
 */
static void
scan_headers (char const *raw, size_t n, size_t *cl, bool *chunked)
{
	size_t i = 0;
	while (i + 1 < n) {
		size_t j = i;
		while (j + 1 < n && !(raw[j] == '\r' && raw[j + 1] == '\n'))
			++j;
		if (j > i)
			header_line(raw + i, j - i, cl, chunked);
		i = j + 2;
	}
}

static bool
dechunk (char const *p, char const *end, struct llm_buf *out)
{
	for (;;) {
		size_t n = 0;
		bool any = false;
		int d;
		while (p < end && (d = hexval(*p)) >= 0) {
			if (n > SIZE_MAX >> 4)
				return false;
			n = n << 4 | (size_t)d;
			++p;
			any = true;
		}
		if (!any)
			return false;
		while (p < end && *p != '\n')
			++p;
		if (p >= end)
			return false;
		++p;
		if (!n)
			return true;    /* final chunk; trailers ignored */
		if ((size_t)(end - p) < n || !llm_buf_put(out, p, n))
			return false;
		p += n;
		if (p < end && *p == '\r')
			++p;
		if (p >= end || *p != '\n')
			return false;
		++p;
	}
}

int
llm_http_parse (char const     *raw,
                size_t          size,
                struct llm_buf *dec,
                char const    **body,
                size_t         *body_size)
{
	size_t cl = SIZE_MAX;
	bool chunked = false;
	size_t split;
	int code;
	if (size < 13 || memcmp(raw, "HTTP/", 5))
		return LLM_ERR_PARSE;
	split = hdr_end(raw, size);
	if (!split)
		return LLM_ERR_PARSE;
	code = status_code(raw, split);
	if (code < 100)
		return LLM_ERR_PARSE;
	scan_headers(raw, split, &cl, &chunked);
	if (chunked) {
		if (!dechunk(raw + split, raw + size, dec))
			return LLM_ERR_PARSE;
		*body = dec->data ? dec->data : "";
		*body_size = dec->size;
		return code;
	}
	*body = raw + split;
	*body_size = size - split;
	if (cl == SIZE_MAX)
		return code;    /* close-delimited */
	if (cl > *body_size)
		return LLM_ERR_PARSE;   /* connection died mid-body */
	*body_size = cl;
	return code;
}

/* ---- reply digestion ------------------------------------------- */

static int
extract_content (struct llm_cur *cur, struct llm_buf *out)
{
	if (!llm_json_get(cur, "choices") || !llm_json_first(cur) ||
	    !llm_json_get(cur, "message") ||
	    !llm_json_get(cur, "content") || !llm_json_str(cur, out))
		return LLM_ERR_PARSE;
	return LLM_OK;
}

/* Best effort: {"error":{"message":"..."}} or {"error":"..."}; a
 * matchless body still reports LLM_ERR_HTTP, just textless.
 */
static int
extract_error (struct llm_cur *cur, struct llm_buf *out)
{
	struct llm_cur err = *cur;
	if (llm_json_get(&err, "error")) {
		struct llm_cur msg = err;
		if (llm_json_get(&msg, "message") &&
		    llm_json_str(&msg, out))
			return LLM_ERR_HTTP;
		out->size = 0;
		if (llm_json_str(&err, out))
			return LLM_ERR_HTTP;
	}
	out->size = 0;
	return LLM_ERR_HTTP;
}

static int
digest (struct llm_buf const *in, struct llm_buf *out)
{
	struct llm_buf dec = {};
	char const *body = nullptr;
	size_t n = 0;
	int code = llm_http_parse(in->data, in->size, &dec, &body, &n);
	struct llm_cur cur = { body, body + n };
	int status = code < 0            ? LLM_ERR_PARSE
	           : code / 100 == 2     ? extract_content(&cur, out)
	           : extract_error(&cur, out);
	llm_buf_free(&dec);
	return status;
}

/* ---- sockets ---------------------------------------------------- */

/* Poll one condition, EINTR retried; > 0 ready, 0 timeout, < 0 error. */
static int
await (int fd, short events, int ms)
{
	struct pollfd pf = { .fd = fd, .events = events };
	int r;
	do
		r = poll(&pf, 1, ms);
	while (r < 0 && errno == EINTR);
	return r;
}

static int
dial_one (struct addrinfo const *ai)
{
	int fd = socket(ai->ai_family,
	                ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
	                ai->ai_protocol);
	if (fd < 0)
		return -1;
	if (!connect(fd, ai->ai_addr, ai->ai_addrlen))
		return fd;
	if (errno == EINPROGRESS && await(fd, POLLOUT, kConnectMs) > 0) {
		int err = 0;
		socklen_t len = sizeof err;
		if (!getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) &&
		    !err)
			return fd;
	}
	close(fd);
	return -1;
}

static int
dial (char const *host, char const *port)
{
	struct addrinfo hints = {
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM,
	};
	struct addrinfo *info = nullptr;
	int fd = -1;
	if (getaddrinfo(host, port, &hints, &info))
		return -1;
	for (struct addrinfo *ai = info; ai && fd < 0; ai = ai->ai_next)
		fd = dial_one(ai);
	freeaddrinfo(info);
	return fd;
}

static int
send_all (int fd, char const *data, size_t n, int ms)
{
	while (n) {
		ssize_t w;
		if (await(fd, POLLOUT, ms) <= 0)
			return LLM_ERR_SEND;
		w = send(fd, data, n, MSG_NOSIGNAL);
		if (w < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		if (w <= 0)
			return LLM_ERR_SEND;
		data += (size_t)w;
		n -= (size_t)w;
	}
	return 0;
}

static int
recv_all (int fd, struct llm_buf *in, int ms)
{
	for (;;) {
		char tmp[16384];
		ssize_t r;
		int p = await(fd, POLLIN, ms);
		if (!p)
			return LLM_ERR_TIMEOUT;
		if (p < 0)
			return LLM_ERR_RECV;
		r = recv(fd, tmp, sizeof tmp, 0);
		if (!r)
			return 0;
		if (r < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		if (r < 0 || in->size + (size_t)r > kMaxReply)
			return LLM_ERR_RECV;
		if (!llm_buf_put(in, tmp, (size_t)r))
			return LLM_ERR_MEM;
	}
}

/* ---- the client ------------------------------------------------- */

struct llm_job {
	struct list     link;
	uint64_t        id;
	llm_done_fn     done;
	void           *ud;
	int             timeout_ms;
	struct llm_buf  req;    /* the full request, headers + body */
};

struct llm {
	pthread_mutex_t  mtx;
	pthread_cond_t   cond;
	pthread_t        worker;
	struct list      queue;   /* pending llm_job links */
	struct llm_job  *active;  /* in flight, owned by the worker */
	uint64_t         last_id;
	int              fd;      /* in-flight socket, for cancellation */
	bool             cancel;  /* retire the active job */
	bool             quit;
	char            *host;
	char             port[6];
};

/* Deliver and release; text (when given) gains a terminator here. */
static void
finish (struct llm_job *job, int status, struct llm_buf *text)
{
	if (text && llm_buf_put(text, "", 1))
		job->done(job->ud, job->id, status, text->data,
		          text->size - 1);
	else
		job->done(job->ud, job->id, status, nullptr, 0);
	llm_buf_free(&job->req);
}

static int
round_trip (struct llm *c, struct llm_job *job,
            struct llm_buf *in, struct llm_buf *out)
{
	int fd = dial(c->host, c->port);
	int status;
	if (fd < 0)
		return LLM_ERR_CONNECT;
	pthread_mutex_lock(&c->mtx);
	c->fd = fd;
	status = c->cancel ? LLM_ERR_CANCEL : 0;
	pthread_mutex_unlock(&c->mtx);
	if (!status)
		status = send_all(fd, job->req.data, job->req.size,
		                  job->timeout_ms);
	if (!status)
		status = recv_all(fd, in, job->timeout_ms);
	pthread_mutex_lock(&c->mtx);
	c->fd = -1;
	pthread_mutex_unlock(&c->mtx);
	close(fd);
	return status ? status : digest(in, out);
}

static void
run_job (struct llm *c, struct llm_job *job)
{
	struct llm_buf in = {}, out = {};
	int status = round_trip(c, job, &in, &out);
	pthread_mutex_lock(&c->mtx);
	if (c->cancel)
		status = LLM_ERR_CANCEL;
	pthread_mutex_unlock(&c->mtx);
	if (status == LLM_OK || (status == LLM_ERR_HTTP && out.size))
		finish(job, status, &out);
	else
		finish(job, status, nullptr);
	llm_buf_free(&in);
	llm_buf_free(&out);
	free(job);
}

static void *
work (void *arg)
{
	struct llm *c = arg;
	pthread_mutex_lock(&c->mtx);
	while (!c->quit) {
		struct list *n = list_head(&c->queue);
		struct llm_job *job;
		if (!n) {
			pthread_cond_wait(&c->cond, &c->mtx);
			continue;
		}
		list_del(n);
		job = container_of(n, struct llm_job, link);
		c->active = job;
		c->cancel = false;
		pthread_mutex_unlock(&c->mtx);
		run_job(c, job);
		pthread_mutex_lock(&c->mtx);
		c->active = nullptr;
	}
	pthread_mutex_unlock(&c->mtx);
	return nullptr;
}

static bool
build_request (struct llm const *c, struct llm_task const *task,
               struct llm_buf *req)
{
	static char const kOpen[] =
		"{\"stream\":false,\"cache_prompt\":true,\"messages\":[";
	struct llm_buf body = {};
	char head[512];
	int n;
	bool ok = llm_buf_str(&body, kOpen);
	if (ok && task->system && *task->system)
		ok = llm_buf_str(&body, "{\"role\":\"system\",\"content\":")
		  && llm_buf_json(&body, task->system)
		  && llm_buf_str(&body, "},");
	ok = ok
	  && llm_buf_str(&body, "{\"role\":\"user\",\"content\":")
	  && llm_buf_json(&body, task->prompt)
	  && llm_buf_str(&body, "}]");
	if (ok && task->max_tokens > 0) {
		char opt[40];
		snprintf(opt, sizeof opt, ",\"max_tokens\":%ld",
		         (long)task->max_tokens);
		ok = llm_buf_str(&body, opt);
	}
	if (ok && task->temperature >= 0.0) {
		char opt[48];
		snprintf(opt, sizeof opt, ",\"temperature\":%.6g",
		         task->temperature);
		ok = llm_buf_str(&body, opt);
	}
	ok = ok && llm_buf_put(&body, "}", 1);
	n = snprintf(head, sizeof head,
	             "POST /v1/chat/completions HTTP/1.1\r\n"
	             "Host: %s:%s\r\n"
	             "Content-Type: application/json\r\n"
	             "Content-Length: %zu\r\n"
	             "Connection: close\r\n\r\n",
	             c->host, c->port, body.size);
	ok = ok && n > 0 && (size_t)n < sizeof head
	  && llm_buf_put(req, head, (size_t)n)
	  && llm_buf_put(req, body.data, body.size);
	llm_buf_free(&body);
	return ok;
}

struct llm *
llm_create (char const *host,
            uint16_t    port)
{
	struct llm *c = calloc(1, sizeof *c);
	if (!c)
		return nullptr;
	c->host = strdup(host && *host ? host : "127.0.0.1");
	snprintf(c->port, sizeof c->port, "%u",
	         port ? port : (uint16_t)8080U);
	c->fd = -1;
	list_init(&c->queue);
	pthread_mutex_init(&c->mtx, nullptr);
	pthread_cond_init(&c->cond, nullptr);
	if (c->host && !pthread_create(&c->worker, nullptr, work, c))
		return c;
	pthread_cond_destroy(&c->cond);
	pthread_mutex_destroy(&c->mtx);
	free(c->host);
	free(c);
	return nullptr;
}

void
llm_destroy (struct llm **p_c)
{
	struct llm *c = p_c ? *p_c : nullptr;
	struct list *n;
	if (!c)
		return;
	*p_c = nullptr;
	pthread_mutex_lock(&c->mtx);
	c->quit = true;
	if (c->active) {
		c->cancel = true;
		if (c->fd >= 0)
			shutdown(c->fd, SHUT_RDWR);
	}
	pthread_cond_signal(&c->cond);
	pthread_mutex_unlock(&c->mtx);
	pthread_join(c->worker, nullptr);
	while ((n = list_head(&c->queue))) {
		struct llm_job *job = container_of(n, struct llm_job, link);
		list_del(n);
		finish(job, LLM_ERR_CANCEL, nullptr);
		free(job);
	}
	pthread_cond_destroy(&c->cond);
	pthread_mutex_destroy(&c->mtx);
	free(c->host);
	free(c);
}

uint64_t
llm_ask (struct llm            *c,
         struct llm_task const *task,
         llm_done_fn            done,
         void                  *ud)
{
	struct llm_job *job;
	int32_t s;
	uint64_t id;
	if (!c || !task || !task->prompt || !done)
		return 0;
	job = calloc(1, sizeof *job);
	if (!job)
		return 0;
	job->done = done;
	job->ud = ud;
	s = task->timeout_s;
	s = s <= 0 ? 300 : s > 86400 ? 86400 : s;
	job->timeout_ms = (int)s * 1000;
	if (!build_request(c, task, &job->req)) {
		llm_buf_free(&job->req);
		free(job);
		return 0;
	}
	pthread_mutex_lock(&c->mtx);
	id = job->id = ++c->last_id;
	list_append(&c->queue, &job->link);
	pthread_cond_signal(&c->cond);
	pthread_mutex_unlock(&c->mtx);
	return id;
}

int
llm_cancel (struct llm *c,
            uint64_t    id)
{
	struct llm_job *job = nullptr;
	struct list *n;
	if (!c || !id)
		return 0;
	pthread_mutex_lock(&c->mtx);
	if (c->active && c->active->id == id) {
		c->cancel = true;
		if (c->fd >= 0)
			shutdown(c->fd, SHUT_RDWR);
		pthread_mutex_unlock(&c->mtx);
		return 1;
	}
	list_foreach(n, &c->queue) {
		struct llm_job *j = container_of(n, struct llm_job, link);
		if (j->id != id)
			continue;
		list_del(n);
		job = j;
		break;
	}
	pthread_mutex_unlock(&c->mtx);
	if (!job)
		return 0;
	finish(job, LLM_ERR_CANCEL, nullptr);
	free(job);
	return 1;
}

char const *
llm_strerror (int status)
{
	static char const *const msg[] = {
		[0]                = "ok",
		[-LLM_ERR_ARGS]    = "invalid arguments",
		[-LLM_ERR_MEM]     = "out of memory",
		[-LLM_ERR_CONNECT] = "cannot reach server",
		[-LLM_ERR_SEND]    = "send failed",
		[-LLM_ERR_RECV]    = "receive failed",
		[-LLM_ERR_TIMEOUT] = "timed out",
		[-LLM_ERR_HTTP]    = "server error",
		[-LLM_ERR_PARSE]   = "malformed reply",
		[-LLM_ERR_CANCEL]  = "cancelled",
	};
	size_t i = (size_t)-status;
	return status <= 0 && i < sizeof msg / sizeof *msg
	       ? msg[i]
	       : "unknown";
}
