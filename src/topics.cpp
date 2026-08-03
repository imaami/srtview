// topics.cpp -- see topics.hpp for the format.
#include "topics.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>

namespace topics {

namespace {

constexpr std::string_view kFile = "file://";
constexpr std::string_view kBom = "\xEF\xBB\xBF";

bool ws(char c)
{
	return c == ' ' || c == '\t';
}

std::string_view strip(std::string_view v)
{
	while (!v.empty() && (ws(v.back()) || v.back() == '\r'))
		v.remove_suffix(1);
	while (!v.empty() && ws(v.front()))
		v.remove_prefix(1);
	return v;
}

// One line per call, counting for diagnostics; CRLF, LF and bare-CR
// all handled (hand-written files travel through Windows).
struct cursor {
	std::string_view rest;
	int              line = 0;

	bool next(std::string_view &out)
	{
		if (rest.empty())
			return false;
		++line;
		std::size_t const n = rest.find_first_of("\r\n");
		out = rest.substr(0, n);
		rest.remove_prefix(std::min(n, rest.size()));
		if (rest.starts_with("\r\n")) {
			rest.remove_prefix(2);
			return true;
		}
		if (!rest.empty())
			rest.remove_prefix(1);
		return true;
	}
};

bool name_char(char c)
{
	return !ws(c) && c != '{' && c != '}' && c != ':'
	    && c != '/' && c != '\\';
}

// Names are identifiers, never filesystem paths: export mints a
// directory per topic, so separators and dot components would turn
// a crafted topic file into an arbitrary-path write primitive.
bool name_ok(std::string_view name)
{
	if (name.empty() || name == "." || name == "..")
		return false;
	for (char c : name)
		if (!name_char(c))
			return false;
	return true;
}

// One reference token: \{name:}.  Any other use of \{ is an escaped
// brace and stays regex as written.
struct ref {
	std::size_t      pos = 0;  // of the backslash
	std::size_t      len = 0;  // of the whole token
	std::string_view name;
};

bool next_ref(std::string_view frag, std::size_t from, ref &out)
{
	for (std::size_t p = frag.find("\\{", from);
	     p != std::string_view::npos; p = frag.find("\\{", p + 2)) {
		std::size_t q = p + 2;
		while (q < frag.size() && name_char(frag[q]))
			++q;
		if (q == p + 2 || !frag.substr(q).starts_with(":}"))
			continue;
		out.pos = p;
		out.len = q + 2 - p;
		out.name = frag.substr(p + 2, q - (p + 2));
		return true;
	}
	return false;
}

// Parser state: heads open blocks, children fill them, finish()
// resolves everything that may point forward.
struct parser {
	enum block { none, in_topic, in_video };

	doc              d;
	std::string      error;
	std::vector<int> topic_line;      // head line per topic
	struct pending {
		std::string name;         // referenced topic
		int         line;
	};
	std::vector<pending> refs;        // video + fragment references
	int              err_line = 0;
	int              line = 0;
	block            open = none;

	bool ok() const { return error.empty(); }

	bool fail(std::string msg, int at = 0)
	{
		error = std::move(msg);
		err_line = at ? at : line;
		return false;
	}

	bool feed(std::string_view raw, int at)
	{
		line = at;
		std::string_view const s = strip(raw);
		if (s.empty() || s.front() == '#')
			return true;
		if (!s.starts_with("- "))
			return fail("expected a \"- \" item");
		std::string_view const content = strip(s.substr(2));
		if (content.empty())
			return fail("empty item");
		if (raw.front() == '-')
			return head(content);
		if (open == none)
			return fail("indented item outside any block");
		return open == in_topic ? fragment(content)
		                        : detail(content);
	}

	bool head(std::string_view content)
	{
		if (content.starts_with(kFile))
			return video_head(content.substr(kFile.size()));
		if (!name_ok(content))
			return fail("topic names cannot contain "
			            "whitespace, braces or colons");
		if (find(d, content))
			return fail("duplicate topic");
		d.topics.push_back({std::string(content), {}});
		topic_line.push_back(line);
		open = in_topic;
		return true;
	}

