/** @file
 *
 * Unit tests for the llm client: JSON escaping and extraction, HTTP
 * response parsing (content-length, chunked, close-delimited), and
 * full round-trips against a fake in-process server -- success,
 * server error, refused connection, timeout, cancellation, FIFO
 * order.  Optionally (LLM_TEST_LIVE=1) one live round-trip against a
 * real llama-server on 127.0.0.1:8080.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "llm_priv.h"

static int g_fail;

static void
check (bool ok, char const *what)
{
	printf("%s  %s\n", ok ? "OK  " : "FAIL", what);
	if (!ok)
		++g_fail;
}

static bool
buf_is (struct llm_buf const *b, char const *want)
{
	return b->size == strlen(want)
	    && (!b->size || !memcmp(b->data, want, b->size));
}

/* ---- pure protocol helpers ------------------------------------- */

static void
test_json_escape (void)
{
	struct llm_buf b = {};
	check(llm_buf_json(&b, "a\"b\\c\nd\x01\xc3\xa9"),
	      "escape builds");
	check(buf_is(&b, "\"a\\\"b\\\\c\\nd\\u0001\xc3\xa9\""),
	      "quote, backslash, newline, control escaped; UTF-8 raw");
	llm_buf_free(&b);
	check(llm_buf_json(&b, ""), "empty string escapes");
	check(buf_is(&b, "\"\""), "empty string is bare quotes");
	llm_buf_free(&b);
}

static void
test_json_unescape (void)
{
	char const *raw = " \"A \\u0041 \\ud83d\\ude00 \\t\\\\\" ";
	struct llm_cur c = { raw, raw + strlen(raw) };
	struct llm_buf b = {};
	check(llm_json_str(&c, &b), "unescape parses");
	check(buf_is(&b, "A A \xf0\x9f\x98\x80 \t\\"),
	      "named, \\u and surrogate-pair escapes decode");
	llm_buf_free(&b);

	raw = "\"bad \\x\"";
	c = (struct llm_cur){ raw, raw + strlen(raw) };
	check(!llm_json_str(&c, &b), "unknown escape rejected");
	llm_buf_free(&b);

	raw = "\"lone \\udc00 low\"";
	c = (struct llm_cur){ raw, raw + strlen(raw) };
	check(!llm_json_str(&c, &b), "lone low surrogate rejected");
	llm_buf_free(&b);
}

static void
test_json_extract (void)
{
	char const *doc =
		"{\"created\":1,\"object\":\"x\",\"choices\":[{\"index\":0,"
		"\"skip\":[{\"deep\":\"[{\\\"not json\\\"\"}],"
		"\"message\":{\"role\":\"assistant\","
		"\"content\":\"He said \\\"hi\\\".\"}}],\"usage\":{}}";
	struct llm_cur c = { doc, doc + strlen(doc) };
	struct llm_buf b = {};
	check(llm_json_get(&c, "choices") && llm_json_first(&c) &&
	      llm_json_get(&c, "message") && llm_json_get(&c, "content"),
	      "walks choices[0].message.content past decoys");
	check(llm_json_str(&c, &b) && buf_is(&b, "He said \"hi\"."),
	      "content extracted and unescaped");
	llm_buf_free(&b);

	c = (struct llm_cur){ doc, doc + strlen(doc) };
	check(!llm_json_get(&c, "missing"), "absent key is not found");
}

