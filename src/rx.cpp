// rx.cpp -- see rx.hpp.  Everything here is exact string algebra:
// knit() and unknit() are inverses over the portable subset, and
// braid() never emits a pattern its own variants would fail to
// match.  UTF-8 is handled by never splitting a codepoint: prefixes
// and suffixes trim to codepoint boundaries, character classes and
// bare optionals stay ASCII-only, and anything wider rides inside a
// (?:...) group.
#include <algorithm>
#include <cstdint>
#include <map>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "rx.hpp"

namespace rx {

namespace {

// The metacharacters escaped in literal position -- the same set
// topics.cpp treats as structure.
constexpr std::string_view kMeta = "^$.[]()*+?{}|\\";

bool meta(unsigned char c)
{
	return c < 0x80 && kMeta.find(char(c)) != std::string_view::npos;
}

bool ascii_alnum(unsigned char c)
{
	return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
	    || (c >= 'a' && c <= 'z');
}

bool ascii_ws(unsigned char c)
{
	return c == ' ' || (c >= '\t' && c <= '\r');
}

// Bytes in the codepoint led by @a c; degenerate leads count as one
// opaque byte, which only ever costs a merge, never a broken split.
std::size_t cp_len(unsigned char c)
{
	if (c < 0x80)
		return 1;
	if (c >= 0xF0)
		return 4;
	if (c >= 0xE0)
		return 3;
	if (c >= 0xC0)
		return 2;
	return 1;
}

bool cp_cont(unsigned char c)
{
	return (c & 0xC0) == 0x80;
}

void esc_to(std::string &out, std::string_view s)
{
	for (char const c : s) {
		if (meta((unsigned char)c))
			out += '\\';
		out += c;
	}
}

std::string esc(std::string_view s)
{
	std::string out;
	esc_to(out, s);
	return out;
}

// A word as an optional tail: a lone ASCII byte quantifies bare,
// anything wider hides in a group so byte-minded engines agree.
std::string opt_word(std::string_view w)
{
	if (w.size() == 1 && (unsigned char)w[0] < 0x80)
		return esc(w) + '?';
	return "(?:" + esc(w) + ")?";
}

// Longest common prefix of a sorted set, trimmed to a codepoint
// boundary of the (identical) common bytes.
std::size_t lcp(std::vector<std::string_view> const &s)
{
	std::string_view const f = s.front();
	std::size_t n = f.size();
	for (std::string_view const v : s) {
		std::size_t k = 0;
		while (k < std::min(n, v.size()) && f[k] == v[k])
			++k;
		n = k;
	}
	std::size_t i = 0;
	while (i < n) {
		std::size_t const l = cp_len((unsigned char)f[i]);
		if (i + l > n)
			break;
		i += l;
	}
	return i;
}

// Longest common suffix, its start backed off any continuation byte
// -- UTF-8 self-synchronizes, so that alone lands every element's
// cut on a codepoint boundary.
std::size_t lcsuf(std::vector<std::string_view> const &s)
{
	std::string_view const f = s.front();
	std::size_t m = f.size();
	for (std::string_view const v : s) {
		std::size_t k = 0;
		while (k < std::min(m, v.size())
		       && f[f.size() - 1 - k] == v[v.size() - 1 - k])
			++k;
		m = k;
	}
	while (m > 0 && cp_cont((unsigned char)f[f.size() - m]))
		--m;
	return m;
}

std::string emit(std::vector<std::string_view> s);

// The set as one alternation, grouped by first codepoint so every
// multi-element branch re-enters emit() with a common prefix to
// factor.  Reached only when at least two groups exist.
std::string alt_groups(std::vector<std::string_view> const &s)
{
	std::string out = "(?:";
	for (std::size_t i = 0; i < s.size();) {
		std::size_t const w = std::min(
			cp_len((unsigned char)s[i][0]), s[i].size());
		std::string_view const head = s[i].substr(0, w);
		std::size_t j = i + 1;
		while (j < s.size() && s[j].substr(0, w) == head)
			++j;
		if (i)
			out += '|';
		if (j - i == 1)
			esc_to(out, s[i]);
		else
			out += emit({s.begin() + std::ptrdiff_t(i),
			             s.begin() + std::ptrdiff_t(j)});
		i = j;
	}
	out += ')';
	return out;
}

// The recursive core over a sorted, unique set: factor the common
// prefix, then the common suffix, fold single ASCII leftovers into
// a class, and only then fall back to a grouped alternation.  An
// empty member escapes the recursion first and returns as a '?' on
// whatever the rest becomes.
std::string emit(std::vector<std::string_view> s)
{
	if (s.size() == 1)
		return esc(s[0]);
	bool const opt = s.front().empty();
	if (opt)
		s.erase(s.begin());
	if (s.size() == 1)
		return opt_word(s[0]);
	if (std::size_t const p = lcp(s)) {
		std::string_view const pre = s.front().substr(0, p);
		for (std::string_view &v : s)
			v.remove_prefix(p);
		std::string const inner = esc(pre) + emit(std::move(s));
		return opt ? "(?:" + inner + ")?" : inner;
	}
	if (std::size_t const q = lcsuf(s)) {
		std::string_view const suf =
			s.front().substr(s.front().size() - q);
		for (std::string_view &v : s)
			v.remove_suffix(q);
		std::ranges::sort(s);
		std::string const inner = emit(std::move(s)) + esc(suf);
		return opt ? "(?:" + inner + ")?" : inner;
	}
	bool const narrow = std::ranges::all_of(s,
		[](std::string_view v) {
			return v.size() == 1 && (unsigned char)v[0] < 0x80;
		});
	if (narrow) {
		std::string out = "[";
		for (std::string_view const v : s) {
			char const c = v[0];
			if (c == '\\' || c == ']' || c == '^' || c == '-')
				out += '\\';
			out += c;
		}
		out += ']';
		return opt ? out + '?' : out;
	}
	std::string const inner = alt_groups(s);
	return opt ? inner + '?' : inner;
}

} // namespace

std::string knit(std::vector<std::string> words)
{
	std::ranges::sort(words);
	auto const dup = std::ranges::unique(words);
	words.erase(dup.begin(), dup.end());
	if (words.empty())
		return {};
	return emit({words.begin(), words.end()});
}

namespace {

// Expansion caps: past these the pattern is treated as opaque --
// exponential blowup must refuse, never allocate.  The depth cap
// bounds the parser's only unbounded stack, the group recursion:
// far above anything knit emits for real word sets, far below a
// crafted 30000-group crash.
constexpr std::size_t kSetCap   = 64;
constexpr std::size_t kLenCap   = 256;
constexpr int         kDepthCap = 32;

struct reader {
	std::string_view s;
	std::size_t      i = 0;
	int              depth = 0;

