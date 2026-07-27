/** @file
 *
 * llm implementation: libcurl transfers driven by an epoll event
 * loop on a private worker thread.  The request body is built whole
 * on the asking thread (fail fast, nothing to copy later); the
 * worker owns every libcurl object and the loop itself, so no lock
 * guards them -- the mutex covers only the queue and the flags that
 * cross the thread boundary.  curl's multi-socket machinery maps
 * onto the loop with two bridges: CURLMOPT_SOCKETFUNCTION turns
 * socket interest into loop watches, CURLMOPT_TIMERFUNCTION arms a
 * timerfd.  An eventfd carries ask/cancel/destroy wakeups in from
 * other threads, and a second timerfd serves the pace gap.  One
 * transfer runs at a time (llama-server serves a single slot by
 * default); the multi's connection cache keeps the server
 * connection warm between tasks.
 *
 * Constraints: plain C23 + POSIX + libcurl + Linux epoll.  No Qt,
 * no C++; the C++ layer consumes llm.h only.
 */
#include <curl/curl.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "list_priv.h"
#include "llm_priv.h"
#include "loop.h"

static constexpr long   kConnectMs = 5000;
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
	[' ' ] = CSEP | CWS, ['\t'] = CSEP | CWS,
	['\n'] = CSEP | CWS, ['\r'] = CSEP | CWS,
	[',' ] = CSEP,       [']' ] = CSEP,
	['}' ] = CSEP,       ['"' ] = CSEP,
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
	if (c->p < c->end && *c->p == '"') {
		while (++c->p < c->end) {
			if (*c->p == '\\') {
				++c->p;
				continue;
			}
			if (*c->p == '"') {
				++c->p;
				return true;
			}
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
		    !__builtin_memcmp(kstart, key, klen))
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
put_utf8 (struct llm_buf *out,
          uint32_t        cp)
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
unescape_u (struct llm_cur *c,
            struct llm_buf *out)
{
	int v;
	if (c->end - c->p < 4 || (v = hex4(c->p)) < 0)
		return false;
	c->p += 4;
	uint32_t cp = (uint32_t)v;
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
unescape (struct llm_cur *c,
          struct llm_buf *out)
{
	if (c->end - c->p < 2)
		return false;
	char ch = c->p[1];
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

/* ---- the client ------------------------------------------------- */

struct llm_job {
	struct list     link;
	CURL           *easy;     /* on the wire; worker-owned */
	llm_done_fn     done;
	void           *ud;
	uint64_t        id;
	struct llm_buf  body;     /* the request JSON */
	struct llm_buf  in;       /* reply bytes as curl delivers them */
	int32_t         timeout_s;
	bool            overflow; /* reply cap tripped in write_cb() */
};

struct llm {
	pthread_mutex_t    mtx;     /* guards the shared block below */
	pthread_t          worker;
	struct list        queue;   /* pending llm_job links */
	struct llm_job    *active;  /* in flight, owned by the worker */
	uint64_t           last_id;
	int32_t            pace_ms; /* quiet gap between tasks */
	bool               cancel;  /* retire the active job */
	bool               quit;
	/* Worker-private from here down: the loop and every libcurl
	 * object are touched by the worker thread alone.  The fds stay
	 * here because loop_unwatch() wipes its ref's copy.
	 */
	bool               pacing;  /* the gap timer is armed */
	int                wakefd;  /* eventfd: ask/cancel/destroy */
	int                ktfd;    /* timerfd: curl's schedule */
	int                ptfd;    /* timerfd: the pace gap */
	struct loop        loop;
	struct loop_ref    wake;
	struct loop_ref    ktimer;
	struct loop_ref    ptimer;
	CURLM             *multi;
	struct curl_slist *hdrs;
	char              *url;
};

/* One watched curl socket; curl_multi_assign() carries it. */
struct llm_sock {
	struct loop_ref  ref;
	struct llm      *c;
};

/* Deliver and release; text (when given) gains a terminator here.
 * A success that cannot afford even the terminator degrades to
 * LLM_ERR_MEM: LLM_OK always carries text, as the header promises.
 */
static void
finish (struct llm_job *job, int status, struct llm_buf *text)
{
	if (text && llm_buf_put(text, "", 1))
		job->done(job->ud, job->id, status, text->data,
		          text->size - 1);
	else
		job->done(job->ud, job->id,
		          status == LLM_OK ? LLM_ERR_MEM : status,
		          nullptr, 0);
	llm_buf_free(&job->body);
}

/* Extraction plus delivery for a job that concluded on the wire. */
static void
conclude (struct llm_job *job, int status)
{
	struct llm_buf out = {};
	/* The cursor exists only over delivered bytes: in C23 even
	 * nullptr + 0 is undefined pointer arithmetic.
	 */
	char const *base = job->in.data ? job->in.data : "";
	struct llm_cur cur = { base, base + job->in.size };
	if (status == LLM_OK)
		status = extract_content(&cur, &out);
	else if (status == LLM_ERR_HTTP)
		status = extract_error(&cur, &out);
	if (status == LLM_OK || (status == LLM_ERR_HTTP && out.size))
		finish(job, status, &out);
	else
		finish(job, status, nullptr);
	llm_buf_free(&job->in);
	llm_buf_free(&out);
	free(job);
}

/* curl's verdict to the callback contract. */
static int
verdict (CURLcode r, long code, bool overflow)
{
	switch (r) {
	case CURLE_OK:
		return code / 100 == 2 ? LLM_OK : LLM_ERR_HTTP;

	case CURLE_COULDNT_RESOLVE_HOST:
	case CURLE_COULDNT_RESOLVE_PROXY:
	case CURLE_COULDNT_CONNECT:
		return LLM_ERR_CONNECT;

	case CURLE_OPERATION_TIMEDOUT:
		/* Always the task timeout: a down server refuses and
		 * lands in CURLE_COULDNT_CONNECT above, and a reused
		 * connection never dials -- discriminating on connect
		 * time would misfile its timeouts as unreachability
		 * and feed the caller's connect-refusal breaker.
		 */
		return LLM_ERR_TIMEOUT;

	case CURLE_SEND_ERROR:
		return LLM_ERR_SEND;

	case CURLE_WRITE_ERROR:
		/* Our own write_cb balked: at the reply cap by policy,
		 * otherwise on allocation failure.
		 */
		return overflow ? LLM_ERR_RECV : LLM_ERR_MEM;

	case CURLE_OUT_OF_MEMORY:
		return LLM_ERR_MEM;

	default:
		return LLM_ERR_RECV;
	}
}

static size_t
write_cb (char *data, size_t size, size_t n, void *ud)
{
	struct llm_job *job = ud;
	size_t total = size * n;
	if (job->in.size + total > kMaxReply) {
		job->overflow = true;
		return 0;
	}
	return llm_buf_put(&job->in, data, total) ? total : 0;
}

/* Arm a timerfd; ms < 0 disarms.  curl's "fire now" is 0, but a
 * zeroed it_value would disarm a timerfd, so now becomes one
 * nanosecond.
 */
static void
arm (int tfd, long ms)
{
	struct itimerspec its = {};
	if (ms > 0) {
		its.it_value.tv_sec = ms / 1000;
		its.it_value.tv_nsec = (ms % 1000) * 1000000L;
	} else if (ms == 0)
		its.it_value.tv_nsec = 1;
	(void)timerfd_settime(tfd, 0, &its, nullptr);
}

/* Clear an eventfd or timerfd; both read as one uint64_t. */
static void
drain (int fd)
{
	uint64_t n;
	while (read(fd, &n, sizeof n) < 0 && errno == EINTR) {}
}

static void
post (int evfd)
{
	uint64_t one = 1;
	while (write(evfd, &one, sizeof one) < 0 && errno == EINTR) {}
}

/* CURLMOPT_TIMERFUNCTION: curl schedules itself through a timerfd. */
static int
timer_cb (CURLM *multi, long ms, void *ud)
{
	struct llm *c = ud;
	(void)multi;
	arm(c->ktfd, ms);
	return 0;
}

static void on_sock (struct loop *loop, struct loop_ref *ref,
                     uint32_t events);

/* CURLMOPT_SOCKETFUNCTION: curl's socket interest becomes a loop
 * watch.  Only curl_multi_assign() may be called back into curl
 * from here.
 */
static int
sock_cb (CURL *easy, curl_socket_t fd, int what, void *ud, void *sp)
{
	static uint32_t const kEv[] = {
		[CURL_POLL_IN]    = EPOLLIN,
		[CURL_POLL_OUT]   = EPOLLOUT,
		[CURL_POLL_INOUT] = EPOLLIN | EPOLLOUT,
	};
	struct llm *c = ud;
	struct llm_sock *s = sp;
	(void)easy;
	if (what == CURL_POLL_REMOVE) {
		if (s) {
			loop_unwatch(&c->loop, &s->ref);
			free(s);
			curl_multi_assign(c->multi, fd, nullptr);
		}
		return 0;
	}
	if (s)
		return loop_modify(&c->loop, &s->ref, kEv[what]) ? -1 : 0;
	s = calloc(1, sizeof *s);
	if (!s)
		return -1;
	s->c = c;
	if (loop_watch(&c->loop, &s->ref, fd, kEv[what], on_sock)) {
		free(s);
		return -1;
	}
	curl_multi_assign(c->multi, fd, s);
	return 0;
}

static void start_next (struct llm *c);

/* The gap or the next task, after a job left the wire (finished,
 * failed or cancelled -- a retired task changes nothing about the
 * accelerator's need to breathe).  On quit the pending wakeup exits
 * the loop instead.
 */
static void
after_job (struct llm *c)
{
	int32_t gap;
	bool quit;
	pthread_mutex_lock(&c->mtx);
	gap = c->pace_ms;
	quit = c->quit;
	pthread_mutex_unlock(&c->mtx);
	if (quit)
		return;
	if (gap > 0) {
		c->pacing = true;
		arm(c->ptfd, gap);
	} else {
		start_next(c);
	}
}

/* Harvest every transfer curl reports done.  The easy handle is
 * torn down before the callback: the connection cache survives in
 * the multi, the handle does not need to.
 */
static void
reap (struct llm *c)
{
	CURLMsg *m;
	int left;
	while ((m = curl_multi_info_read(c->multi, &left))) {
		struct llm_job *job = nullptr;
		long code = 0;
		bool cancelled;
		if (m->msg != CURLMSG_DONE)
			continue;
		CURLcode r = m->data.result;
		CURL *easy = m->easy_handle;
		curl_easy_getinfo(easy, CURLINFO_PRIVATE, &job);
		curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code);
		curl_multi_remove_handle(c->multi, easy);
		curl_easy_cleanup(easy);
		job->easy = nullptr;
		/* Unpublish before the callback runs and the job is
		 * freed: a concurrent cancel of this id must find
		 * nothing to point at.
		 */
		pthread_mutex_lock(&c->mtx);
		cancelled = c->cancel;
		c->cancel = false;
		c->active = nullptr;
		pthread_mutex_unlock(&c->mtx);
		conclude(job, cancelled
		              ? LLM_ERR_CANCEL
		              : verdict(r, code, job->overflow));
		after_job(c);
	}
}

/* Put one job on the wire; false means it never got there. */
static bool
launch (struct llm *c, struct llm_job *job)
{
	CURL *e = curl_easy_init();
	if (!e)
		return false;
	bool ok =
	    !curl_easy_setopt(e, CURLOPT_URL, c->url) &&
	    !curl_easy_setopt(e, CURLOPT_POSTFIELDS, job->body.data) &&
	    !curl_easy_setopt(e, CURLOPT_POSTFIELDSIZE_LARGE,
	                      (curl_off_t)job->body.size) &&
	    !curl_easy_setopt(e, CURLOPT_HTTPHEADER, c->hdrs) &&
	    !curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, write_cb) &&
	    !curl_easy_setopt(e, CURLOPT_WRITEDATA, job) &&
	    !curl_easy_setopt(e, CURLOPT_PRIVATE, job) &&
	    !curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L) &&
	    /* Explicitly no proxy: an inherited http_proxy/ALL_PROXY
	     * environment must never reroute prompt traffic.
	     */
	    !curl_easy_setopt(e, CURLOPT_PROXY, "") &&
	    !curl_easy_setopt(e, CURLOPT_PROTOCOLS_STR, "http") &&
	    !curl_easy_setopt(e, CURLOPT_HTTP_VERSION,
	                      (long)CURL_HTTP_VERSION_1_1) &&
	    !curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT_MS, kConnectMs) &&
	    /* Not CURLOPT_LOW_SPEED_*: those compare a rolling average
	     * that the request upload inflates, so a silent server
	     * after a large POST times out only once the burst decays
	     * below the limit -- hours, for our prompt sizes.  The
	     * reply arrives whole, so a total cap says the same thing
	     * the old per-silence poll did.
	     */
	    !curl_easy_setopt(e, CURLOPT_TIMEOUT_MS,
	                      (long)job->timeout_s * 1000L) &&
	    curl_multi_add_handle(c->multi, e) == CURLM_OK;
	if (!ok) {
		curl_easy_cleanup(e);
		return false;
	}
	job->easy = e;
	return true;
}