	bool video_head(std::string_view path)
	{
		if (path.empty())
			return fail("empty file path");
		for (video const &v : d.videos)
			if (v.path == path)
				return fail("duplicate video");
		d.videos.push_back({std::string(path), {}, {}});
		open = in_video;
		return true;
	}

	// Topic child: a regex fragment; collect its references.
	bool fragment(std::string_view content)
	{
		std::size_t at = 0;
		for (ref r; next_ref(content, at, r); at = r.pos + r.len)
			refs.push_back({std::string(r.name), line});
		d.topics.back().fragments.emplace_back(content);
		return true;
	}

	// Video child: the subtitle file or an applying topic's name.
	bool detail(std::string_view content)
	{
		video &v = d.videos.back();
		if (content.starts_with(kFile))
			return srt_detail(v, content.substr(kFile.size()));
		if (!name_ok(content))
			return fail("expected a file:// path or a topic "
			            "name");
		for (std::string const &n : v.topics)
			if (n == content)
				return fail("duplicate topic reference");
		v.topics.emplace_back(content);
		refs.push_back({std::string(content), line});
		return true;
	}

	bool srt_detail(video &v, std::string_view path)
	{
		if (path.empty())
			return fail("empty file path");
		if (!v.srt.empty())
			return fail("second subtitle file");
		v.srt = path;
		return true;
	}

	bool finish()
	{
		for (std::size_t i = 0; i < d.topics.size(); ++i)
			if (d.topics[i].fragments.empty())
				return fail("topic \"" + d.topics[i].name
				            + "\" has no pattern",
				            topic_line[i]);
		for (pending const &p : refs)
			if (!find(d, p.name))
				return fail("unknown topic \"" + p.name
				            + "\"", p.line);
		return acyclic();
	}

	// White/gray/black DFS over topic references.
	bool acyclic()
	{
		std::vector<char> state(d.topics.size(), 0);
		for (std::size_t i = 0; i < d.topics.size(); ++i)
			if (!dfs(i, state))
				return fail("circular reference involving "
				            "topic \"" + d.topics[i].name
				            + "\"", topic_line[i]);
		return true;
	}

	bool dfs(std::size_t i, std::vector<char> &state)
	{
		if (state[i])
			return state[i] == 2;
		state[i] = 1;
		for (std::string const &f : d.topics[i].fragments)
			if (!dfs_frag(f, state))
				return false;
		state[i] = 2;
		return true;
	}

	bool dfs_frag(std::string_view frag, std::vector<char> &state)
	{
		std::size_t at = 0;
		for (ref r; next_ref(frag, at, r); at = r.pos + r.len)
			if (!dfs(index_of(r.name), state))
				return false;
		return true;
	}

	std::size_t index_of(std::string_view name) const
	{
		return std::size_t(find(d, name) - d.topics.data());
	}
};

void expand_into(doc const &d, topic const &t, std::string &out);

void expand_frag(doc const &d, std::string_view frag, std::string &out)
{
	std::size_t at = 0;
	for (ref r; next_ref(frag, at, r); at = r.pos + r.len) {
		out += frag.substr(at, r.pos - at);
		out += "(?:";
		expand_into(d, *find(d, r.name), out);
		out += ')';
	}
	out += frag.substr(at);
}

void expand_into(doc const &d, topic const &t, std::string &out)
{
	for (std::string const &f : t.fragments)
		expand_frag(d, f, out);
}

// --- export-plan analysis -------------------------------------------

// Cursor over a pattern's structural bytes: escapes consume their
// follower and [...] classes swallow their content, so parens and
// pipes are reported only where PCRE sees them, with the depth
// after the byte.
struct rx_cursor {
	std::string_view s;
	std::size_t      i = 0;
	int              depth = 0;
	bool             cls = false;

