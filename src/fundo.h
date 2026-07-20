/** @file
 * Fundo: an undo *tree* with persistent side branches.
 *
 * Actions grow the tree upward (up = future); undo climbs down toward
 * the past, redo climbs back up the branch it last grew out of or
 * descended into.  Both return the payload of the node they arrive
 * at, so payloads read naturally as recorded states rather than
 * transitions.  Branches abandoned by undoing are never deleted:
 * taking a different action after undoing splits a new branch, while
 * taking an action byte-identical to an existing child at that point
 * adopts that branch instead -- as if redo had been pressed -- and
 * redo then continues up it, even after a detour.
 *
 * Payloads are opaque bytes owned by the tree; identity is exact
 * content equality.  Pointers returned by undo/redo remain valid
 * until fundo_fini()/fundo_destroy(), since nodes are never freed
 * before then.
 *
 * Nodes can additionally be joined into a travel ring (fundo_join()):
 * a closed cycle of states threaded through the tree, for loops like
 * search hits wrapping around a document.  Traveling the ring
 * (fundo_travel()) revisits existing nodes instead of growing the
 * tree, and each node's direction/pass pair linearizes any number of
 * laps after the fact (fundo_net(), fundo_heading()).
 *
 * Plain C API, consumable from C++.
 */
#ifndef SRTVIEW_SRC_FUNDO_H_
#define SRTVIEW_SRC_FUNDO_H_

