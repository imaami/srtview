/** @file
 *
 * Small C utility layer for the fundo core; the subset of the donor
 * project's util.h that this module needs.
 */
#ifndef SRTVIEW_SRC_CUTIL_H_
#define SRTVIEW_SRC_CUTIL_H_

#include <stddef.h> /* offsetof() */

/** @brief Instruct the compiler to always inline a function.
 */
#define force_inline    static inline __attribute__((always_inline,unused))

#define typeof_member(T, member) __typeof__(((T *)0)->member)

#define container_of(ptr, T, member) ((T *)(void *)( \
	(char *)(typeof_member(T, member) *)(ptr) - offsetof(T, member) \
))

#endif /* SRTVIEW_SRC_CUTIL_H_ */