/* Start the next queued job unless one is active, the gap is being
 * served, or the client is quitting.  A job that cannot launch
 * reports LLM_ERR_MEM and the next one gets its chance.
 */
static void
start_next (struct llm *c)
{
	while (!c->pacing) {
		struct llm_job *job;
		struct list *n;
		pthread_mutex_lock(&c->mtx);
		if (c->quit || c->active || !(n = list_head(&c->queue))) {
			pthread_mutex_unlock(&c->mtx);
			return;
		}
		list_del(n);
		job = container_of(n, struct llm_job, link);
		c->active = job;
		c->cancel = false;
		pthread_mutex_unlock(&c->mtx);
		if (launch(c, job))
			return;
		pthread_mutex_lock(&c->mtx);
		c->active = nullptr;
		pthread_mutex_unlock(&c->mtx);
		finish(job, LLM_ERR_MEM, nullptr);
		llm_buf_free(&job->in);
		free(job);
	}
}

/* The eventfd handler: asks start work, cancels retire the active
 * transfer, destroy exits the loop.  Wakeups coalesce in the
 * counter; one drain answers them all.
 */
static void
on_wake (struct loop *loop, struct loop_ref *ref, uint32_t events)
{
	struct llm *c = container_of(ref, struct llm, wake);
	struct llm_job *job;
	bool quit;
	(void)loop;
	(void)events;
	drain(ref->fd);
	pthread_mutex_lock(&c->mtx);
	quit = c->quit;
	job = c->cancel ? c->active : nullptr;
	if (job) {
		c->active = nullptr;
		c->cancel = false;
	}
	pthread_mutex_unlock(&c->mtx);
	if (job) {
		/* Removal is silent -- no CURLMSG_DONE follows -- so
		 * the job concludes right here, on the worker.
		 */
		curl_multi_remove_handle(c->multi, job->easy);
		curl_easy_cleanup(job->easy);
		finish(job, LLM_ERR_CANCEL, nullptr);
		llm_buf_free(&job->in);
		free(job);
	}
	if (quit) {
		loop_exit(&c->loop);
		return;
	}
	if (job)
		after_job(c);
	else
		start_next(c);
}

