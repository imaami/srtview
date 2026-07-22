/** @file
 *
 */
#ifndef SRTVIEW_SRC_LIST_PRIV_H_
#define SRTVIEW_SRC_LIST_PRIV_H_

#include "cutil.h"
#include "list.h"

/** @brief A doubly-linked list node.
 */
struct list
{
	struct list *next; //!< Pointer to the next list node.
	struct list *prev; //!< Pointer to the previous list node.
};

/** @brief Iterate a list.
 *
 * The next node is fetched before the loop body runs, so removing or
 * freeing the current node while iterating is safe.
 *
 * @param node__ A list pointer to use as an iterator.
 * @param list__ The list to iterate.
 */
#define list_foreach(node__, list__) \
	for (struct list *n__ = (list__)->next; \
	     (node__) = n__, n__ = n__->next, (node__) != (list__);)


/** @brief Initialize a list by value.
 *
 * In normal circumstances and if used correctly, this function's job
 * is to make `obj->prev` and `obj->next` point to `obj` itself. It's
 * possible to use this in other ways, too, if you actually know what
 * you're doing (see e.g. `list_fini()`).
 *
 * @note If you're using this as a RAII-style initializer - which is
 *       the primary intended purpose - the input argument *must* be
 *       a pointer to the *same* object you assign the return value.
 *
 * @param self The list to initialize.
 */
force_inline struct list
list (struct list *self)
{
	return (struct list){ self, self };
}

/** @brief Initialize a list.
 *
 * After the call `obj->prev` and `obj->next` will point to `obj` itself.
 *
 * @param obj The list to initialize.
 */
force_inline void
list_init (struct list *obj)
{
	*obj = list(obj);
}

/** @brief Uninitialize a list.
 *
 * After the call `obj->prev` and `obj->next` will be `nullptr`.
 *
 * @note Existing links involving `obj` will be left dangling.
 *       Any cleanup actions must be done before calling this.
 *
 * @param obj The list to uninitialize and "poison".
 */
force_inline void
list_fini (struct list *obj)
{
	*obj = list(nullptr);
}

/** @brief Return a pointer to the first node of a list without removing it.
 *
 * @param list The list to inspect.
 * @return     The first node if @a list was not empty, otherwise nullptr.
 */
force_inline struct list *
list_head (struct list const *list)
{
	return list != (struct list const *)list->next
	       ? list->next
	       : nullptr;
}

/** @brief Return a pointer to the last node of a list without removing it.
 *
 * @param list The list to inspect.
 * @return     The last node if @a list was not empty, otherwise nullptr.
 */
force_inline struct list *
list_tail (struct list const *list)
{
	return list != (struct list const *)list->prev
	       ? list->prev
	       : nullptr;
}

/** @brief Add a node to the end of a list.
 *
 * Identical to @ref list_append() but inlined.
 *
 * @param list The list to modify.
 * @param node The node to append.
 */
force_inline void
list_append_ (struct list *list,
              struct list *node)
{
	node->next = list;
	node->prev = list->prev;
	list->prev->next = node;
	list->prev = node;
}

/** @brief Detach a node from a list without uninitializing the node.
 *
 * Leaves the list in a good state but the detached node with dangling
 * pointers; only for implementing more complex functions.
 *
 * @param node The node to detach.
 */
force_inline void
list_detach_ (struct list *node)
{
	node->next->prev = node->prev;
	node->prev->next = node->next;
}

#endif /* SRTVIEW_SRC_LIST_PRIV_H_ */
