/** @file
 * Small C utility layer for the fundo core; the subset of the donor
 * project's util.h that this module needs.
 */
#ifndef SRTVIEW_SRC_CUTIL_H_
#define SRTVIEW_SRC_CUTIL_H_

#include <stdbool.h>
#include <stddef.h> /* offsetof() */
#include <string.h>

/** @brief Instruct the compiler to always inline a function.
 */
#define force_inline    static inline __attribute__((always_inline,unused))

#define typeof_member(T, member) __typeof__(((T *)0)->member)

#define container_of(ptr, T, member) ((T *)(void *)( \
	(char *)(typeof_member(T, member) *)(ptr) - offsetof(T, member) \
))

/** @brief Exact content identity of two memory areas.
 *
 * Unlike a bare memcmp() this is only true when the sizes match too.
 *
 * @param s1 Memory area 1.
 * @param s2 Memory area 2.
 * @param n1 Size of memory area 1.
 * @param n2 Size of memory area 2.
 * @return   @a true if the areas are the same size with identical
 *           content, otherwise @a false.
 */
force_inline bool
mem_same (void const *s1,
          void const *s2,
          size_t      n1,
          size_t      n2)
{
	return n1 == n2 && !memcmp(s1, s2, n1);
}

#endif /* SRTVIEW_SRC_CUTIL_H_ */