	bool done() const { return i >= s.size(); }
	char peek() const { return s[i]; }

	bool eat(char c)
	{
		if (i < s.size() && s[i] == c) {
			++i;
			return true;
		}
		return false;
	}
};

// Cross product of the sequence so far and one atom's options,
// bounded by the caps.
bool cross(std::vector<std::string>       &acc,
           std::vector<std::string> const &opts)
{
	if (acc.size() * opts.size() > kSetCap)
		return false;
	std::vector<std::string> out;
	out.reserve(acc.size() * opts.size());
	for (std::string const &a : acc) {
		for (std::string const &o : opts) {
			if (a.size() + o.size() > kLenCap)
				return false;
			out.push_back(a + o);
		}
	}
	acc = std::move(out);
	return true;
}

bool alt(reader &r, std::vector<std::string> &out);

// One [...] class: literal members and ASCII ranges only.  A
// leading '^' negates and an escape letter abbreviates a set --
// both are infinite, both bail.
bool cls(reader &r, std::vector<std::string> &opts)
{
	auto member = [&r](char &c, bool &esc) {
		if (r.done())
			return false;
		unsigned char const u = r.peek();
		if (u >= 0x80)
			return false;
		if (r.eat('\\')) {
			if (r.done() || ascii_alnum(r.peek()))
				return false;
			c = r.s[r.i++];
			esc = true;
			return true;
		}
		if (u == ']')
			return false;
		c = r.s[r.i++];
		return true;
	};
	bool first = true;
	while (!r.eat(']')) {
		char lo;
		bool esc = false;
		// Only a BARE leading caret negates; the escaped literal
		// knit itself emits ("[\^ab]") is a member like any
		// other, and refusing it would orphan knit's own output.
		if (!member(lo, esc) || (first && lo == '^' && !esc))
			return false;
		first = false;
		char hi = lo;
		if (r.i + 1 < r.s.size() && r.peek() == '-'
		    && r.s[r.i + 1] != ']') {
			++r.i;
			bool hesc = false;
			if (!member(hi, hesc) || hi < lo
			    || std::size_t(hi - lo) >= 32)
				return false;
		}
		for (char c = lo; c <= hi; ++c)
			opts.push_back(std::string(1, c));
		if (opts.size() > kSetCap)
			return false;
	}
	return !opts.empty();
}

bool atom(reader &r, std::vector<std::string> &opts)
{
	if (r.eat('(')) {
		if (++r.depth > kDepthCap || !r.eat('?') || !r.eat(':')
		    || !alt(r, opts) || !r.eat(')'))
			return false;
		--r.depth;
		return true;
	}
	if (r.eat('['))
		return cls(r, opts);
	if (r.eat('\\')) {
		if (r.done() || ascii_alnum(r.peek()))
			return false;
		opts.push_back(std::string(1, r.s[r.i++]));
		return true;
	}
	unsigned char const u = r.peek();
	if (meta(u))
		return false;
	// A multi-byte lead must bring its whole codepoint: declaring
	// a truncated one "a codepoint" would swallow whatever comes
	// next -- possibly structure like '|' or ')'.  Beyond the
	// subset, refuse; a lone continuation byte stays a one-byte
	// literal, which can swallow nothing.
	std::size_t const w = cp_len(u);
	if (r.i + w > r.s.size())
		return false;
	for (std::size_t k = 1; k < w; ++k)
		if (!cp_cont((unsigned char)r.s[r.i + k]))
			return false;
	opts.push_back(std::string(r.s.substr(r.i, w)));
	r.i += w;
	return true;
}

bool seq(reader &r, std::vector<std::string> &out)
{
	out = {std::string()};
	while (!r.done() && r.peek() != '|' && r.peek() != ')') {
		std::vector<std::string> opts;
		if (!atom(r, opts))
			return false;
		if (r.eat('?'))
			opts.push_back({});
		std::ranges::sort(opts);
		auto const dup = std::ranges::unique(opts);
		opts.erase(dup.begin(), dup.end());
		if (!cross(out, opts))
			return false;
	}
	return true;
}

bool alt(reader &r, std::vector<std::string> &out)
{
	out.clear();
	do {
		std::vector<std::string> one;
		if (!seq(r, one))
			return false;
		out.insert(out.end(),
		           std::make_move_iterator(one.begin()),
		           std::make_move_iterator(one.end()));
		if (out.size() > kSetCap)
			return false;
	} while (r.eat('|'));
	return true;
}

} // namespace

std::optional<std::vector<std::string>> unknit(std::string_view pattern)
{
	reader r{pattern};
	std::vector<std::string> out;
	if (!alt(r, out) || !r.done())
		return std::nullopt;
	std::ranges::sort(out);
	auto const dup = std::ranges::unique(out);
	out.erase(dup.begin(), dup.end());
	return out;
}

namespace {

// Alignment width past OCR reality: refuse the quadratic table
// rather than fill it.
constexpr std::size_t kBraidCap = 512;

std::string squeeze(std::string_view s)
{
	std::string out;
	for (char const c : s) {
		if (ascii_ws((unsigned char)c)) {
			if (!out.empty() && out.back() != ' ')
				out += ' ';
		} else {
			out += c;
		}
	}
	if (!out.empty() && out.back() == ' ')
		out.pop_back();
	return out;
}

std::size_t lcs_len(std::string_view a, std::string_view b)
{
	std::vector<std::size_t> row(b.size() + 1, 0);
	for (std::size_t i = 1; i <= a.size(); ++i) {
		std::size_t diag = 0;
		for (std::size_t j = 1; j <= b.size(); ++j) {
			std::size_t const up = row[j];
			row[j] = a[i - 1] == b[j - 1]
			       ? diag + 1
			       : std::max(row[j], row[j - 1]);
			diag = up;
		}
	}
	return row[b.size()];
}

std::vector<std::string_view> codepoints(std::string_view s)
{
	std::vector<std::string_view> out;
	for (std::size_t i = 0; i < s.size();) {
		std::size_t const w = std::min(
			cp_len((unsigned char)s[i]), s.size() - i);
		out.push_back(s.substr(i, w));
		i += w;
	}
	return out;
}

// One aligned position's options with weights; std::map so every
// walk over it is deterministic.
using spot = std::map<std::string, std::size_t>;

// LCS-align @a v against the seed's codepoints and distribute its
// weight: matches and substitutions land on the seed column they
// align to, deletions leave "" there, leftover insertions attach
// before the next anchored column.
void thread_in(std::vector<std::string_view> const &seed,
               std::vector<std::string_view> const &v,
               std::size_t weight, std::vector<spot> &col,
               std::vector<spot> &ins)
{
	std::size_t const n = seed.size(), m = v.size();
	std::vector<std::uint16_t> dp((n + 1) * (m + 1), 0);
	auto at = [&dp, m](std::size_t i, std::size_t j)
		-> std::uint16_t & {
		return dp[i * (m + 1) + j];
	};
	for (std::size_t i = 1; i <= n; ++i)
		for (std::size_t j = 1; j <= m; ++j)
			at(i, j) = seed[i - 1] == v[j - 1]
			         ? std::uint16_t(at(i - 1, j - 1) + 1)
			         : std::max(at(i - 1, j), at(i, j - 1));
	// Anchors in reverse, then replayed forward.
	std::vector<std::pair<std::size_t, std::size_t>> hits;
	for (std::size_t i = n, j = m; i > 0 && j > 0;) {
		if (seed[i - 1] == v[j - 1]
		    && at(i, j) == at(i - 1, j - 1) + 1) {
			hits.push_back({--i, --j});
		} else if (at(i - 1, j) >= at(i, j - 1)) {
			--i;
		} else {
			--j;
		}
	}
	std::ranges::reverse(hits);
	hits.push_back({n, m});
	std::size_t pi = 0, pj = 0;
	for (auto const [i, j] : hits) {
		std::size_t const ga = i - pi, gb = j - pj;
		std::size_t const sub = std::min(ga, gb);
		for (std::size_t k = 0; k < sub; ++k)
			col[pi + k][std::string(v[pj + k])] += weight;
		for (std::size_t k = sub; k < ga; ++k)
			col[pi + k][{}] += weight;
		if (gb > sub) {
			std::string extra;
			for (std::size_t k = sub; k < gb; ++k)
				extra += v[pj + k];
			ins[i][std::move(extra)] += weight;
		}
		if (i < n)
			col[i][std::string(v[j])] += weight;
		pi = i + 1;
		pj = j + 1;
	}
}

bool ws_only(std::string_view s)
{
	return std::ranges::all_of(s, [](char c) {
		return ascii_ws((unsigned char)c);
	});
}

// A run of whitespace-only spots becomes one \s quantifier: + when
// some column in the run forces every variant to spend a character
// there, * when each spot could sit empty.
struct wsrun {
	bool open   = false;
	bool forced = false;

