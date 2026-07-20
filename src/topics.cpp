// topics.cpp -- see topics.hpp for the format.
#include "topics.hpp"

#include <algorithm>
#include <cstddef>
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

bool name_ok(std::string_view name)
{
	if (name.empty())
		return false;
	for (char c : name)
		if (ws(c) || c == '{' || c == '}' || c == ':')
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
		while (q < frag.size() && name_ok(frag.substr(q, 1)))
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
// Pattern structure is only read where PCRE would: backslash escapes
// consume the next byte and [...] classes hide everything inside.

std::vector<std::string_view> branches(std::string_view body)
{
	std::vector<std::string_view> out;
	std::size_t start = 0;
	int depth = 0;
	bool cls = false;
	for (std::size_t i = 0; i < body.size(); ++i) {
		char const c = body[i];
		if (c == '\\') {
			++i;
			continue;
		}
		if (cls) {
			cls = c != ']';
			continue;
		}
		if (c == '[' || c == '(' || c == ')') {
			cls = c == '[';
			depth += c == '(' ? 1 : c == ')' ? -1 : 0;
			continue;
		}
		if (c == '|' && depth == 0) {
			out.push_back(body.substr(start, i - start));
			start = i + 1;
		}
	}
	out.push_back(body.substr(start));
	return out;
}

// The branch is exactly one capturing group: "(...)" spanning all of
// it, and not a "(?..." construct.
bool whole_group(std::string_view b)
{
	if (b.size() < 2 || b[0] != '(' || b[1] == '?')
		return false;
	int depth = 0;
	bool cls = false;
	for (std::size_t i = 0; i < b.size(); ++i) {
		char const c = b[i];
		if (c == '\\') {
			++i;
			continue;
		}
		if (cls) {
			cls = c != ']';
			continue;
		}
		if (c == '[')
			cls = true;
		else if (c == '(')
			++depth;
		else if (c == ')' && --depth == 0)
			return i == b.size() - 1;
	}
	return false;
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

std::vector<export_item> export_plan(doc const &d)
{
	// A referenced topic is a component, not a grouping of its own.
	std::vector<char> component(d.topics.size(), 0);
	for (topic const &t : d.topics) {
		for (std::string const &f : t.fragments) {
			std::size_t at = 0;
			for (ref r; next_ref(f, at, r); at = r.pos + r.len)
				component[std::size_t(find(d, r.name)
				          - d.topics.data())] = 1;
		}
	}
	std::vector<export_item> plan;
	for (std::size_t i = 0; i < d.topics.size(); ++i)
		if (!component[i])
			plan.push_back(plan_one(d, d.topics[i]));
	return plan;
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
