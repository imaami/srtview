/** @file
 *
 * llm internals shared with tests: the growable byte buffer and
 * JSON string escaping and extraction.  Everything here is pure
 * buffer-in/buffer-out -- no sockets, no threads -- so the protocol
 * payloads are testable without a server.  HTTP itself is libcurl's
 * business now and has no parsing surface of ours to test.
 */
#ifndef SRTVIEW_SRC_LLM_PRIV_H_
#define SRTVIEW_SRC_LLM_PRIV_H_

#include <stddef.h>

#include "llm.h"

/** @brief A growable byte buffer; zero-initialize to start empty. */
struct llm_buf {
	char   *data; //!< Heap storage; nullptr while empty.
	size_t  size; //!< Bytes used.
	size_t  cap;  //!< Bytes allocated.
};

/** @brief Appends bytes, growing geometrically.
 *
 * @param b    The buffer.
 * @param data The bytes; may be nullptr when @a n is 0.
 * @param n    The byte count.
 * @return     true on success, false on allocation failure.
 */
extern bool
llm_buf_put (struct llm_buf *b,
             void const     *data,
             size_t          n);

/** @brief Appends a C string verbatim (no terminator). */
extern bool
llm_buf_str (struct llm_buf *b,
             char const     *s);

/** @brief Appends a C string as a JSON string, quotes included.
 *
 * Escapes the quote, the backslash and control bytes; UTF-8 passes
 * through raw.
 */
extern bool
llm_buf_json (struct llm_buf *b,
              char const     *s);

/** @brief Frees the storage and zeroes the buffer. */
extern void
llm_buf_free (struct llm_buf *b);

/** @brief A read cursor over raw JSON text. */
struct llm_cur {
	char const *p;   //!< Current position.
	char const *end; //!< One past the last byte.
};

/** @brief Seeks the cursor from an object to the value of a key.
 *
 * The cursor must sit at (whitespace before) a '{'; on success it
 * sits at the key's value.  Sibling values are skipped structurally,
 * strings and nesting respected.  Keys are matched as raw bytes.
 *
 * @return true when the key was found, false otherwise (including
 *         malformed input); the cursor is unspecified on failure.
 */
extern bool
llm_json_get (struct llm_cur *c,
              char const     *key);

/** @brief Seeks the cursor from an array to its first element.
 *
 * @return true when the array is well-formed and non-empty.
 */
extern bool
llm_json_first (struct llm_cur *c);

/** @brief Unescapes the string at the cursor into a buffer.
 *
 * Handles the named escapes and \\uXXXX including surrogate pairs
 * (encoded to UTF-8).  On success the cursor sits past the closing
 * quote.
 *
 * @return true on success; on failure @a out may hold a partial
 *         prefix.
 */
extern bool
llm_json_str (struct llm_cur *c,
              struct llm_buf *out);

#endif /* SRTVIEW_SRC_LLM_PRIV_H_ */