	void flush(std::string &pat)
	{
		if (open)
			pat += forced ? "\\s+" : "\\s*";
		open = false;
		forced = false;
	}
};

// One spot into the growing consensus and pattern.  Options are
// weight-ranked for the consensus (ties to the map's lexical
// order) and emitted smallest-form for the pattern.
void spot_out(spot const &s, bool column, std::string &con,
              std::string &pat, wsrun &run)
{
	std::string_view top;
	std::size_t best = 0;
	bool gap = false, wide = false;
	std::size_t named = 0;
	for (auto const &[o, w] : s) {
		if (!w)
			continue;
		if (w > best) {
			best = w;
			top = o;
		}
		if (o.empty()) {
			gap = true;
			continue;
		}
		++named;
		if (o.size() != 1 || (unsigned char)o[0] >= 0x80)
			wide = true;
	}
	con += top;
	if (!named)
		return;
	bool const ws = std::ranges::all_of(s, [](auto const &e) {
		return !e.second || ws_only(e.first);
	});
	if (ws) {
		run.open = true;
		run.forced = run.forced || (column && !gap);
		return;
	}
	run.flush(pat);
	if (named == 1 && !gap) {
		esc_to(pat, top);
		return;
	}
	if (named == 1) {
		for (auto const &[o, w] : s)
			if (w && !o.empty())
				pat += opt_word(o);
		return;
	}
	if (!wide) {
		pat += '[';
		for (auto const &[o, w] : s) {
			if (!w || o.empty())
				continue;
			char const c = o[0];
			if (c == '\\' || c == ']' || c == '^' || c == '-')
				pat += '\\';
			pat += c;
		}
		pat += ']';
	} else {
		pat += "(?:";
		bool sep = false;
		for (auto const &[o, w] : s) {
			if (!w || o.empty())
				continue;
			if (sep)
				pat += '|';
			sep = true;
			esc_to(pat, o);
		}
		pat += ')';
	}
	if (gap)
		pat += '?';
}

} // namespace

double alike(std::string_view a, std::string_view b)
{
	std::string const x = squeeze(a);
	std::string const y = squeeze(b);
	if (x.empty() && y.empty())
		return 1.0;
	return 2.0 * double(lcs_len(x, y))
	     / double(x.size() + y.size());
}

weave braid(std::vector<std::string> const &variants)
{
	if (variants.empty())
		return {};
	std::map<std::string_view, std::size_t> tally;
	for (std::string const &v : variants)
		++tally[v];
	std::string_view seed;
	std::size_t lead = 0;
	for (auto const &[t, n] : tally) {
		if (n > lead) {
			lead = n;
			seed = t;
		}
	}
	constexpr double kFloor = 0.8;
	for (auto const &[t, n] : tally) {
		if (t.size() > kBraidCap || alike(seed, t) < kFloor)
			return {std::string(seed), esc(seed)};
	}
	std::vector<std::string_view> const scp = codepoints(seed);
	std::vector<spot> col(scp.size());
	std::vector<spot> ins(scp.size() + 1);
	std::size_t total = 0;
	for (auto const &[t, n] : tally) {
		total += n;
		thread_in(scp, codepoints(t), n, col, ins);
	}
	// A slot nobody inserted at was silently skipped by everyone.
	for (spot &s : ins) {
		std::size_t used = 0;
		for (auto const &[o, w] : s)
			used += w;
		s[{}] += total - used;
	}
	weave out;
	wsrun run;
	for (std::size_t i = 0; i <= scp.size(); ++i) {
		spot_out(ins[i], false, out.consensus, out.pattern, run);
		if (i < scp.size())
			spot_out(col[i], true, out.consensus,
			         out.pattern, run);
	}
	run.flush(out.pattern);
	return out;
}

} // namespace rx
