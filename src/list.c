/** @file
 *
 */
#include <stdio.h>

#include "list_priv.h"

void
list_append (struct list *list,
             struct list *node)
{
	list_append_(list, node);
}

void
list_prepend (struct list *list,
              struct list *node)
{
	node->prev = list;
	node->next = list->next;
	list->next->prev = node;
	list->next = node;
}

void
list_del (struct list *node)
{
	struct list *next = list_head(node);
	struct list *prev = list_tail(node);

	/* Validate. */
	if (!next != !prev) {
		fprintf(stderr, "list: corrupt node: node=%p next=%p"
		        " prev=%p\n", (void *)node, (void *)node->next,
		        (void *)node->prev);

	} else if (next) {
		if (next->prev != node || prev->next != node)
			fprintf(stderr, "list: corrupt list: node=%p"
			        " node->next->prev=%p node->prev->next=%p\n",
			        (void *)node, (void *)next->prev,
			        (void *)prev->next);
		else
			list_detach_(node);
	}

	list_fini(node);
}
