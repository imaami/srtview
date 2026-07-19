/** @file
 */
#include <errno.h>
#include <stdlib.h>

#include "fundo_priv.h"

struct fundo
fundo (void)
{
	struct fundo ret = {0};

	ret.root = fundo_node_new_(nullptr, 0);
	if (!ret.root) {
		ret.error = errno ? errno : ENOMEM;
		return ret;
	}
	ret.cur = ret.root;
	return ret;
}

int
fundo_init (struct fundo *dest)
{
	if (!dest)
		return EFAULT;
	*dest = fundo();
	return dest->error;
}

/* Iterative teardown driven by the list mechanism itself: descend to
 * a leaf, unhook it, free it, climb.  The foreach-safe ring makes the
 * leaf hunt trivial and no recursion is needed.
 */
static void
fundo_free_tree_ (struct fundo_node *root)
{
	struct fundo_node *n = root;

	while (n) {
		struct list *head = list_head(&n->children);
		if (head) {
			n = container_of(head, struct fundo_node, hook);
			continue;
		}
		struct fundo_node *up = n->parent;
		list_del(&n->hook);
		free(n);
		n = up;
	}
}

void
fundo_fini (struct fundo *dest)
{
	if (dest) {
		fundo_free_tree_(dest->root);
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
	if (!f || !f->cur || (!data && size))
		return EFAULT;

	struct fundo_node *hit = fundo_find_child_(f->cur, data, size);
	if (hit) {
		/* Adoption: identical action, descend as if redone. */
		f->cur->last = hit;
		f->cur = hit;
		return 0;
	}

	struct fundo_node *n = fundo_node_new_(data, size);
	if (!n)
		return errno ? errno : ENOMEM;
	n->parent = f->cur;
	list_append(&f->cur->children, &n->hook);
	f->cur->last = n;
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
	size_t count = 0;
	struct list *it;

	if (!f || !f->cur)
		return 0;
	list_foreach (it, &f->cur->children)
		++count;
	return count;
}
