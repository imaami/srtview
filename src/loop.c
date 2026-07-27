/** @file
 *
 * loop implementation: a thin, deliberately unbatched epoll wrapper.
 * See loop.h for the one-event-per-wait rationale.
 */
#include <errno.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "cutil.h"
#include "loop.h"

int
loop_init (struct loop *loop)
{
	int epfd = epoll_create1(EPOLL_CLOEXEC);
	if (epfd < 0)
		return errno;
	*loop = (struct loop){ .epfd = epfd };
	list_init(&loop->refs);
	return 0;
}

void
loop_fini (struct loop *loop)
{
	struct list *n;
	while ((n = list_head(&loop->refs)))
		loop_unwatch(loop, container_of(n, struct loop_ref, hook));
	if (loop->epfd >= 0)
		close(loop->epfd);
	loop->epfd = -1;
	loop->stop = false;
}

int
loop_watch (struct loop     *loop,
            struct loop_ref *ref,
            int              fd,
            uint32_t         events,
            loop_fn         *fn)
{
	if (!fn || fd < 0)
		return EINVAL;
	*ref = (struct loop_ref){
		.fn     = fn,
		.events = events,
		.fd     = fd,
	};
	struct epoll_event ev = { .events = events, .data.ptr = ref };
	if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, fd, &ev)) {
		ref->fd = -1;
		return errno;
	}
	list_append(&loop->refs, &ref->hook);
	return 0;
}

int
loop_modify (struct loop     *loop,
             struct loop_ref *ref,
             uint32_t         events)
{
	if (ref->fd < 0)
		return EINVAL;
	if (ref->events == events)
		return 0;
	struct epoll_event ev = { .events = events, .data.ptr = ref };
	if (epoll_ctl(loop->epfd, EPOLL_CTL_MOD, ref->fd, &ev))
		return errno;
	ref->events = events;
	return 0;
}

void
loop_unwatch (struct loop     *loop,
              struct loop_ref *ref)
{
	if (ref->fd < 0)
		return;
	(void)epoll_ctl(loop->epfd, EPOLL_CTL_DEL, ref->fd, nullptr);
	list_del(&ref->hook);
	ref->fd = -1;
}

int
loop_exec (struct loop *loop)
{
	while (!loop->stop) {
		struct epoll_event ev;
		int n = epoll_wait(loop->epfd, &ev, 1, -1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return errno;
		}
		if (n > 0) {
			struct loop_ref *ref = ev.data.ptr;
			ref->fn(loop, ref, ev.events);
		}
	}
	loop->stop = false;
	return 0;
}

void
loop_exit (struct loop *loop)
{
	loop->stop = true;
}