static void
on_ktimer (struct loop *loop, struct loop_ref *ref, uint32_t events)
{
	struct llm *c = container_of(ref, struct llm, ktimer);
	int running;
	(void)loop;
	(void)events;
	drain(ref->fd);
	curl_multi_socket_action(c->multi, CURL_SOCKET_TIMEOUT, 0,
	                         &running);
	reap(c);
}

static void
on_ptimer (struct loop *loop, struct loop_ref *ref, uint32_t events)
{
	struct llm *c = container_of(ref, struct llm, ptimer);
	(void)loop;
	(void)events;
	drain(ref->fd);
	c->pacing = false;
	start_next(c);
}

static void
on_sock (struct loop *loop, struct loop_ref *ref, uint32_t events)
{
	struct llm_sock *s = container_of(ref, struct llm_sock, ref);
	struct llm *c = s->c;
	curl_socket_t fd = ref->fd;
	int bits = (events & EPOLLIN  ? CURL_CSELECT_IN  : 0)
	         | (events & EPOLLOUT ? CURL_CSELECT_OUT : 0)
	         | (events & (EPOLLERR | EPOLLHUP) ? CURL_CSELECT_ERR
	                                           : 0);
	int running;
	(void)loop;
	/* The action call may retire this very socket (CURL_POLL_REMOVE
	 * frees s); nothing after it touches s or ref.
	 */
	curl_multi_socket_action(c->multi, fd, bits, &running);
	reap(c);
}