static void
test_http_parse (void)
{
	char const *cl =
		"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
		"Content-Length: 5\r\n\r\nhellotrailing-junk";
	char const *chunked =
		"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
		"5;ext=1\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
	char const *closed =
		"HTTP/1.0 500 Oops\r\nX: y\r\n\r\nsad body";
	struct llm_buf dec = {};
	char const *body = nullptr;
	size_t n = 0;

	check(llm_http_parse(cl, strlen(cl), &dec, &body, &n) == 200 &&
	      n == 5 && !memcmp(body, "hello", 5),
	      "content-length body, extra bytes trimmed");
	check(llm_http_parse(chunked, strlen(chunked), &dec, &body,
	                     &n) == 200 &&
	      n == 11 && !memcmp(body, "hello world", 11),
	      "chunked body reassembled, extension ignored");
	llm_buf_free(&dec);
	check(llm_http_parse(closed, strlen(closed), &dec, &body,
	                     &n) == 500 &&
	      n == 8 && !memcmp(body, "sad body", 8),
	      "close-delimited body, non-2xx status returned");
	char const *cut =
		"HTTP/1.1 200 OK\r\nContent-Length: 99\r\n\r\nshort";
	check(llm_http_parse("junk", 4, &dec, &body, &n) ==
	      LLM_ERR_PARSE, "garbage rejected");
	check(llm_http_parse(cut, strlen(cut), &dec, &body, &n) ==
	      LLM_ERR_PARSE, "truncated body rejected");
	llm_buf_free(&dec);
}

/* ---- fake server ------------------------------------------------ */

struct fake {
	pthread_t       thread;
	int             lfd;
	uint16_t        port;
	char const     *resp;   /* nullptr: read to EOF, answer nothing */
	int             conns;
	struct llm_buf  reqs;   /* every captured request, concatenated */
};

/* One connection: swallow the request (headers plus Content-Length
 * body -- or up to EOF for the stall case), maybe respond, close.
 */
static void
fake_serve (struct fake *f, int fd)
{
	char tmp[8192];
	size_t got = 0, want = SIZE_MAX;
	for (;;) {
		ssize_t r = recv(fd, tmp, sizeof tmp, 0);
		if (r <= 0)
			break;
		if (!llm_buf_put(&f->reqs, tmp, (size_t)r))
			break;
		got += (size_t)r;
		if (want == SIZE_MAX && f->resp) {
			struct llm_buf *b = &f->reqs;
			char const *he, *cl;
			llm_buf_put(b, "", 1);
			--b->size;
			he = strstr(b->data + (b->size - got),
			            "\r\n\r\n");
			cl = strstr(b->data + (b->size - got),
			            "Content-Length: ");
			if (he && cl)
				want = (size_t)(he + 4 -
				                (b->data + b->size - got))
				     + (size_t)atol(cl + 16);
		}
		if (got >= want)
			break;
	}
	if (f->resp)
		(void)!write(fd, f->resp, strlen(f->resp));
	close(fd);
}

static void *
fake_run (void *arg)
{
	struct fake *f = arg;
	for (int i = 0; i < f->conns; ++i) {
		int fd = accept(f->lfd, nullptr, nullptr);
		if (fd < 0)
			break;
		fake_serve(f, fd);
	}
	return nullptr;
}

static bool
fake_start (struct fake *f, char const *resp, int conns)
{
	struct sockaddr_in a = {
		.sin_family = AF_INET,
		.sin_addr   = { htonl(INADDR_LOOPBACK) },
	};
	socklen_t len = sizeof a;
	*f = (struct fake){ .resp = resp, .conns = conns };
	f->lfd = socket(AF_INET, SOCK_STREAM, 0);
	if (f->lfd < 0)
		return false;
	if (bind(f->lfd, (struct sockaddr *)&a, sizeof a) ||
	    listen(f->lfd, 4) ||
	    getsockname(f->lfd, (struct sockaddr *)&a, &len) ||
	    pthread_create(&f->thread, nullptr, fake_run, f)) {
		close(f->lfd);
		return false;
	}
	f->port = ntohs(a.sin_port);
	return true;
}

/* Unstick the accept loop: a client that never dialed (cancelled
 * before its turn) would otherwise leave the join below waiting.
 * Surplus pokes land in the backlog and drain as instant EOFs.
 */
static void
fake_poke (struct fake const *f)
{
	struct sockaddr_in a = {
		.sin_family = AF_INET,
		.sin_port   = htons(f->port),
		.sin_addr   = { htonl(INADDR_LOOPBACK) },
	};
	for (int i = 0; i < f->conns; ++i) {
		int fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0)
			continue;
		(void)!connect(fd, (struct sockaddr *)&a, sizeof a);
		close(fd);
	}
}

