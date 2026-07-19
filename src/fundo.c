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
			.parent = nullptr,
			.last = nullptr,
			.size = size
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

	struct fundo_node *departed = f->cur;
	f->cur = departed->parent;
	if (size)
		*size = departed->size;
	return departed->data;
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
