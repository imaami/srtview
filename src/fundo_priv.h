/** @file
 *
 */
#ifndef SRTVIEW_SRC_FUNDO_PRIV_H_
#define SRTVIEW_SRC_FUNDO_PRIV_H_

#include <stdint.h> /* int32_t, uint64_t */
#include <string.h> /* memcmp() */

#include "fundo.h"
#include "list_priv.h"

/** @brief One recorded action: a node in the undo tree.
 *
 * The tree is built from the same circular sentinel lists as
 * everything else: @a children is the sentinel of this node's child
 * ring, and @a hook is this node's link in its parent's ring.
 * @a ring is different: a headless cycle of travel peers with no
 * sentinel -- every member is a node -- self-linked while the node
 * is a ring of one.
 */
struct fundo_node
{
	struct list        hook;     //!< Sibling link in the parent's ring.
	struct list        children; //!< Sentinel of the child ring.
	struct list        ring;     //!< Travel cycle link (headless).
	struct fundo_node *parent;   //!< Toward the past; root: nullptr.
	struct fundo_node *last;     //!< Redo direction; may be nullptr.
	size_t             count;    //!< Child count
	size_t             size;     //!< Payload byte count.
	union {
		int32_t    trav[2];  //!< One counter >= 0 (net forward
		                     //!< passes), one <= -1 (net backward
		                     //!< passes offset by -1); the latest
		                     //!< pass direction's counter sits at
		                     //!< index 0.  Net = [0] + [1] + 1.
		uint64_t   trav64;   //!< The pair as one word, for turning
		                     //!< the bookkeeping into single 64-bit
		                     //!< ops.  C-only punning; this header
		                     //!< is never seen by C++.
	};
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
		if (c->size == size &&
		    (!size || !memcmp(c->data, data, size)))
			return c;
	}
	return nullptr;
}

#endif /* SRTVIEW_SRC_FUNDO_PRIV_H_ */