static void *
work (void *arg)
{
	struct llm *c = arg;
	loop_exec(&c->loop);
	/* Exactly one callback per accepted task even if the loop died
	 * on an epoll failure rather than loop_exit(): retire whatever
	 * was still on the wire.
	 */
	pthread_mutex_lock(&c->mtx);
	struct llm_job *job = c->active;
	c->active = nullptr;
	pthread_mutex_unlock(&c->mtx);
	if (job) {
		curl_multi_remove_handle(c->multi, job->easy);
		curl_easy_cleanup(job->easy);
		finish(job, LLM_ERR_CANCEL, nullptr);
		llm_buf_free(&job->in);
		free(job);
	}
	return nullptr;
}

static bool
build_body (struct llm_task const *task, struct llm_buf *body)
{
	static char const kOpen[] =
		"{\"stream\":false,\"cache_prompt\":true,\"messages\":[";
	bool ok = llm_buf_str(body, kOpen);
	if (ok && task->system && *task->system)
		ok = llm_buf_str(body, "{\"role\":\"system\",\"content\":")
		  && llm_buf_json(body, task->system)
		  && llm_buf_str(body, "},");
	ok = ok
	  && llm_buf_str(body, "{\"role\":\"user\",\"content\":")
	  && llm_buf_json(body, task->prompt)
	  && llm_buf_str(body, "}]");
	if (ok && task->max_tokens > 0) {
		char opt[40];
		snprintf(opt, sizeof opt, ",\"max_tokens\":%ld",
		         (long)task->max_tokens);
		ok = llm_buf_str(body, opt);
	}
	if (ok && task->temperature >= 0.0) {
		char num[32];
		int len = snprintf(num, sizeof num, "%.6g",
		                   task->temperature);
		/* A Qt host has run setlocale(LC_ALL, "") by now: a
		 * comma-decimal locale would print "0,5", which is not
		 * JSON.  Normalize the one character that varies.
		 */
		for (char *p = num; *p; ++p)
			*p = *p == ',' ? '.' : *p;
		ok = len > 0 && (size_t)len < sizeof num
		  && llm_buf_put(body, ",\"temperature\":",
		                 sizeof ",\"temperature\":" - 1)
		  && llm_buf_put(body, num, (size_t)len);
	}
	return ok && llm_buf_put(body, "}", 1);
}

