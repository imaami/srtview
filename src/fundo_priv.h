/** @file
 */
#ifndef SRTVIEW_SRC_FUNDO_PRIV_H_
#define SRTVIEW_SRC_FUNDO_PRIV_H_

#include "cutil.h"
#include "fundo.h"
#include "list_priv.h"

/** @brief One recorded action: a node in the undo tree.
 *
 * The tree is built from the same circular sentinel lists as
 * everything else: @a children is the sentinel of this node's child
 * ring, and @a hook is this node's link in its parent's ring.
 */
struct fundo_node
{
	struct list        hook;     //!< Sibling link in the parent's ring.
	struct list        children; //!< Sentinel of the child ring.
	struct fundo_node *parent;   //!< Toward the past; root: nullptr.
	struct fundo_node *last;     //!< Redo direction; may be nullptr.
	size_t             size;     //!< Payload byte count.
	unsigned char      data[];   //!< The opaque action payload.
};

/** @brief Allocate a node adopting @a size payload bytes from @a data.
 *
 * @param data Payload bytes, may be nullptr when @a size is 0.
 * @param size Payload byte count.
 * @return     The new node or nullptr on allocation failure.
 */
force_inline struct fundo_node *
fundo_node_new_ (void const *data,
                 size_t      size)
{
	struct fundo_node *n = malloc(sizeof *n + size);
	if (!n)
		return nullptr;
	list_init(&n->hook);
	list_init(&n->children);
	n->parent = nullptr;
	n->last = nullptr;
	n->size = size;
	if (size)
		memcpy(n->data, data, size);
	return n;
}

/** @brief Find a child of @a parent with a byte-identical payload.
 *
 * @param parent The node whose child ring to search.
 * @param data   Payload bytes to match.
 * @param size   Payload byte count to match.
 * @return       The matching child or nullptr.
 */
force_inline struct fundo_node *
fundo_find_child_ (struct fundo_node const *parent,
                   void const              *data,
                   size_t                   size)
{
	struct list *it;

	list_foreach (it, &parent->children) {
		struct fundo_node *c =
			container_of(it, struct fundo_node, hook);
		if (mem_same(c->data, data, c->size, size))
			return c;
	}
	return nullptr;
}

#endif /* SRTVIEW_SRC_FUNDO_PRIV_H_ */
