/** @file
 *
 * Minimal epoll dispatcher for a single-threaded event loop.
 *
 * A watch is an intrusive, caller-owned struct loop_ref: embed it in
 * the owning object and recover the owner with container_of() in the
 * handler.  Watches are level-triggered.  loop_exec() dispatches one
 * event per epoll_wait() round by design: a handler may unwatch and
 * free any watch -- its own or another's -- and the loop never holds
 * a reference that a handler could have invalidated.  (A batched
 * variant must track exactly that; see mcp2210-proxy's wakeup skip
 * masks.)  At this module's intended scale, a handful of
 * descriptors, the extra wakeups are noise.
 *
 * The loop owns none of the descriptors it watches: the caller opens
 * and closes them.  Plain C23 + Linux epoll; no allocation, no
 * threads of its own.  No Qt, no C++.
 */
#ifndef SRTVIEW_SRC_LOOP_H_
#define SRTVIEW_SRC_LOOP_H_

#include <stdint.h>

#include "list_priv.h"

struct loop;
struct loop_ref;

/** @brief Event handler; @a events is the epoll event mask. */
typedef void (loop_fn) (struct loop     *loop,
                        struct loop_ref *ref,
                        uint32_t         events);

/** @brief One watched descriptor, embedded in its owner. */
struct loop_ref {
	struct list  hook;   //!< Loop's watch list linkage.
	loop_fn     *fn;     //!< Event handler.
	uint32_t     events; //!< Current epoll interest mask.
	int          fd;     //!< Watched descriptor; -1 when inactive.
};

/** @brief The loop; zero state before loop_init(). */
struct loop {
	struct list refs; //!< Active watches.
	int         epfd; //!< The epoll instance.
	bool        stop; //!< loop_exit() latch; exec returns on it.
};

/** @brief Initializes a loop.
 *
 * @param loop The loop.
 * @return     0 on success, errno on failure.
 */
extern int
loop_init (struct loop *loop);

/** @brief Unwatches everything and closes the epoll instance.
 *
 * Watched descriptors themselves stay open: they belong to their
 * owners.
 *
 * @param loop The loop.
 */
extern void
loop_fini (struct loop *loop);

/** @brief Starts watching a descriptor.
 *
 * @param loop   The loop.
 * @param ref    The caller-owned watch to initialize and hook up.
 * @param fd     The descriptor.
 * @param events The epoll interest mask (EPOLLIN, EPOLLOUT, ...).
 * @param fn     The handler; required.
 * @return       0 on success, errno on failure.
 */
extern int
loop_watch (struct loop     *loop,
            struct loop_ref *ref,
            int              fd,
            uint32_t         events,
            loop_fn         *fn);

/** @brief Changes a watch's interest mask.
 *
 * @param loop   The loop.
 * @param ref    An active watch.
 * @param events The new mask; an unchanged mask is a no-op.
 * @return       0 on success, errno on failure.
 */
extern int
loop_modify (struct loop     *loop,
             struct loop_ref *ref,
             uint32_t         events);

/** @brief Stops watching; safe on an inactive watch.
 *
 * @param loop The loop.
 * @param ref  The watch; left inactive (fd -1).
 */
extern void
loop_unwatch (struct loop     *loop,
              struct loop_ref *ref);

/** @brief Runs the loop until loop_exit() or an epoll failure.
 *
 * @param loop The loop.
 * @return     0 after loop_exit(), errno on failure.
 */
extern int
loop_exec (struct loop *loop);

/** @brief Makes loop_exec() return after the current handler. */
extern void
loop_exit (struct loop *loop);

#endif /* SRTVIEW_SRC_LOOP_H_ */
