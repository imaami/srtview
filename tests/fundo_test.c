/** @file
 * Unit tests for the fundo undo tree: linear walk, branch splitting,
 * adoption of identical actions, and redo continuity after detours.
 */
#include <stdio.h>
#include <string.h>

#include "fundo.h"

static int g_fail;

static void
check (bool ok, char const *what)
{
	printf("%s  %s\n", ok ? "OK  " : "FAIL", what);
	if (!ok)
		++g_fail;
}

static bool
is (void const *data, size_t size, char const *want)
{
	return data && size == strlen(want)
	    && !memcmp(data, want, size);
}

static int
act (struct fundo *f, char const *s)
{
	return fundo_act(f, s, strlen(s));
}

/* Sequenced wrappers: the payload pointer and its size come from the
 * same call, so they must not be read as unsequenced arguments.
 */
static bool
undo_is (struct fundo *f, char const *want)
{
	size_t n = 0;
	void const *d = fundo_undo(f, &n);
	return is(d, n, want);
}

static bool
redo_is (struct fundo *f, char const *want)
{
	size_t n = 0;
	void const *d = fundo_redo(f, &n);
	return is(d, n, want);
}

static void
test_linear (void)
{
	struct fundo f;
	size_t n;

	check(fundo_init(&f) == 0, "init");
	check(!fundo_can_undo(&f) && !fundo_can_redo(&f)
	      && fundo_branches(&f) == 0, "fresh tree is empty");

	check(act(&f, "A") == 0 && act(&f, "B") == 0 && act(&f, "C") == 0,
	      "three linear actions");
	check(undo_is(&f, "C") && undo_is(&f, "B") && undo_is(&f, "A"),
	      "undo walks back C, B, A");
	check(!fundo_can_undo(&f) && fundo_undo(&f, &n) == nullptr,
	      "undo stops at the root");
	check(redo_is(&f, "A") && redo_is(&f, "B") && redo_is(&f, "C"),
	      "redo walks forward A, B, C");
	check(!fundo_can_redo(&f) && fundo_redo(&f, &n) == nullptr,
	      "redo stops at the tip");

	fundo_fini(&f);
	check(f.root == nullptr && f.cur == nullptr, "fini zeroes");
}

static void
test_branching (void)
{
	struct fundo f;

	fundo_init(&f);
	act(&f, "A");
	act(&f, "B");
	act(&f, "C");                          /* A -> B -> C          */

	fundo_undo(&f, nullptr);               /* at B                 */
	check(fundo_branches(&f) == 1, "one branch above B");
	act(&f, "D");                          /* split: B -> D        */
	fundo_undo(&f, nullptr);               /* back at B            */
	check(fundo_branches(&f) == 2, "split kept the old branch");
	check(redo_is(&f, "D"), "redo follows the branch last grown");

	fundo_undo(&f, nullptr);               /* at B again           */
	act(&f, "C");                          /* identical: adoption  */
	check(fundo_branches(&f) == 0, "adopted C has no children");
	fundo_undo(&f, nullptr);
	check(fundo_branches(&f) == 2, "adoption did not duplicate");
	check(redo_is(&f, "C"), "redo now follows the adopted branch");

	fundo_fini(&f);
}

/* The spec's detour case: descend an old branch by repeating its
 * action, then redo must continue up that branch's own future.
 */
static void
test_detour (void)
{
	struct fundo f;

	fundo_init(&f);
	act(&f, "A");
	act(&f, "B");
	act(&f, "C");                          /* A -> B -> C          */
	fundo_undo(&f, nullptr);
	fundo_undo(&f, nullptr);               /* at A                 */
	act(&f, "X");                          /* detour branch        */
	fundo_undo(&f, nullptr);               /* at A                 */
	act(&f, "B");                          /* adopt old B          */
	check(fundo_can_redo(&f), "adopted branch kept its future");
	check(redo_is(&f, "C"),
	      "redo continues up the adopted branch after a detour");

	fundo_fini(&f);
}

static void
test_edges (void)
{
	struct fundo *p;
	size_t n;

	check(fundo_act(nullptr, "x", 1) != 0
	      && fundo_undo(nullptr, &n) == nullptr
	      && fundo_redo(nullptr, &n) == nullptr
	      && !fundo_can_undo(nullptr) && !fundo_can_redo(nullptr)
	      && fundo_branches(nullptr) == 0,
	      "null tolerance");

	p = fundo_create();
	check(p && p->error == 0, "create");
	check(fundo_act(p, nullptr, 0) == 0 && fundo_undo(p, &n)
	      && n == 0, "empty payload is a valid action");
	fundo_act(p, nullptr, 0);
	fundo_act(p, nullptr, 0);              /* adoption of empty    */
	fundo_undo(p, nullptr);
	check(fundo_branches(p) == 1, "empty payloads deduplicate");
	check(fundo_act(p, nullptr, 7) != 0, "null data with size fails");
	fundo_destroy(&p);
	check(p == nullptr, "destroy nulls the pointer");
}

int
main (void)
{
	test_linear();
	test_branching();
	test_detour();
	test_edges();
	printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
	       g_fail, g_fail == 1 ? "" : "s");
	return g_fail ? 1 : 0;
}