#ifdef __cplusplus
# include <cstddef>
# define STD(x) std::x
extern "C" {
#else
# include <stddef.h>
# define STD(x) x
#endif

struct fundo_node;

/** @brief An undo tree and the current position within it. */
struct fundo {
	struct fundo_node *root;  //!< The initial state; empty payload.
	struct fundo_node *cur;   //!< Current position in the tree.
	int                error; //!< Error number from construction.
};

/** @brief Initializes and returns a new undo tree by-value.
 *
 * Allocates the root node; check .error before use.
 *
 * @return The new tree.
 */
extern struct fundo
fundo (void);

/** @brief Initializes, but does not allocate, an undo tree.
 *
 * Allocates the root node.
 *
 * @param dest The tree to initialize.
 * @return     0 on success and an error number on failure.
 */
extern int
fundo_init (struct fundo *dest);

/** @brief Frees the whole tree, including every side branch,
 *         but not the object itself.
 *
 * @param dest The tree to uninitialize.
 */
extern void
fundo_fini (struct fundo *dest);

/** @brief Allocates and initializes a new undo tree.
 *
 * @return A pointer to the created tree, or nullptr if allocating
 *         the tree itself failed. A non-null tree may still have
 *         failed root allocation: check .error before use, as with
 *         fundo().
 */
extern struct fundo *
fundo_create (void);

/** @brief Uninitializes and frees the tree, then sets the
 *         caller's pointer to nullptr.
 *
 * @param p_dest Pointer to the caller's tree pointer.
 */
extern void
fundo_destroy (struct fundo **p_dest);

/** @brief Records an action of @a size bytes at @a data.
 *
 * If a child of the current node holds a byte-identical payload,
 * descends into it (adoption); otherwise grows a new branch. Either
 * way the redo direction now points along the taken action.
 *
 * @param f    The tree to record into.
 * @param data The action payload bytes.
 * @param size The payload byte count.
 * @return     0 on success and an error number on failure.
 */
extern int
fundo_act (struct fundo *f,
           void const   *data,
           STD(size_t)   size);

/** @brief Steps one node down toward the past.
 *
 * @param f    The tree to step in.
 * @param size Receives the payload size if non-null.
 * @return     The payload of the node arrived at (the parent of the
 *             action being undone), or nullptr when already at the
 *             root. On success the pointer is never nullptr, even
 *             for an empty payload.
 */
extern void const *
fundo_undo (struct fundo *f,
            STD(size_t)  *size);

/** @brief Steps one node up along the last grown or adopted branch.
 *
 * @param f    The tree to step in.
 * @param size Receives the payload size if non-null.
 * @return     The payload of the action being redone, or nullptr
 *             when there is nothing to redo. On success the pointer
 *             is never nullptr, even for an empty payload.
 */
extern void const *
fundo_redo (struct fundo *f,
            STD(size_t)  *size);

/** @brief Whether undo is possible (not at the root).
 *
 * @param f The tree to inspect.
 * @return  1 if the current node has a parent, otherwise 0.
 */
extern int
fundo_can_undo (struct fundo const *f);

/** @brief Whether redo is possible (a branch was grown or adopted
 *         above the current node).
 *
 * @param f The tree to inspect.
 * @return  1 if the current node has a redo direction, otherwise 0.
 */
extern int
fundo_can_redo (struct fundo const *f);

/** @brief The number of branches growing out of the current node.
 *
 * @param f The tree to inspect.
 * @return  The current node's child count.
 */
extern STD(size_t)
fundo_branches (struct fundo const *f);

/** @brief The current node of the tree.
 *
 * Together with fundo_up() and fundo_data() this allows read-only
 * inspection of the path back to the root without moving the tree's
 * position -- e.g. resolving the previous value of one facet of a
 * state payload before undoing.
 *
 * @param f The tree to inspect.
 * @return  The current node, or nullptr if @a f is nullptr or
 *          uninitialized.
 */
extern struct fundo_node const *
fundo_at (struct fundo const *f);

/** @brief The parent of a node, one step toward the past.
 *
 * @param n The node whose parent to get.
 * @return  The parent, or nullptr for the root or a nullptr @a n.
 */
extern struct fundo_node const *
fundo_up (struct fundo_node const *n);

/** @brief A node's payload.
 *
 * @param n    The node to read.
 * @param size Receives the payload size if non-null.
 * @return     The payload bytes, or nullptr if @a n is nullptr. For
 *             a valid node the pointer is never nullptr, even for an
 *             empty payload.
 */
extern void const *
fundo_data (struct fundo_node const *n,
            STD(size_t)             *size);

/** @brief Splices the current node into a travel ring.
 *
 * The current node must not already be in a ring (every node starts
 * as a ring of one).  It is inserted as @a anchor's ring successor
 * when @a dir is positive and as its predecessor when negative; a
 * loop discovered by walking forward therefore keeps the invariant
 * that the newest member's next pointer is the origin and the
 * origin's prev pointer is the newest member.
 *
 * @param f      The tree whose current node joins.
 * @param anchor A node of the same tree to splice next to.  Const
 *               only for symmetry with the inspection API; the ring
 *               links belong to the tree and are mutated.
 * @param dir    Positive to join after @a anchor, negative to join
 *               before it.
 * @return       0 on success and an error number on failure.
 */
extern int
fundo_join (struct fundo            *f,
            struct fundo_node const *anchor,
            int                      dir);

/** @brief Steps the current position along its travel ring.
 *
 * Moves to the ring successor (@a dir positive) or predecessor
 * (negative) without creating nodes, and bumps the arrival node's
 * direction/pass pair: a pass against the direction of earlier
 * surplus passes cancels one of them, otherwise it counts a new
 * pass, and the latest direction is remembered (fundo_heading()).
 * A node that never joined a ring is a ring of one, so traveling
 * steps to the node itself.
 *
 * @param f    The tree to travel in.
 * @param dir  Positive for forward, negative for backward.
 * @param size Receives the payload size if non-null.
 * @return     The payload of the node arrived at, or nullptr if
 *             @a f is empty or @a dir is 0.  On success the pointer
 *             is never nullptr, even for an empty payload.
 */
extern void const *
fundo_travel (struct fundo *f,
              int           dir,
              STD(size_t)  *size);

/** @brief A node's successor on its travel ring.
 *
 * @param n The node to inspect.
 * @return  The next ring member, or nullptr if @a n is nullptr or a
 *          ring of one.
 */
extern struct fundo_node const *
fundo_next (struct fundo_node const *n);

/** @brief A node's predecessor on its travel ring.
 *
 * @param n The node to inspect.
 * @return  The previous ring member, or nullptr if @a n is nullptr
 *          or a ring of one.
 */
extern struct fundo_node const *
fundo_prev (struct fundo_node const *n);

/** @brief Net number of travel passes through a node.
 *
 * Positive for surplus forward passes, negative for backward.
 * Summed over a whole ring this linearizes multi-lap travel into a
 * single position on an unrolled line.
 *
 * @param n The node to inspect.
 * @return  Forward passes minus backward passes; 0 for nullptr.
 */
extern int
fundo_net (struct fundo_node const *n);

/** @brief The direction of the latest travel pass through a node.
 *
 * @param n The node to inspect.
 * @return  1 if the latest pass (or none yet) was forward, -1 if it
 *          was backward, 0 for nullptr.
 */
extern int
fundo_heading (struct fundo_node const *n);

#ifdef __cplusplus
} /* extern "C" */
#endif

#undef STD

#endif /* SRTVIEW_SRC_FUNDO_H_ */