static char const *
fake_finish (struct fake *f)
{
	fake_poke(f);
	pthread_join(f->thread, nullptr);
	close(f->lfd);
	llm_buf_put(&f->reqs, "", 1);
	return f->reqs.data ? f->reqs.data : "";
}

/* ---- callback capture ------------------------------------------- */

static pthread_mutex_t g_seq_mtx = PTHREAD_MUTEX_INITIALIZER;
static int             g_seq;

struct slot {
	pthread_mutex_t mtx;
	pthread_cond_t  cond;
	int             fired;
	int             seq;
	int             status;
	uint64_t        id;
	size_t          size;
	char            text[4096];
};

static void
slot_init (struct slot *s)
{
	*s = (struct slot){};
	pthread_mutex_init(&s->mtx, nullptr);
	pthread_cond_init(&s->cond, nullptr);
}

static void
on_done (void *ud, uint64_t id, int status, char const *text,
         size_t size)
{
	struct slot *s = ud;
	pthread_mutex_lock(&g_seq_mtx);
	int seq = ++g_seq;
	pthread_mutex_unlock(&g_seq_mtx);
	pthread_mutex_lock(&s->mtx);
	s->id = id;
	s->seq = seq;
	s->status = status;
	s->size = size < sizeof s->text - 1 ? size : sizeof s->text - 1;
	if (text)
		memcpy(s->text, text, s->size);
	s->text[s->size] = 0;
	s->fired = 1;
	pthread_cond_signal(&s->cond);
	pthread_mutex_unlock(&s->mtx);
}

static bool
slot_wait (struct slot *s, int ms)
{
	struct timespec ts;
	bool ok;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += ms / 1000;
	ts.tv_nsec += (long)(ms % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L) {
		++ts.tv_sec;
		ts.tv_nsec -= 1000000000L;
	}
	pthread_mutex_lock(&s->mtx);
	while (!s->fired)
		if (pthread_cond_timedwait(&s->cond, &s->mtx, &ts))
			break;
	ok = s->fired;
	pthread_mutex_unlock(&s->mtx);
	return ok;
}

/* ---- round trips ------------------------------------------------ */

#define REPLY_JSON \
	"{\"choices\":[{\"finish_reason\":\"stop\",\"index\":0," \
	"\"message\":{\"role\":\"assistant\"," \
	"\"content\":\"Hi from fake\"}}],\"model\":\"gemma\"," \
	"\"object\":\"chat.completion\"}"

static void
test_round_trip (void)
{
	struct llm_task const ask = {
		.system     = "be \"brief\"",
		.prompt     = "say hi\n",
		.max_tokens = 32,
	};
	char resp[1024];
	struct fake f;
	struct slot s;
	struct llm *c;
	char const *reqs;
	snprintf(resp, sizeof resp,
	         "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
	         "Content-Length: %zu\r\n\r\n%s",
	         strlen(REPLY_JSON), REPLY_JSON);
	check(fake_start(&f, resp, 1), "fake server up");
	slot_init(&s);
	c = llm_create(nullptr, f.port);
	check(c != nullptr, "client up");
	check(llm_ask(c, &ask, on_done, &s) != 0, "task queued");
	check(slot_wait(&s, 5000), "callback fired");
	check(s.status == LLM_OK, "status ok");
	check(!strcmp(s.text, "Hi from fake") && s.size == 12,
	      "reply text extracted");
	llm_destroy(&c);
	check(c == nullptr, "destroy clears the pointer");
	reqs = fake_finish(&f);
	check(strstr(reqs, "POST /v1/chat/completions HTTP/1.1")
	      == reqs, "request line");
	check(strstr(reqs, "\"stream\":false") &&
	      strstr(reqs, "\"max_tokens\":32") &&
	      strstr(reqs, "\"temperature\":0"),
	      "options serialized; zero-init means greedy");
	check(strstr(reqs, "{\"role\":\"system\","
	                   "\"content\":\"be \\\"brief\\\"\"},"
	                   "{\"role\":\"user\","
	                   "\"content\":\"say hi\\n\"}"),
	      "messages serialized and escaped");
	llm_buf_free(&f.reqs);
}

