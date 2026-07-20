/** @file
 */
#include <errno.h>
#include <stdint.h> /* PTRDIFF_MAX */
#include <stdlib.h>
#include <string.h>

#include "fundo_priv.h"

/** @brief Allocate a node for at most `size` payload bytes,
 *         optionally initializing said payload from `data`.
 *
 * @param data Payload bytes. May be `nullptr`.
 * @param size Payload byte count. Must be nonzero if `data`
 *             is not `nullptr`.
 * @return     The new node or `nullptr` on failure.
 */
static struct fundo_node *
fundo_node_create (void const *data,
                   size_t      size,
                   int        *error)
{
	struct fundo_node *n = nullptr;
	int e = (data && !size) || size > (size_t)PTRDIFF_MAX - sizeof *n
	        ? EINVAL
	        : 0;

	if (!e) do {
		n = malloc(sizeof *n + size);
		if (!n) {
			e = errno ? errno : ENOMEM;
			break;
		}

		*n = (struct fundo_node){
			.hook = list(&n->hook),
			.children = list(&n->children),
			.ring = list(&n->ring),
			.parent = nullptr,
			.last = nullptr,
			.size = size,
			.trav = {0, -1}
		};

		if (data) {
			memcpy(n->data, data, size);
			break;
		}

		if (size)
			memset(n->data, 0, size);
	} while (0);

	if (error)
		*error = e;

	return n;
}

struct fundo
fundo (void)
{
	int e;
	typeof(fundo().cur) n = fundo_node_create(nullptr, 0, &e);
	return (struct fundo){
		.root  = n,
		.cur   = n,
		.error = e
	};
}

int
fundo_init (struct fundo *dest)
{
	return dest ? (*dest = fundo()).error : EFAULT;
}

/* Iterative teardown driven by the list mechanism itself: descend to
 * a leaf, unhook it, free it, climb.  The foreach-safe ring makes the
 * leaf hunt trivial and no recursion is needed.
 */
static void
fundo_free_tree_ (struct fundo_node **root)
{
	if (root) {
		for (struct fundo_node *n = *root; n; ) {
			struct list *head = list_head(&n->children);
			if (head) {
				n = container_of(head, struct fundo_node, hook);
				continue;
			}
			struct fundo_node *up = n->parent;
			list_del(&n->hook);
			*n = (struct fundo_node){0};
			free(n);
			n = up;
		}

		*root = nullptr;
	}
}

void
fundo_fini (struct fundo *dest)
{
	if (dest) {
		fundo_free_tree_(&dest->root);
		*dest = (struct fundo){0};
	}
}

struct fundo *
fundo_create (void)
{
	struct fundo *ret = malloc(sizeof *ret);
	if (ret)
		*ret = fundo();
	return ret;
}

void
fundo_destroy (struct fundo **p_dest)
{
	if (p_dest && *p_dest) {
		struct fundo *ptr = *p_dest;
		*p_dest = nullptr;
		fundo_fini(ptr);
		free(ptr);
	}
}

int
fundo_act (struct fundo *f,
           void const   *data,
           size_t        size)
{
	if (!f || !f->cur || (data && !size))
		return EFAULT;

	struct fundo_node *hit = fundo_find_child_(f->cur, data, size);
	if (hit) {
		/* Adoption: identical action, descend as if redone. */
		f->cur->last = hit;
		f->cur = hit;
		return 0;
	}

	int e;
	struct fundo_node *n = fundo_node_create(data, size, &e);
	if (!n)
		return e;

	n->parent = f->cur;
	list_append(&f->cur->children, &n->hook);
	f->cur->last = n;
	f->cur->count++;
	f->cur = n;
	return 0;
}

void const *
fundo_undo (struct fundo *f,
            size_t       *size)
{
	if (!f || !f->cur || !f->cur->parent)
		return nullptr;

	f->cur = f->cur->parent;
	if (size)
		*size = f->cur->size;
	return f->cur->data;
}

void const *
fundo_redo (struct fundo *f,
            size_t       *size)
{
	if (!f || !f->cur || !f->cur->last)
		return nullptr;

	f->cur = f->cur->last;
	if (size)
		*size = f->cur->size;
	return f->cur->data;
}

int
fundo_can_undo (struct fundo const *f)
{
	return f && f->cur && f->cur->parent;
}

int
fundo_can_redo (struct fundo const *f)
{
	return f && f->cur && f->cur->last;
}

size_t
fundo_branches (struct fundo const *f)
{
	return f && f->cur ? f->cur->count : 0;
}

struct fundo_node const *
fundo_at (struct fundo const *f)
{
	return f ? f->cur : nullptr;
}

struct fundo_node const *
fundo_up (struct fundo_node const *n)
{
	return n ? n->parent : nullptr;
}

void const *
fundo_data (struct fundo_node const *n,
            size_t                  *size)
{
	if (size)
		*size = n ? n->size : 0;
	return n ? n->data : nullptr;
}

/* Bump a node's travel pair for a pass in direction dir.  A pass
 * against the direction of earlier surplus passes cancels one of
 * them, otherwise it counts a new pass; either way the counter of
 * the latest direction claims index 0.  The stated invariants (one
 * value >= 0, the other <= -1, only one carrying magnitude) follow:
 * p grows only while n == -1, n shrinks only while p == 0, and
 * cancellation stops at the anchors.
 */
static void
fundo_bump_ (int32_t *t, int dir)
{
	int32_t p = t[t[0] < 0];
	int32_t n = t[t[0] >= 0];

	if (dir > 0) {
		if (n < -1)
			++n;
		else
			++p;
		t[0] = p;
		t[1] = n;
		return;
	}
	if (p > 0)
		--p;
	else
		--n;
	t[0] = n;
	t[1] = p;
}

int
fundo_join (struct fundo            *f,
            struct fundo_node const *anchor,
            int                      dir)
{
	if (!f || !f->cur || !anchor)
		return EFAULT;

	struct fundo_node *n = f->cur;
	if (!dir || anchor == n || n->ring.next != &n->ring)
		return EINVAL;

	/* The ring links are owned by the (mutable) tree; anchor is
	 * const only for symmetry with the inspection API.
	 */
	struct list *at = (struct list *)&anchor->ring;
	list_append_(dir > 0 ? at->next : at, &n->ring);
	return 0;
}

void const *
fundo_travel (struct fundo *f,
              int           dir,
              size_t       *size)
{
	if (!f || !f->cur || !dir)
		return nullptr;

	struct list *hop = dir > 0 ? f->cur->ring.next
	                           : f->cur->ring.prev;
	struct fundo_node *n = container_of(hop, struct fundo_node, ring);
	fundo_bump_(n->trav, dir);
	f->cur = n;
	if (size)
		*size = n->size;
	return n->data;
}

struct fundo_node const *
fundo_next (struct fundo_node const *n)
{
	return n && n->ring.next != &n->ring
	       ? container_of(n->ring.next, struct fundo_node, ring)
	       : nullptr;
}

struct fundo_node const *
fundo_prev (struct fundo_node const *n)
{
	return n && n->ring.prev != &n->ring
	       ? container_of(n->ring.prev, struct fundo_node, ring)
	       : nullptr;
}

int
fundo_net (struct fundo_node const *n)
{
	return n ? n->trav[0] + n->trav[1] + 1 : 0;
}

int
fundo_heading (struct fundo_node const *n)
{
	if (!n)
		return 0;
	return n->trav[0] < 0 ? -1 : 1;
}