	bool next(char &c)
	{
		while (i < s.size()) {
			c = s[i++];
			if (c == '\\') {
				++i;
				continue;
			}
			if (cls) {
				cls = c != ']';
				continue;
			}
			if (c == '[') {
				cls = true;
				continue;
			}
			depth += c == '(' ? 1 : c == ')' ? -1 : 0;
			return true;
		}
		return false;
	}
};

std::vector<std::string_view> branches(std::string_view body)
{
	std::vector<std::string_view> out;
	std::size_t start = 0;
	char c;
	for (rx_cursor cur{body}; cur.next(c);) {
		if (c != '|' || cur.depth != 0)
			continue;
		out.push_back(body.substr(start, cur.i - 1 - start));
		start = cur.i;
	}
	out.push_back(body.substr(start));
	return out;
}

// The first depth-zero ')' is the final byte: one whole group.
bool spans_whole(std::string_view b)
{
	char c;
	for (rx_cursor cur{b}; cur.next(c);)
		if (c == ')' && cur.depth == 0)
			return cur.i == b.size();
	return false;
}

// The branch is exactly one capturing group: "(...)" spanning all of
// it, and not a "(?..." construct.
bool whole_group(std::string_view b)
{
	return b.size() >= 2 && b[0] == '(' && b[1] != '?'
	    && spans_whole(b);
}

// Exactly one reference inside: the acknowledged component.
std::string_view sole_ref(std::string_view inner)
{
	ref r;
	if (!next_ref(inner, 0, r))
		return {};
	ref more;
	if (next_ref(inner, r.pos + r.len, more))
		return {};
	return r.name;
}

export_item plan_one(doc const &d, topic const &t)
{
	std::string body;
	for (std::string const &f : t.fragments)
		body += f;
	export_item it;
	it.name = t.name;
	bool first = true;
	for (std::string_view const b : branches(body)) {
		if (!first)
			it.pattern += '|';
		first = false;
		std::string_view const name = whole_group(b)
			? sole_ref(b.substr(1, b.size() - 2))
			: std::string_view{};
		if (name.empty()) {
			expand_frag(d, b, it.pattern);
			continue;
		}
		it.pattern += "(?<g" + std::to_string(it.parts.size())
		            + ">";
		expand_frag(d, b.substr(1, b.size() - 2), it.pattern);
		it.pattern += ')';
		it.parts.emplace_back(name);
	}
	return it;
}

// --- normalization --------------------------------------------------
// Comparison keys for "fuzzy but accurate" pattern identity: enough
// canonicalization to see through branch order, redundant wrapping
// and the house case idioms, and no more -- a doubtful structure
// falls back to exact text.  Keys compare; they are never stored,
// exported or hashed into an identity.

// Locale-free ASCII classification: the Qt host runs
// setlocale(LC_ALL, ""), and keys must not shift with it.
constexpr bool ascii_alpha(unsigned char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

constexpr bool ascii_digit(unsigned char c)
{
	return c >= '0' && c <= '9';
}

constexpr char ascii_lower(unsigned char c)
{
	return char(c >= 'A' && c <= 'Z' ? c + 32 : c);
}

// Flattening depth past anything the pipeline writes: deeper
// nesting reads as structure we should not pretend to understand.
constexpr int kFlattenDepth = 8;

// Peels a leading "(?i)" and full-span (?i:X), (?:X) and (X)
// wrappers; the case-insensitive shapes set ci.
std::string_view peel(std::string_view b, bool &ci)
{
	for (;;) {
		if (b.size() > 4 && !b.compare(0, 4, "(?i)")) {
			ci = true;
			b = b.substr(4);
			continue;
		}
		std::size_t skip;
		if (b.size() > 5 && !b.compare(0, 4, "(?i:"))
			skip = 4;
		else if (b.size() > 4 && !b.compare(0, 3, "(?:"))
			skip = 3;
		else if (b.size() > 2 && b[0] == '(' && b[1] != '?')
			skip = 1;
		else
			return b;
		if (!spans_whole(b))
			return b;
		ci = ci || skip == 4;
		b = b.substr(skip, b.size() - skip - 1);
	}
}

// The index of the ']' closing a class opened before @a at, escapes
// honored; npos when unterminated.  Shares rx_cursor's blind spot
// for a literal leading ']'.
std::size_t class_end(std::string_view b, std::size_t at)
{
	for (; at < b.size(); ++at) {
		if (b[at] == '\\') {
			++at;
			continue;
		}
		if (b[at] == ']')
			return at;
	}
	return std::string_view::npos;
}

// Canonical key text of a class, or empty to keep it verbatim.
// Membership is order-free once parsed, so the members land in a
// bitmap (negation noted, ranges expanded, literal escapes decoded,
// class-type escapes like \d kept as opaque set members) and are
// re-emitted deterministically: ascending bytes, runs of three or
// more as ranges, the metacharacters \ ] ^ - always escaped so no
// position rule survives into the key.  Under ci the letter bits
// fold to lowercase.  Beyond-the-rules content bails to verbatim,
// which is always sound: non-ASCII (UTF-8 units, not bytes),
// numeric escapes, \p and unknown letter escapes, POSIX [:...:]
// classes (class_end() cannot deliver one whole), inverted ranges.
std::string canon_class(std::string_view cls, bool ci)
{
	bool const neg = !cls.empty() && cls[0] == '^';
	if (cls.size() <= std::size_t{neg})
		return {};
	bool map[256] = {};
	std::vector<std::string> opaque;
	int prev = -1;   /* pending single member */
	bool dash = false;
	auto flush = [&] {
		if (prev >= 0)
			map[prev] = true;
		prev = -1;
	};
	for (std::size_t i = neg; i < cls.size();) {
		unsigned char const c = cls[i];
		if (c >= 0x80)
			return {};
		if (c == '-' && !dash && prev >= 0 &&
		    i + 1 < cls.size()) {
			dash = true;
			++i;
			continue;
		}
		int one = -1;
		if (c == '[' && i + 1 < cls.size() && cls[i + 1] == ':')
			// class_end() cannot carry a complete [:...:]
			// token past its inner ']': whatever reaches here
			// is a truncated POSIX class, and canonicalizing
			// its debris as literal members would forge
			// equalities.  Verbatim is the sound key.
			return {};
		if (c == '\\') {
			if (i + 1 >= cls.size())
				return {};
			unsigned char const e = cls[i + 1];
			if (ascii_alpha(e) || ascii_digit(e)) {
				int byte = 0;
				switch (e) {
				case 'a': byte = 7;  break;
				case 'b': byte = 8;  break;
				case 't': byte = 9;  break;
				case 'n': byte = 10; break;
				case 'f': byte = 12; break;
				case 'r': byte = 13; break;
				case 'e': byte = 27; break;
				}
				if (!byte) {
					if (dash ||
					    !std::strchr("dDsSwWhHvV",
					                 e))
						return {};
					flush();
					opaque.push_back({'\\',
					                  char(e)});
					i += 2;
					continue;
				}
				one = byte;
			} else {
				one = e;
			}
			i += 2;
		} else {
			one = c;
			++i;
		}
		if (dash) {
			if (prev < 0 || one < prev)
				return {};
			for (int b = prev; b <= one; ++b)
				map[b] = true;
			prev = -1;
			dash = false;
		} else {
			flush();
			prev = one;
		}
	}
	flush();
	if (ci)
		for (int c = 'A'; c <= 'Z'; ++c)
			if (map[c]) {
				map[c] = false;
				map[c + 32] = true;
			}
	std::ranges::sort(opaque);
	auto const dup = std::ranges::unique(opaque);
	opaque.erase(dup.begin(), dup.end());
	std::string out = neg ? "[^" : "[";
	auto emit = [&](int b) {
		if (b == '\\' || b == ']' || b == '^' || b == '-')
			out += '\\';
		out += char(b);
	};
	for (int b = 0; b < 256;) {
		if (!map[b]) {
			++b;
			continue;
		}
		int run = b;
		while (run + 1 < 256 && map[run + 1])
			++run;
		if (run - b >= 2) {
			emit(b);
			out += '-';
			emit(run);
		} else
			for (int m = b; m <= run; ++m)
				emit(m);
		b = run + 1;
	}
	for (std::string const &o : opaque)
		out += o;
	out += ']';
	return out;
}

// [Xx]: exactly the upper and lower of one ASCII letter -- the house
// idiom that marks deliberate case coverage.
bool case_idiom(std::string_view cls, char &low)
{
	if (cls.size() != 2)
		return false;
	unsigned char const a = (unsigned char)cls[0];
	unsigned char const b = (unsigned char)cls[1];
	if (a == b || !ascii_alpha(a) || !ascii_alpha(b) ||
	    ascii_lower(a) != ascii_lower(b))
		return false;
	low = ascii_lower(a);
	return true;
}

// One branch's key: escapes and classes verbatim (the idiom folds
// to its letter), other bytes lowered under ci.  The idiom also
// makes its branch case-insensitive, so [Qq]uorum and (?i:quorum)
// meet in the middle; folded keys carry an "i:" tag so a case-
// sensitive branch can never collide with them.
std::string branch_key(std::string_view b, bool ci)
{
	for (std::size_t i = 0; !ci && i < b.size();) {
		if (b[i] == '\\') {
			i += 2;
			continue;
		}
		if (b[i] != '[') {
			++i;
			continue;
		}
		std::size_t const end = class_end(b, i + 1);
		char low;
		if (end == std::string_view::npos)
			break;
		ci = case_idiom(b.substr(i + 1, end - i - 1), low);
		i = end + 1;
	}
	// The fold tag is a newline: adopt() refuses patterns carrying
	// one and the topic-file format is line-based, so no corpus
	// pattern can begin with it -- a printable tag ("i:") would be
	// forgeable by a literal branch and cause a false subtraction.
	std::string out = ci ? "\n" : "";
	for (std::size_t i = 0; i < b.size();) {
		char const c = b[i];
		if (c == '\\') {
			out.append(b.substr(i, 2));
			i += 2;
			continue;
		}
		if (c == '[') {
			std::size_t const end = class_end(b, i + 1);
			if (end == std::string_view::npos) {
				out.append(b.substr(i));
				break;
			}
			std::string_view const cls =
				b.substr(i + 1, end - i - 1);
			char low;
			if (case_idiom(cls, low))
				out += low;
			else if (std::string canon = canon_class(cls, ci);
			         !canon.empty())
				out += canon;
			else
				out.append(b.substr(i, end - i + 1));
			i = end + 1;
			continue;
		}
		out += ci ? ascii_lower((unsigned char)c) : c;
		++i;
	}
	return out;
}

struct part {
	std::string text; // rebuild text, wrappers peeled
	std::string key;  // comparison key
};

// Flattens a peeled pattern into top-level parts.  False on
// structural doubt (empty branch, runaway nesting): the caller then
// treats the whole pattern as one opaque part.
bool flatten(std::string_view s, bool ci, int depth,
             std::vector<part> &out)
{
	if (depth > kFlattenDepth)
		return false;
	for (std::string_view const b : branches(s)) {
		if (b.empty())
			return false;
		bool bci = ci;
		std::string_view const p = peel(b, bci);
		if (bci != ci)
			// A branch-local (?i:...) stays opaque: splitting
			// it would need per-branch re-wrapping on rebuild.
			out.push_back({std::string(b),
			               branch_key(p, true)});
		else if (p.size() != b.size()) {
			if (!flatten(p, ci, depth + 1, out))
				return false;
		} else {
			out.push_back({std::string(p),
			               branch_key(p, ci)});
		}
	}
	return true;
}

// Group references dangle or renumber when a capturing wrapper
// peels away: numeric and \g/\k backreferences, recursion and
// conditionals all pin group identity.  The walk is escape-aware
// but class-blind; a false hit only costs verbatim keys.  (?- and
// (?+ need a digit next, so inline modifiers like (?-i) pass.
bool references_groups(std::string_view s)
{
	for (std::size_t i = 0; i + 1 < s.size(); ++i) {
		unsigned char const a = s[i];
		unsigned char const b = s[i + 1];
		if (a == '\\') {
			if (ascii_digit(b) || b == 'g' || b == 'k')
				return true;
			++i;
			continue;
		}
		if (a != '(' || b != '?' || i + 2 >= s.size())
			continue;
		unsigned char const c = s[i + 2];
		if (ascii_digit(c) || c == 'R' || c == '&' ||
		    c == 'P' || c == '(')
			return true;
		if ((c == '+' || c == '-') && i + 3 < s.size() &&
		    ascii_digit((unsigned char)s[i + 3]))
			return true;
	}
	return false;
}

// The pattern-level entry: outer wrappers peeled once, parts
// collected; ci reports the peeled case flag for the rebuild wrap.
bool flatten_pattern(std::string_view pattern, bool &ci,
                     std::vector<part> &out)
{
	ci = false;
	if (references_groups(pattern))
		return false;
	// Sequenced apart deliberately: peel() writes ci, and argument
	// evaluation order would be free to read it first.
	std::string_view const inner = peel(pattern, ci);
	return flatten(inner, ci, 0, out);
}

} // namespace

result parse(std::string_view text)
{
	if (text.starts_with(kBom))
		text.remove_prefix(kBom.size());
	parser ps;
	cursor cur{text, 0};
	for (std::string_view raw; ps.ok() && cur.next(raw);)
		ps.feed(raw, cur.line);
	if (ps.ok())
		ps.finish();
	if (!ps.ok())
		return {{}, std::move(ps.error), ps.err_line};
	return {std::move(ps.d), {}, 0};
}

topic const *find(doc const &d, std::string_view name)
{
	for (topic const &t : d.topics)
		if (t.name == name)
			return &t;
	return nullptr;
}

std::string expand(doc const &d, topic const &t)
{
	std::string out;
	expand_into(d, t, out);
	return out;
}

namespace {

// A referenced topic is a component, not a grouping of its own;
// the marking is shared by export_plan() and components().
std::vector<char> mark_components(doc const &d)
{
	std::vector<char> component(d.topics.size(), 0);
	for (topic const &t : d.topics) {
		for (std::string const &f : t.fragments) {
			std::size_t at = 0;
			for (ref r; next_ref(f, at, r); at = r.pos + r.len)
				component[std::size_t(find(d, r.name)
				          - d.topics.data())] = 1;
		}
	}
	return component;
}

} // namespace

std::vector<export_item> export_plan(doc const &d)
{
	std::vector<char> const component = mark_components(d);
	std::vector<export_item> plan;
	for (std::size_t i = 0; i < d.topics.size(); ++i)
		if (!component[i])
			plan.push_back(plan_one(d, d.topics[i]));
	return plan;
}

std::vector<topic const *> components(doc const &d)
{
	std::vector<char> const component = mark_components(d);
	std::vector<topic const *> out;
	for (std::size_t i = 0; i < d.topics.size(); ++i)
		if (component[i])
			out.push_back(&d.topics[i]);
	return out;
}

bool adopt(doc &d, std::string const &pattern, char const *stem)
{
	// Edge whitespace and interior line breaks would not survive
	// write()/parse() -- the format strips edges and splits lines
	// -- so adopting either would break the round-trip and orphan
	// the pattern's dive id.  Refused, like references: rewriting
	// instead would change what the pattern matches.
	ref r;
	if (pattern.empty() || strip(pattern) != pattern
	    || pattern.find_first_of("\r\n") != std::string::npos
	    || next_ref(pattern, 0, r))
		return false;
	std::string const key = normal_key(pattern);
	for (topic const &t : d.topics)
		if (normal_key(expand(d, t)) == key)
			return false;

	std::string name;
	for (unsigned n = 1;; ++n) {
		name = stem + std::to_string(n);
		if (!find(d, name))
			break;
	}
	d.topics.push_back({std::move(name), {pattern}});
	return true;
}

std::string normal_key(std::string_view pattern)
{
	bool ci;
	std::vector<part> parts;
	if (!flatten_pattern(pattern, ci, parts))
		return std::string(pattern);
	std::vector<std::string> keys;
	keys.reserve(parts.size());
	for (part &p : parts)
		keys.push_back(std::move(p.key));
	std::ranges::sort(keys);
	auto const dup = std::ranges::unique(keys);
	keys.erase(dup.begin(), dup.end());
	std::string out;
	for (std::string const &k : keys) {
		if (!out.empty())
			out += '|';
		out += k;
	}
	return out;
}

namespace {

// The union of every topic's expanded branch keys -- the covered
// set adopt_novel() and extend() subtract against.  A doubtful
// expansion contributes its whole-pattern key.
std::vector<std::string> corpus_keys(doc const &d)
{
	std::vector<std::string> covered;
	for (topic const &t : d.topics) {
		bool tci;
		std::vector<part> tp;
		std::string const ex = expand(d, t);
		if (flatten_pattern(ex, tci, tp))
			for (part &q : tp)
				covered.push_back(std::move(q.key));
		else
			covered.push_back(normal_key(ex));
	}
	return covered;
}

// Branches of parts whose key the covered set lacks, joined in
// original order -- survivors keep their text, repeats within the
// pattern itself collapse.  Empty when nothing novel remains.
std::string novel_body(std::vector<part> &parts,
                       std::vector<std::string> const &covered)
{
	std::string body;
	std::vector<std::string> kept;
	for (part &q : parts) {
		if (std::ranges::find(covered, q.key) != covered.end() ||
		    std::ranges::find(kept, q.key) != kept.end())
			continue;
		kept.push_back(std::move(q.key));
		if (!body.empty())
			body += '|';
		body += q.text;
	}
	return body;
}

} // namespace

std::string adopt_novel(doc &d, std::string const &pattern,
                        char const *stem)
{
	bool ci;
	std::vector<part> parts;
	if (!flatten_pattern(pattern, ci, parts))
		// Opaque structure: no branch surgery, but adopt()'s
		// key-level duplicate refusal still applies.
		return adopt(d, pattern, stem) ? pattern : std::string();

	std::string body = novel_body(parts, corpus_keys(d));
	if (body.empty())
		return {};
	std::string rebuilt = ci ? "(?i:" + body + ")"
	                         : std::move(body);
	return adopt(d, rebuilt, stem) ? rebuilt : std::string();
}

bool stem_name(std::string_view name, std::string_view stem)
{
	if (!name.starts_with(stem) || name.size() == stem.size())
		return false;
	for (char const c : name.substr(stem.size()))
		if (!ascii_digit((unsigned char)c))
			return false;
	return true;
}

std::string extend(doc &d, std::string_view name,
                   std::string const &pattern)
{
	topic *target = nullptr;
	for (topic &t : d.topics)
		if (t.name == name) {
			target = &t;
			break;
		}
	ref r;
	if (!target || target->fragments.size() != 1
	    || next_ref(target->fragments[0], 0, r))
		return {};
	// adopt()'s hygiene guards: what cannot survive the
	// write()/parse() round trip must not enter a fragment here
	// either.
	if (pattern.empty() || strip(pattern) != pattern
	    || pattern.find_first_of("\r\n") != std::string::npos
	    || next_ref(pattern, 0, r))
		return {};
	bool ci, tci;
	std::vector<part> parts, tp;
	if (!flatten_pattern(pattern, ci, parts)
	    || !flatten_pattern(target->fragments[0], tci, tp)
	    || ci != tci)
		return {};
	std::string const body = novel_body(parts, corpus_keys(d));
	if (body.empty())
		return {};
	// One wrap covers the grown alternation; the peel flag is
	// spent -- ci == tci already pinned the case context.
	bool spent = false;
	std::string const inner{peel(target->fragments[0], spent)};
	target->fragments[0] = ci ? "(?i:" + inner + "|" + body + ")"
	                          : inner + "|" + body;
	return target->fragments[0];
}

std::string cover_of(doc const &d, std::string const &pattern,
                     char const *stem)
{
	bool ci;
	std::vector<part> parts;
	if (!flatten_pattern(pattern, ci, parts))
		return {};
	// Keys carry the fold tag, so case contexts cannot cross-match;
	// strict > keeps the earliest max -- doc order is the only
	// tie-break two sessions share.
	std::string best;
	std::size_t most = 0;
	for (topic const &t : d.topics) {
		if (!stem_name(t.name, stem))
			continue;
		bool tci;
		std::vector<part> tp;
		if (!flatten_pattern(expand(d, t), tci, tp))
			continue;
		auto const covers = [&tp](part const &q) {
			return std::ranges::find(tp, q.key, &part::key)
			       != tp.end();
		};
		std::size_t const n = std::size_t(
			std::ranges::count_if(parts, covers));
		if (n > most) {
			most = n;
			best = t.name;
		}
	}
	return best;
}

std::string tidy(std::string const &pattern)
{
	bool ci;
	std::vector<part> parts;
	if (!flatten_pattern(pattern, ci, parts))
		return pattern;
	std::string body;
	std::vector<std::string> seen;
	for (part &q : parts) {
		// Folded keys decide repeats only under an outer (?i:),
		// where the wrapper covers the case difference; in a
		// case-sensitive context the [Xx] idiom's promotion
		// would let "[Ee]tsy-?d" swallow "[Ee]tsy-?[Dd]" and
		// narrow what the pattern matches.
		std::string const &k = ci ? q.key : q.text;
		if (std::ranges::find(seen, k) != seen.end())
			continue;
		seen.push_back(k);
		if (!body.empty())
			body += '|';
		body += q.text;
	}
	return ci ? "(?i:" + body + ")" : body;
}

std::vector<gloss_entry> parse_gloss(std::string_view text)
{
	if (text.starts_with(kBom))
		text.remove_prefix(kBom.size());
	std::vector<gloss_entry> out;
	cursor cur{text, 0};
	for (std::string_view line; cur.next(line);) {
		std::string_view const s = strip(line);
		if (s.empty() || s.front() == '#' || !s.starts_with("- "))
			continue;
		std::string_view const body = strip(s.substr(2));
		if (line.front() == '-') {
			if (!body.empty())
				out.push_back({std::string(body), {}});
		} else if (!out.empty()) {
			out.back().lines.push_back(std::string(body));
		}
	}
	return out;
}

std::string write_gloss(std::vector<gloss_entry> const &g)
{
	std::string out;
	for (gloss_entry const &e : g) {
		if (e.name.empty())
			continue;
		if (!out.empty())
			out += '\n';
		out += "- " + e.name + '\n';
		for (std::string const &l : e.lines)
			out += "  - " + l + '\n';
	}
	return out;
}

std::string write(doc const &d)
{
	std::string out;
	for (topic const &t : d.topics) {
		if (!out.empty())
			out += '\n';
		out += "- ";
		out += t.name;
		out += '\n';
		for (std::string const &f : t.fragments)
			out.append("  - ").append(f).append(1, '\n');
	}
	for (video const &v : d.videos) {
		if (!out.empty())
			out += '\n';
		out.append("- ").append(kFile).append(v.path)
		   .append(1, '\n');
		if (!v.srt.empty())
			out.append("  - ").append(kFile).append(v.srt)
			   .append(1, '\n');
		for (std::string const &n : v.topics)
			out.append("  - ").append(n).append(1, '\n');
	}
	return out;
}

} // namespace topics