static void
test_chunked_trip (void)
{
	char const *resp =
		"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
		"10\r\n{\"choices\":[{\"me\r\n"
		"26\r\nssage\":{\"content\":\"chunky\"}}],\"obj\":1}\r\n"
		"0\r\n\r\n";
	struct fake f;
	struct slot s;
	struct llm *c;
	check(fake_start(&f, resp, 1), "chunked fake up");
	slot_init(&s);
	c = llm_create("localhost", f.port);
	check(c != nullptr, "client up");
	check(llm_ask(c, &(struct llm_task){ .prompt = "x" },
	              on_done, &s) != 0, "task queued");
	check(slot_wait(&s, 5000) && s.status == LLM_OK &&
	      !strcmp(s.text, "chunky"),
	      "chunked reply reassembled and extracted");
	llm_destroy(&c);
	fake_finish(&f);
	llm_buf_free(&f.reqs);
}

static void
test_http_error (void)
{
	char const *err_json =
		"{\"error\":{\"code\":500,\"message\":\"server exploded\","
		"\"type\":\"server_error\"}}";
	char resp[512];
	struct fake f;
	struct slot s;
	struct llm *c;
	snprintf(resp, sizeof resp,
	         "HTTP/1.1 500 Internal Server Error\r\n"
	         "Content-Length: %zu\r\n\r\n%s",
	         strlen(err_json), err_json);
	check(fake_start(&f, resp, 1), "erroring fake up");
	slot_init(&s);
	c = llm_create(nullptr, f.port);
	check(c != nullptr, "client up");
	check(llm_ask(c, &(struct llm_task){ .prompt = "x" },
	              on_done, &s) != 0, "task queued");
	check(slot_wait(&s, 5000) && s.status == LLM_ERR_HTTP,
	      "non-2xx reported as LLM_ERR_HTTP");
	check(!strcmp(s.text, "server exploded"),
	      "server's message extracted");
	llm_destroy(&c);
	fake_finish(&f);
	llm_buf_free(&f.reqs);
}

static void
test_refused (void)
{
	struct sockaddr_in a = {
		.sin_family = AF_INET,
		.sin_addr   = { htonl(INADDR_LOOPBACK) },
	};
	socklen_t len = sizeof a;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	struct slot s;
	struct llm *c;
	uint16_t port = 1;
	if (fd >= 0 && !bind(fd, (struct sockaddr *)&a, sizeof a) &&
	    !getsockname(fd, (struct sockaddr *)&a, &len))
		port = ntohs(a.sin_port);
	if (fd >= 0)
		close(fd);      /* free the port again: nobody listens */
	slot_init(&s);
	c = llm_create(nullptr, port);
	check(c != nullptr, "client up");
	check(llm_ask(c, &(struct llm_task){ .prompt = "x" },
	              on_done, &s) != 0, "task queued");
	check(slot_wait(&s, 5000) && s.status == LLM_ERR_CONNECT,
	      "refused connection reported as LLM_ERR_CONNECT");
	llm_destroy(&c);
}

static void
test_timeout (void)
{
	struct llm_task const ask = { .prompt = "x", .timeout_s = 1 };
	struct fake f;
	struct slot s;
	struct llm *c;
	check(fake_start(&f, nullptr, 1), "stalling fake up");
	slot_init(&s);
	c = llm_create(nullptr, f.port);
	check(c != nullptr, "client up");
	check(llm_ask(c, &ask, on_done, &s) != 0, "task queued");
	check(slot_wait(&s, 5000) && s.status == LLM_ERR_TIMEOUT,
	      "silent server reported as LLM_ERR_TIMEOUT");
	llm_destroy(&c);
	fake_finish(&f);
	llm_buf_free(&f.reqs);
}

