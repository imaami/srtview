/** @file
 *
 * Circular doubly-linked list, sentinel-head style: an empty list is
 * one node whose next and prev point to itself.  Adapted from the
 * mcp2210-proxy list module.
 */
#ifndef SRTVIEW_SRC_LIST_H_
#define SRTVIEW_SRC_LIST_H_

struct list;

/** @brief Add a node to the end of a list.
 *
 * @param list The list to modify.
 * @param node The node to append.
 */
extern void
list_append (struct list *list,
             struct list *node);

/** @brief Add a node to the front of a list.
 *
 * @param list The list to modify.
 * @param node The node to prepend.
 */
extern void
list_prepend (struct list *list,
              struct list *node);

/** @brief Remove a node from a list.
 *
 * @param node The node to remove.
 */
extern void
list_del (struct list *node);

#endif /* SRTVIEW_SRC_LIST_H_ */
