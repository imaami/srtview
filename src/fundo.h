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

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct fundo_node;

/** @brief An undo tree and the current position within it. */
struct fundo {
	struct fundo_node *root;  //!< The initial state; empty payload.
	struct fundo_node *cur;   //!< Current position in the tree.
	int                error; //!< Error number from construction.
};

/* Initializes and returns a new undo tree by-value.
 * Allocates the root node; check .error before use.
 */
extern struct fundo
fundo (void);

/* Initializes, but does not allocate, an undo tree.
 * Allocates the root node. Returns 0 on success and
 * an error number on failure.
 */
extern int
fundo_init (struct fundo *dest);

/* Frees the whole tree, including every side branch,
 * but not the object itself.
 */
extern void
fundo_fini (struct fundo *dest);

/* Allocates and initializes a new undo tree.
 * Returns a pointer to the created tree.
 */
extern struct fundo *
fundo_create (void);

/* Uninitializes and frees the tree, then sets the
 * caller's pointer to nullptr.
 */
extern void
fundo_destroy (struct fundo **p_dest);

/* Records an action of @a size bytes at @a data. If a child of the
 * current node holds a byte-identical payload, descends into it
 * (adoption); otherwise grows a new branch. Either way the redo
 * direction now points along the taken action. Returns 0 on success
 * and an error number on failure.
 */
extern int
fundo_act (struct fundo *f,
           void const   *data,
           size_t        size);

/* Steps one node down toward the past. Returns the payload of the
 * action being undone (the node departed from), with its size in
 * @a size if non-null, or nullptr at the root.
 */
extern void const *
fundo_undo (struct fundo *f,
            size_t       *size);

/* Steps one node up along the last grown or adopted branch. Returns
 * the payload of the action being redone, with its size in @a size
 * if non-null, or nullptr when there is nothing to redo.
 */
extern void const *
fundo_redo (struct fundo *f,
            size_t       *size);

/* Returns true if undo is possible (not at the root). */
extern bool
fundo_can_undo (struct fundo const *f);

/* Returns true if redo is possible (a branch was grown or adopted
 * above the current node).
 */
extern bool
fundo_can_redo (struct fundo const *f);

/* Returns the number of branches growing out of the current node. */
extern size_t
fundo_branches (struct fundo const *f);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SRTVIEW_SRC_FUNDO_H_ */