static void
test_cancel (void)
{
	struct fake f;
	struct slot s1, s2;
	struct llm *c;
	uint64_t id1, id2;
	check(fake_start(&f, nullptr, 1), "stalling fake up");
	slot_init(&s1);
	slot_init(&s2);
	c = llm_create(nullptr, f.port);
	check(c != nullptr, "client up");
	id1 = llm_ask(c, &(struct llm_task){ .prompt = "one" },
	              on_done, &s1);
	id2 = llm_ask(c, &(struct llm_task){ .prompt = "two" },
	              on_done, &s2);
	check(id1 && id2 && id1 != id2, "two tasks queued");
	check(llm_cancel(c, id2) == 1 && s2.fired &&
	      s2.status == LLM_ERR_CANCEL,
	      "queued task retires synchronously");
	check(llm_cancel(c, id2) == 0, "retired id is unknown");
	check(llm_cancel(c, id1) == 1, "in-flight task found");
	check(slot_wait(&s1, 5000) && s1.status == LLM_ERR_CANCEL,
	      "in-flight task reports LLM_ERR_CANCEL from the worker");
	llm_destroy(&c);
	fake_finish(&f);
	llm_buf_free(&f.reqs);
}

static void
test_fifo (void)
{
	char resp[1024];
	struct fake f;
	struct slot s1, s2;
	struct llm *c;
	uint64_t id1, id2;
	snprintf(resp, sizeof resp,
	         "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n%s",
	         strlen(REPLY_JSON), REPLY_JSON);
	check(fake_start(&f, resp, 2), "two-connection fake up");
	slot_init(&s1);
	slot_init(&s2);
	c = llm_create(nullptr, f.port);
	check(c != nullptr, "client up");
	id1 = llm_ask(c, &(struct llm_task){ .prompt = "first" },
	              on_done, &s1);
	id2 = llm_ask(c, &(struct llm_task){ .prompt = "second" },
	              on_done, &s2);
	check(id1 && id1 < id2, "ids ascend");
	check(slot_wait(&s1, 5000) && slot_wait(&s2, 5000) &&
	      s1.status == LLM_OK && s2.status == LLM_OK,
	      "both tasks complete");
	check(s1.seq < s2.seq, "completion order is FIFO");
	llm_destroy(&c);
	fake_finish(&f);
	llm_buf_free(&f.reqs);
}

/* One real round-trip when a llama-server is up and opted into. */
static void
test_live (void)
{
	/* A reasoning model spends tokens thinking before any of them
	 * land in content; the cap must cover both.
	 */
	struct llm_task const ask = {
		.system     = "Answer with the single word asked for, "
		              "nothing else.",
		.prompt     = "Say: pong",
		.max_tokens = 512,
		.timeout_s  = 300,
	};
	struct slot s;
	struct llm *c;
	char const *env = getenv("LLM_TEST_LIVE");
	if (!env || !*env || *env == '0')
		return;
	slot_init(&s);
	c = llm_create(nullptr, 0);
	check(c != nullptr, "client up");
	check(llm_ask(c, &ask, on_done, &s) != 0, "live task queued");
	check(slot_wait(&s, 300000) && s.status == LLM_OK && s.size,
	      "live server answered");
	printf("      live reply: %s\n", s.text);
	llm_destroy(&c);
}

int
main (void)
{
	/* The fake's writes can race a client that already hung up;
	 * the client side uses MSG_NOSIGNAL on its own.
	 */
	signal(SIGPIPE, SIG_IGN);
	test_json_escape();
	test_json_unescape();
	test_json_extract();
	test_http_parse();
	test_round_trip();
	test_chunked_trip();
	test_http_error();
	test_refused();
	test_timeout();
	test_cancel();
	test_fifo();
	test_live();
	printf("%s\n", g_fail ? "FAILURES" : "all passed");
	return g_fail ? 1 : 0;
}