/* The endpoint URL; a host containing ':' is an IPv6 literal and
 * gets its brackets.
 */
static char *
endpoint (char const *host, uint16_t port)
{
	char const *fmt = strchr(host, ':')
	                  ? "http://[%s]:%u/v1/chat/completions"
	                  : "http://%s:%u/v1/chat/completions";
	int n = snprintf(nullptr, 0, fmt, host, port);
	char *url = n > 0 ? malloc((size_t)n + 1) : nullptr;
	if (url)
		snprintf(url, (size_t)n + 1, fmt, host, port);
	return url;
}

struct llm *
llm_create (char const *host,
            uint16_t    port)
{
	if (curl_global_init(CURL_GLOBAL_DEFAULT))
		return nullptr;
	struct llm *c = calloc(1, sizeof *c);
	if (!c)
		goto fail_alloc;
	c->url = endpoint(host && *host ? host : "127.0.0.1",
	                  port ? port : (uint16_t)8080U);
	if (!c->url)
		goto fail_url;
	/* llama-server answers nothing to Expect: 100-continue, and
	 * libcurl would add it for bodies this size -- an empty value
	 * keeps the header out and the body flowing immediately.
	 */
	c->hdrs = curl_slist_append(nullptr,
	                            "Content-Type: application/json");
	if (!c->hdrs || !curl_slist_append(c->hdrs, "Expect:"))
		goto fail_hdrs;
	c->multi = curl_multi_init();
	if (!c->multi ||
	    curl_multi_setopt(c->multi, CURLMOPT_SOCKETFUNCTION,
	                      sock_cb) ||
	    curl_multi_setopt(c->multi, CURLMOPT_SOCKETDATA, c) ||
	    curl_multi_setopt(c->multi, CURLMOPT_TIMERFUNCTION,
	                      timer_cb) ||
	    curl_multi_setopt(c->multi, CURLMOPT_TIMERDATA, c))
		goto fail_multi;
	if (loop_init(&c->loop))
		goto fail_multi;
	c->wakefd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	c->ktfd = timerfd_create(CLOCK_MONOTONIC,
	                         TFD_CLOEXEC | TFD_NONBLOCK);
	c->ptfd = timerfd_create(CLOCK_MONOTONIC,
	                         TFD_CLOEXEC | TFD_NONBLOCK);
	if (c->wakefd < 0 || c->ktfd < 0 || c->ptfd < 0 ||
	    loop_watch(&c->loop, &c->wake, c->wakefd, EPOLLIN,
	               on_wake) ||
	    loop_watch(&c->loop, &c->ktimer, c->ktfd, EPOLLIN,
	               on_ktimer) ||
	    loop_watch(&c->loop, &c->ptimer, c->ptfd, EPOLLIN,
	               on_ptimer))
		goto fail_fds;
	list_init(&c->queue);
	pthread_mutex_init(&c->mtx, nullptr);
	if (!pthread_create(&c->worker, nullptr, work, c))
		return c;
	pthread_mutex_destroy(&c->mtx);
fail_fds:
	if (c->wakefd >= 0)
		close(c->wakefd);
	if (c->ktfd >= 0)
		close(c->ktfd);
	if (c->ptfd >= 0)
		close(c->ptfd);
	loop_fini(&c->loop);
fail_multi:
	if (c->multi)
		curl_multi_cleanup(c->multi);
fail_hdrs:
	curl_slist_free_all(c->hdrs);
fail_url:
	free(c->url);
	free(c);
fail_alloc:
	curl_global_cleanup();
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
	if (c->active)
		c->cancel = true;
	pthread_mutex_unlock(&c->mtx);
	post(c->wakefd);
	pthread_join(c->worker, nullptr);
	while ((n = list_head(&c->queue))) {
		struct llm_job *job = container_of(n, struct llm_job, link);
		list_del(n);
		finish(job, LLM_ERR_CANCEL, nullptr);
		free(job);
	}
	curl_multi_cleanup(c->multi);
	curl_slist_free_all(c->hdrs);
	loop_fini(&c->loop);
	close(c->wakefd);
	close(c->ktfd);
	close(c->ptfd);
	pthread_mutex_destroy(&c->mtx);
	free(c->url);
	free(c);
	curl_global_cleanup();
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
	job->timeout_s = s <= 0 ? 300 : s > 86400 ? 86400 : s;
	if (!build_body(task, &job->body)) {
		llm_buf_free(&job->body);
		free(job);
		return 0;
	}
	pthread_mutex_lock(&c->mtx);
	id = job->id = ++c->last_id;
	list_append(&c->queue, &job->link);
	pthread_mutex_unlock(&c->mtx);
	post(c->wakefd);
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
		pthread_mutex_unlock(&c->mtx);
		post(c->wakefd);
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

void
llm_pace (struct llm *c,
          int32_t     ms)
{
	if (!c)
		return;

	pthread_mutex_lock(&c->mtx);
	c->pace_ms = ms > 0 ? (int)ms : 0;
	pthread_mutex_unlock(&c->mtx);
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
	/* Negate after converting: -INT_MIN is signed overflow. */
	size_t i = (size_t)0 - (size_t)status;
	return status <= 0 && i < sizeof msg / sizeof *msg
	       ? msg[i]
	       : "unknown";
}
