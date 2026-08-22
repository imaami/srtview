/** @file
 *
 * Task client for a local llama.cpp server (llama-server).
 *
 * Sends free-form tasks -- optional system context plus a prompt --
 * to the server's OpenAI-compatible chat endpoint (POST
 * /v1/chat/completions) on a private worker thread, and hands each
 * reply to a callback.  The transport is libcurl's multi interface
 * driven by an epoll event loop on that worker.  The server owns
 * the model and applies its chat template; this module is transport
 * and protocol only: no JSON library, no tokenization, no Qt, no
 * C++.
 *
 * Tasks run FIFO, one in flight at a time (llama-server serves a
 * single slot by default).  The completion callback runs on the
 * worker thread -- marshal in the callee if the result must land
 * elsewhere; llm_cancel() and llm_destroy() instead run the
 * callbacks of the tasks they retire on the calling thread.  The
 * result text is owned by the client and valid only inside the
 * callback.  Every accepted task gets exactly one callback.
 *
 * Plain C API, consumable from C++.
 */
#ifndef SRTVIEW_SRC_LLM_H_
#define SRTVIEW_SRC_LLM_H_

#ifdef __cplusplus
# include <cstddef>
# include <cstdint>
# define LLM_STD(x) std::x
extern "C" {
#else
# include <stddef.h>
# include <stdint.h>
# define LLM_STD(x) x
#endif

struct llm;

/** @brief Completion status codes delivered to callbacks. */
enum {
	LLM_OK          =  0, //!< Reply extracted; text is the answer.
	LLM_ERR_ARGS    = -1, //!< Malformed task arguments.
	LLM_ERR_MEM     = -2, //!< Allocation failure.
	LLM_ERR_CONNECT = -3, //!< Could not reach the server.
	LLM_ERR_SEND    = -4, //!< Sending the request failed.
	LLM_ERR_RECV    = -5, //!< Reading the reply failed.
	LLM_ERR_TIMEOUT = -6, //!< No reply bytes within the task timeout.
	LLM_ERR_HTTP    = -7, //!< Non-2xx; text is the server's message.
	LLM_ERR_PARSE   = -8, //!< Reply arrived but could not be read.
	LLM_ERR_CANCEL  = -9, //!< Retired by llm_cancel()/llm_destroy().
};

/** @brief Task completion callback.
 *
 * Runs on the worker thread for tasks that reached the wire, on the
 * canceling/destroying thread for tasks retired early.
 *
 * @param ud     The pointer given to llm_ask().
 * @param id     The task id llm_ask() returned.
 * @param status LLM_OK or one of the LLM_ERR_* codes.
 * @param text   The reply (LLM_OK) or the server's error message
 *               (LLM_ERR_HTTP, when one was parseable); NUL-
 *               terminated, valid only inside the call.  May be
 *               nullptr for errors.
 * @param size   Byte count of @a text, excluding the terminator.
 */
typedef void (*llm_done_fn) (void             *ud,
                             LLM_STD(uint64_t) id,
                             int               status,
                             char const       *text,
                             LLM_STD(size_t)   size);

/** @brief One task: what to send and how long to wait.
 *
 * Zero-initialize, then set fields; the zero defaults are the
 * intended ones: no system context, no reply cap, temperature 0
 * (greedy sampling -- repeatable output, which is what mechanical
 * summarize/organize tasks want), 300 s timeout.
 */
struct llm_task {
	char const      *system;      //!< Optional context; may be null.
	char const      *prompt;      //!< The task body; required.
	char const      *json_schema; //!< Optional JSON Schema object;
	                              //!< requests schema-constrained
	                              //!< JSON through response_format.
	double           temperature; //!< Sampling temperature as-is;
	                              //!< negative for the server's
	                              //!< default.
	LLM_STD(int32_t) max_tokens;  //!< > 0 caps the reply length.
	LLM_STD(int32_t) timeout_s;   //!< Hard cap on the whole round
	                              //!< trip; <= 0 means 300, above
	                              //!< 86400 clamps to a day.  A
	                              //!< non-streamed reply arrives
	                              //!< whole, so waiting out the
	                              //!< generation and waiting on a
	                              //!< dead server are the same
	                              //!< silence; this bounds both.
	LLM_STD(int32_t) urgent;      //!< 1 queues ahead of every task
	                              //!< not yet on the wire and waits
	                              //!< no pace gap, ending one that
	                              //!< is running; 0 queues behind
	                              //!< and breathes.  A generation
	                              //!< under way is never
	                              //!< interrupted.
};

/** @brief Creates a client and starts its worker thread.
 *
 * Nothing is contacted yet; each task dials the server on its own.
 *
 * @param host Server host; nullptr or empty for 127.0.0.1.
 * @param port Server port; 0 for 8080.
 * @return     The new client, or nullptr on failure.
 */
extern struct llm *
llm_create (char const       *host,
            LLM_STD(uint16_t) port);

/** @brief Stops the worker, retires every task, frees the client,
 *         then sets the caller's pointer to nullptr.
 *
 * An in-flight task is aborted on the wire and reported as
 * LLM_ERR_CANCEL from the worker before it exits; queued tasks are
 * reported as LLM_ERR_CANCEL on the calling thread.  May block
 * briefly (a connect attempt already in progress must resolve).
 *
 * @param p_c Pointer to the caller's client pointer.
 */
extern void
llm_destroy (struct llm **p_c);

/** @brief Queues a task; the callback delivers the outcome.
 *
 * The task struct and its strings are copied out before returning
 * and need not outlive the call.
 *
 * @param c    The client.
 * @param task What to ask.
 * @param done Completion callback; required.
 * @param ud   Passed through to @a done.
 * @return     A nonzero task id, or 0 on failure (bad arguments or
 *             out of memory; no callback happens).
 */
extern LLM_STD(uint64_t)
llm_ask (struct llm            *c,
         struct llm_task const *task,
         llm_done_fn            done,
         void                  *ud);

/** @brief Retires a task early.
 *
 * A queued task is dequeued and its callback runs with
 * LLM_ERR_CANCEL before this returns; the in-flight task has its
 * connection shut down and reports LLM_ERR_CANCEL from the worker.
 *
 * @param c  The client.
 * @param id The id llm_ask() returned.
 * @return   1 if the task was found (retired or aborting), 0 if it
 *           was unknown or already complete.
 */
extern int
llm_cancel (struct llm       *c,
            LLM_STD(uint64_t) id);

/** @brief Sets a quiet gap between tasks.
 *
 * After a task completes, the worker idles @a ms milliseconds
 * before starting the next -- breathing room for a thermally
 * fragile accelerator behind the server.  The gap sits between
 * tasks only, never delays the first, and llm_destroy() cuts it
 * short, as does setting the pace to zero while a gap runs or
 * queueing an urgent task into one -- the next task starts at
 * once.  Cancellation does not: a retired task changes nothing
 * about the accelerator's need to breathe.
 *
 * @param c  The client.
 * @param ms Gap in milliseconds; <= 0 for none (the default).
 */
extern void
llm_pace (struct llm      *c,
          LLM_STD(int32_t) ms);

/** @brief A static description of a status code.
 *
 * @param status LLM_OK or an LLM_ERR_* code.
 * @return       A short English phrase; never nullptr.
 */
extern char const *
llm_strerror (int status);

#ifdef __cplusplus
} /* extern "C" */
#endif

#undef LLM_STD

#endif /* SRTVIEW_SRC_LLM_H_ */
