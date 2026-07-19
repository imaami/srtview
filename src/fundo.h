/** @file
 * Fundo: an undo *tree* with persistent side branches.
 *
 * Actions grow the tree upward (up = future); undo climbs down toward
 * the past, redo climbs back up the branch it last grew out of or
 * descended into.  Branches abandoned by undoing are never deleted:
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
 * @return     The payload of the action being undone (the node
 *             departed from), or nullptr at the root. On success
 *             the pointer is never nullptr, even for an empty
 *             payload.
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

#ifdef __cplusplus
} /* extern "C" */
#endif

#undef STD

#endif /* SRTVIEW_SRC_FUNDO_H_ */
