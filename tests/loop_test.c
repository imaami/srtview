/** @file
 *
 * Unit tests for the epoll dispatcher: handler dispatch with
 * container_of ownership, interest modification, unwatch, exit
 * semantics, and the safety property the one-event-per-wait design
 * buys -- a handler freeing another owner's watch mid-round.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "cutil.h"
#include "loop.h"

static int g_fail;

static void
check (bool ok, char const *what)
{
	printf("%s  %s\n", ok ? "OK  " : "FAIL", what);
	if (!ok)
		++g_fail;
}

static void
fire (int evfd)
{
	uint64_t one = 1;
	(void)!write(evfd, &one, sizeof one);
}

static void
clear (int evfd)
{
	uint64_t n;
	(void)!read(evfd, &n, sizeof n);
}

/* ---- dispatch and ownership ------------------------------------- */

struct counter {
	struct loop_ref ref;
	int             fires;
	uint32_t        events;
};

static void
on_count (struct loop *loop, struct loop_ref *ref, uint32_t events)
{
	struct counter *t = container_of(ref, struct counter, ref);
	++t->fires;
	t->events = events;
	clear(ref->fd);
	loop_exit(loop);
}

static void
test_dispatch (void)
{
	struct loop loop;
	struct counter t = {};
	int evfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	check(!loop_init(&loop) && evfd >= 0, "loop and eventfd up");
	check(!loop_watch(&loop, &t.ref, evfd, EPOLLIN, on_count),
	      "watch accepted");
	check(loop_watch(&loop, &(struct loop_ref){}, -1, EPOLLIN,
	                 on_count) == EINVAL,
	      "negative fd refused");
	fire(evfd);
	check(!loop_exec(&loop), "exec returns 0 after loop_exit");
	check(t.fires == 1 && (t.events & EPOLLIN),
	      "handler fired once with EPOLLIN, owner via container_of");
	fire(evfd);
	check(!loop_exec(&loop) && t.fires == 2,
	      "loop is reusable after exit");
	loop_unwatch(&loop, &t.ref);
	check(t.ref.fd == -1, "unwatch deactivates the ref");
	loop_unwatch(&loop, &t.ref);
	check(t.ref.fd == -1, "unwatch is idempotent");
	loop_fini(&loop);
	check(loop.epfd == -1, "fini closes the epoll instance");
	close(evfd);
}

static void
test_modify (void)
{
	struct loop loop;
	struct counter t = {};
	int evfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	check(!loop_init(&loop) && evfd >= 0, "loop and eventfd up");
	/* An empty eventfd is writable but not readable: EPOLLOUT
	 * fires immediately, EPOLLIN only after a post.
	 */
	check(!loop_watch(&loop, &t.ref, evfd, EPOLLOUT, on_count),
	      "watch for output");
	check(!loop_exec(&loop) && t.fires == 1 &&
	      (t.events & EPOLLOUT),
	      "writable side reported");
	check(!loop_modify(&loop, &t.ref, EPOLLOUT), "same mask no-op");
	check(!loop_modify(&loop, &t.ref, EPOLLIN), "mask change");
	fire(evfd);
	check(!loop_exec(&loop) && t.fires == 2 &&
	      (t.events & EPOLLIN),
	      "readable side reported after modify");
	loop_fini(&loop);
	check(loop_modify(&loop, &t.ref, EPOLLOUT) == EINVAL,
	      "modify refused after fini deactivated the watch");
	close(evfd);
}

/* ---- cross-watch teardown --------------------------------------- */

struct rival {
	struct loop_ref   ref;
	struct rival     *other;  /* freed by whoever fires first */
	struct rival    **winner; /* the survivor reports here */
	int               evfd;
};

static void
on_rival (struct loop *loop, struct loop_ref *ref, uint32_t events)
{
	struct rival *r = container_of(ref, struct rival, ref);
	(void)events;
	clear(ref->fd);
	if (r->other) {
		/* The loser's event may already be pending in the
		 * kernel; one event per wait means it is re-read after
		 * this unwatch, never dispatched stale.
		 */
		struct rival *dead = r->other;
		r->other = nullptr;
		loop_unwatch(loop, &dead->ref);
		close(dead->evfd);
		free(dead);
	}
	*r->winner = r;
	loop_exit(loop);
}

static void
test_rival_teardown (void)
{
	struct loop loop;
	struct rival *winner = nullptr;
	struct rival *a = calloc(1, sizeof *a);
	struct rival *b = calloc(1, sizeof *b);
	check(a && b && !loop_init(&loop), "rivals allocated, loop up");
	a->evfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	b->evfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	a->other = b;
	b->other = a;
	a->winner = b->winner = &winner;
	check(!loop_watch(&loop, &a->ref, a->evfd, EPOLLIN, on_rival) &&
	      !loop_watch(&loop, &b->ref, b->evfd, EPOLLIN, on_rival),
	      "both rivals watched");
	fire(a->evfd);
	fire(b->evfd);
	check(!loop_exec(&loop), "exec survives a mid-round teardown");
	check(winner && !winner->other,
	      "exactly one handler ran and it retired its rival");
	if (winner) {
		loop_unwatch(&loop, &winner->ref);
		close(winner->evfd);
		free(winner);
	}
	loop_fini(&loop);
}

int
main (void)
{
	test_dispatch();
	test_modify();
	test_rival_teardown();
	printf("%s\n", g_fail ? "FAILURES" : "all passed");
	return g_fail ? 1 : 0;
}
