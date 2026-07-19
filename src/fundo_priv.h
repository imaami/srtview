/** @file
 */
#ifndef SRTVIEW_SRC_FUNDO_PRIV_H_
#define SRTVIEW_SRC_FUNDO_PRIV_H_

#include <stdint.h> /* SIZE_MAX */

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
	size_t             count;    //!< Child count
	size_t             size;     //!< Payload byte count.
	unsigned char      data[];   //!< The opaque action payload.
};

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
