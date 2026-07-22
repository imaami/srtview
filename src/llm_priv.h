/** @file
 *
 * llm internals shared with tests: the growable byte buffer, JSON
 * string escaping and extraction, and HTTP/1.1 response parsing.
 * Everything here is pure buffer-in/buffer-out -- no sockets, no
 * threads -- so the wire protocol is testable without a server.
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

/** @brief Parses a complete HTTP/1.1 response held in memory.
 *
 * Locates the body by Content-Length, Transfer-Encoding: chunked
 * (decoded into @a dec) or connection close (everything after the
 * blank line).
 *
 * @param raw       The full response bytes.
 * @param size      The byte count.
 * @param dec       Receives the de-chunked body when chunked.
 * @param body      Receives the body location (into @a raw or
 *                  @a dec).
 * @param body_size Receives the body byte count.
 * @return          The HTTP status code (>= 100), or LLM_ERR_PARSE
 *                  on malformed or truncated input.
 */
extern int
llm_http_parse (char const     *raw,
                size_t          size,
                struct llm_buf *dec,
                char const    **body,
                size_t         *body_size);

#endif /* SRTVIEW_SRC_LLM_PRIV_H_ */
