// semantic.cpp -- see semantic.hpp.
#include "semantic.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>

#include "slurp.hpp"
#include "unicode.hpp"

namespace semantic {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kMaxDepth     = 48;
constexpr std::size_t kMaxRecords   = 64;
constexpr std::size_t kMaxCitations = 64;
// The schemas cap subject, statement and rationale in characters
// -- the grammar counts code points -- while the parser caps bytes,
// so it allows the UTF-8 worst case of four per character: a reply
// the grammar admitted must never be refused here, or it would be
// cached, rejected, and re-asked forever.
constexpr std::size_t kMaxSubject   = 4 * 256;
constexpr std::size_t kMaxObject    = 4 * 1024;
constexpr std::size_t kMaxStatement = 4 * 4096;

struct json {
	enum class type : std::uint8_t {
		null, boolean, number, string, array, object
	};

	type kind = type::null;
	bool boolean = false;
	double number = 0.0;
	std::string string;
	std::vector<json> array;
	std::vector<std::pair<std::string, json>> object;

	json const *member(std::string_view key) const
	{
		if (kind != type::object)
			return nullptr;
		for (auto const &[name, value] : object)
			if (name == key)
				return &value;
		return nullptr;
	}
};

void utf8(std::string &out, std::uint32_t cp)
{
	if (cp <= 0x7f) {
		out.push_back(char(cp));
	} else if (cp <= 0x7ff) {
		out.push_back(char(0xc0 | cp >> 6));
		out.push_back(char(0x80 | (cp & 0x3f)));
	} else if (cp <= 0xffff) {
		out.push_back(char(0xe0 | cp >> 12));
		out.push_back(char(0x80 | (cp >> 6 & 0x3f)));
		out.push_back(char(0x80 | (cp & 0x3f)));
	} else {
		out.push_back(char(0xf0 | cp >> 18));
		out.push_back(char(0x80 | (cp >> 12 & 0x3f)));
		out.push_back(char(0x80 | (cp >> 6 & 0x3f)));
		out.push_back(char(0x80 | (cp & 0x3f)));
	}
}

class json_parser {
public:
	explicit json_parser(std::string_view text)
		: m_text(text) {}

	bool parse(json &out)
	{
		space();
		if (!value(out, 0))
			return false;
		space();
		return m_at == m_text.size();
	}

private:
	void space()
	{
		while (m_at < m_text.size()
		       && (m_text[m_at] == ' ' || m_text[m_at] == '\t'
		           || m_text[m_at] == '\r' || m_text[m_at] == '\n'))
			++m_at;
	}

	bool take(char c)
	{
		space();
		if (m_at >= m_text.size() || m_text[m_at] != c)
			return false;
		++m_at;
		return true;
	}

	bool literal(std::string_view word)
	{
		if (!m_text.substr(m_at).starts_with(word))
			return false;
		m_at += word.size();
		return true;
	}

	static int hex(char c)
	{
		if (c >= '0' && c <= '9')
			return c - '0';
		if (c >= 'a' && c <= 'f')
			return c - 'a' + 10;
		if (c >= 'A' && c <= 'F')
			return c - 'A' + 10;
		return -1;
	}

	bool hex4(std::uint32_t &cp)
	{
		if (m_text.size() - m_at < 4)
			return false;
		cp = 0;
		for (int i = 0; i < 4; ++i) {
			int const x = hex(m_text[m_at++]);
			if (x < 0)
				return false;
			cp = cp * 16 + std::uint32_t(x);
		}
		return true;
	}

	bool string(std::string &out)
	{
		if (!take('"'))
			return false;
		out.clear();
		while (m_at < m_text.size()) {
			unsigned char const c = m_text[m_at++];
			if (c == '"')
				return true;
			if (c < 0x20)
				return false;
			if (c != '\\') {
				out.push_back(char(c));
				continue;
			}
			if (m_at >= m_text.size())
				return false;
			switch (char const e = m_text[m_at++]; e) {
			case '"': case '\\': case '/': out.push_back(e); break;
			case 'b': out.push_back('\b'); break;
			case 'f': out.push_back('\f'); break;
			case 'n': out.push_back('\n'); break;
			case 'r': out.push_back('\r'); break;
			case 't': out.push_back('\t'); break;
			case 'u': {
				std::uint32_t cp;
				if (!hex4(cp))
					return false;
				if (cp >= 0xd800 && cp <= 0xdbff) {
					if (m_text.size() - m_at < 6
					    || m_text[m_at] != '\\'
					    || m_text[m_at + 1] != 'u')
						return false;
					m_at += 2;
					std::uint32_t low;
					if (!hex4(low) || low < 0xdc00
					    || low > 0xdfff)
						return false;
					cp = 0x10000 + ((cp - 0xd800) << 10)
					   + low - 0xdc00;
				} else if (cp >= 0xdc00 && cp <= 0xdfff) {
					return false;
				}
				utf8(out, cp);
				break;
			}
			default: return false;
			}
		}
		return false;
	}

	bool number(double &out)
	{
		std::size_t const first = m_at;
		if (m_at < m_text.size() && m_text[m_at] == '-')
			++m_at;
		if (m_at >= m_text.size())
			return false;
		if (m_text[m_at] == '0') {
			++m_at;
		} else {
			if (m_text[m_at] < '1' || m_text[m_at] > '9')
				return false;
			while (m_at < m_text.size()
			       && m_text[m_at] >= '0' && m_text[m_at] <= '9')
				++m_at;
		}
		if (m_at < m_text.size() && m_text[m_at] == '.') {
			++m_at;
			std::size_t const digits = m_at;
			while (m_at < m_text.size()
			       && m_text[m_at] >= '0' && m_text[m_at] <= '9')
				++m_at;
			if (m_at == digits)
				return false;
		}
		if (m_at < m_text.size()
		    && (m_text[m_at] == 'e' || m_text[m_at] == 'E')) {
			++m_at;
			if (m_at < m_text.size()
			    && (m_text[m_at] == '+' || m_text[m_at] == '-'))
				++m_at;
			std::size_t const digits = m_at;
			while (m_at < m_text.size()
			       && m_text[m_at] >= '0' && m_text[m_at] <= '9')
				++m_at;
			if (m_at == digits)
				return false;
		}
		auto const [p, ec] = std::from_chars(
			m_text.data() + first, m_text.data() + m_at, out,
			std::chars_format::general);
		return ec == std::errc() && p == m_text.data() + m_at
		    && std::isfinite(out);
	}

	bool value(json &out, std::size_t depth)
	{
		if (depth >= kMaxDepth)
			return false;
		space();
		if (m_at >= m_text.size())
			return false;
		if (m_text[m_at] == '"') {
			out.kind = json::type::string;
			return string(out.string);
		}
		if (m_text[m_at] == '{')
			return object(out, depth + 1);
		if (m_text[m_at] == '[')
			return array(out, depth + 1);
		if (literal("true")) {
			out.kind = json::type::boolean;
			out.boolean = true;
			return true;
		}
		if (literal("false")) {
			out.kind = json::type::boolean;
			out.boolean = false;
			return true;
		}
		if (literal("null")) {
			out.kind = json::type::null;
			return true;
		}
		out.kind = json::type::number;
		return number(out.number);
	}

	bool array(json &out, std::size_t depth)
	{
		if (!take('['))
			return false;
		out.kind = json::type::array;
		out.array.clear();
		space();
		if (take(']'))
			return true;
		for (;;) {
			out.array.emplace_back();
			if (!value(out.array.back(), depth))
				return false;
			space();
			if (take(']'))
				return true;
			if (!take(','))
				return false;
		}
	}

	bool object(json &out, std::size_t depth)
	{
		if (!take('{'))
			return false;
		out.kind = json::type::object;
		out.object.clear();
		space();
		if (take('}'))
			return true;
		for (;;) {
			std::string key;
			if (!string(key) || !take(':'))
				return false;
			for (auto const &[seen, ignored] : out.object) {
				(void)ignored;
				if (seen == key)
					return false;
			}
			out.object.emplace_back(std::move(key), json{});
			if (!value(out.object.back().second, depth))
				return false;
			space();
			if (take('}'))
				return true;
			if (!take(','))
				return false;
		}
	}

	std::string_view m_text;
	std::size_t m_at = 0;
};

bool exact_object(json const &v,
	              std::initializer_list<std::string_view> keys)
{
	if (v.kind != json::type::object || v.object.size() != keys.size())
		return false;
	return std::ranges::all_of(keys, [&v](std::string_view key) {
		return v.member(key);
	});
}

bool text(json const *v, std::string &out, std::size_t cap,
	      bool empty = false)
{
	if (!v || v->kind != json::type::string || v->string.size() > cap)
		return false;
	std::size_t a = 0, b = v->string.size();
	while (a < b && (v->string[a] == ' ' || v->string[a] == '\t'
	                 || v->string[a] == '\r' || v->string[a] == '\n'))
		++a;
	while (b > a && (v->string[b - 1] == ' '
	                 || v->string[b - 1] == '\t'
	                 || v->string[b - 1] == '\r'
	                 || v->string[b - 1] == '\n'))
		--b;
	if (!empty && a == b)
		return false;
	out.assign(v->string, a, b - a);
	return true;
}

bool number(json const *v, double &out)
{
	if (!v || v->kind != json::type::number || !std::isfinite(v->number))
		return false;
	out = v->number;
	return true;
}

bool integer(json const *v, std::uint32_t &out)
{
	double n;
	if (!number(v, n) || n < 0.0
	    || n > double(std::numeric_limits<std::uint32_t>::max())
	    || std::floor(n) != n)
		return false;
	out = std::uint32_t(n);
	return true;
}

bool to_kind(std::string_view s, kind &out)
{
	for (kind const k : {kind::claim, kind::relationship,
	                     kind::procedure_step, kind::rule,
	                     kind::definition})
		if (name(k) == s) {
			out = k;
			return true;
		}
	return false;
}

bool to_relation(std::string_view s, relation &out)
{
	for (relation const r : {relation::same, relation::related,
	                         relation::contradicts, relation::novel})
		if (name(r) == s) {
			out = r;
			return true;
		}
	return false;
}

void field(std::string &to, std::string_view s)
{
	to += std::to_string(s.size());
	to += ':';
	to += s;
}

agenda::id record_id(record const &r, hash8_fn h)
{
	std::string key{"semantic-record-v2"};
	field(key, name(r.what));
	field(key, r.subject);
	field(key, r.relation);
	field(key, r.object);
	for (evidence_span const &e : r.evidence) {
		field(key, e.source);
		key += std::to_string(e.first);
		key += '-';
		key += std::to_string(e.last);
		key += ';';
	}
	return h(key);
}

// Contiguous ascending numbers make the lookup arithmetic; the
// number check keeps a malformed window from lying about it.
cue const *find_cue(window const &w, std::uint32_t n)
{
	if (w.cues.empty() || n < w.cues.front().number)
		return nullptr;
	std::size_t const at = n - w.cues.front().number;
	return at < w.cues.size() && w.cues[at].number == n
	     ? &w.cues[at] : nullptr;
}

// The assertion itself -- kind, subject, statement -- as both the
// model's reply and the catalog's own file carry it.
bool assertion(json const &v, record &out)
{
	std::string k;
	return text(v.member("kind"), k, 32) && to_kind(k, out.what)
	    && text(v.member("subject"), out.subject, kMaxSubject)
	    && text(v.member("relation"), out.relation, kMaxSubject)
	    && text(v.member("object"), out.object, kMaxObject)
	    && text(v.member("statement"), out.statement, kMaxStatement);
}

bool one_record(json const &v, window const &w, hash8_fn h,
	            record &out)
{
	if (!exact_object(v, {"kind", "subject", "relation", "object",
	                      "statement", "cues"})
	    || !assertion(v, out))
		return false;
	json const *const refs = v.member("cues");
	if (!refs || refs->kind != json::type::array || refs->array.empty()
	    || refs->array.size() > kMaxCitations || w.source.empty())
		return false;
	std::vector<std::uint32_t> cited;
	for (json const &j : refs->array) {
		std::uint32_t n;
		if (!integer(&j, n) || !find_cue(w, n))
			return false;
		cited.push_back(n);
	}
	std::ranges::sort(cited);
	if (std::ranges::adjacent_find(cited) != cited.end())
		return false;
	for (std::size_t i = 0; i < cited.size();) {
		std::size_t last = i;
		while (last + 1 < cited.size()
		       && cited[last + 1] == cited[last] + 1)
			++last;
		cue const *const a = find_cue(w, cited[i]);
		cue const *const b = find_cue(w, cited[last]);
		evidence_span e{std::string(w.source), std::string(w.title),
		                cited[i], cited[last], a->start, b->end, {}};
		for (std::size_t n = i; n <= last; ++n) {
			if (!e.quote.empty())
				e.quote += '\n';
			e.quote += find_cue(w, cited[n])->text;
		}
		out.evidence.push_back(std::move(e));
		i = last + 1;
	}
	out.id = record_id(out, h);
	return bool(out.id);
}

void json_string(std::string &out, std::string_view s)
{
	static constexpr char hex[] = "0123456789abcdef";
	out += '"';
	for (unsigned char const c : s) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\b': out += "\\b"; break;
		case '\f': out += "\\f"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20) {
				out += "\\u00";
				out += hex[c >> 4];
				out += hex[c & 15];
			} else {
				out += char(c);
			}
		}
	}
	out += '"';
}

void json_number(std::string &out, double n)
{
	char buf[64];
	auto const [p, ec] = std::to_chars(
		buf, buf + sizeof buf, n, std::chars_format::general, 17);
	if (ec == std::errc())
		out.append(buf, p);
}

std::string encode(record const &r)
{
	std::string out{"{\"id\":"};
	json_string(out, r.id.hex());
	out += ",\"kind\":";
	json_string(out, name(r.what));
	out += ",\"subject\":";
	json_string(out, r.subject);
	out += ",\"relation\":";
	json_string(out, r.relation);
	out += ",\"object\":";
	json_string(out, r.object);
	out += ",\"statement\":";
	json_string(out, r.statement);
	out += ",\"evidence\":[";
	for (std::size_t i = 0; i < r.evidence.size(); ++i) {
		if (i)
			out += ',';
		evidence_span const &e = r.evidence[i];
		out += "{\"source\":";
		json_string(out, e.source);
		out += ",\"title\":";
		json_string(out, e.title);
		out += ",\"first\":" + std::to_string(e.first)
		     + ",\"last\":" + std::to_string(e.last)
		     + ",\"start\":";
		json_number(out, e.start);
		out += ",\"end\":";
		json_number(out, e.end);
		out += ",\"quote\":";
		json_string(out, e.quote);
		out += '}';
	}
	out += "]}\n";
	return out;
}

std::string encode(edge const &e)
{
	std::string out{"{\"a\":"};
	json_string(out, e.a.hex());
	out += ",\"b\":";
	json_string(out, e.b.hex());
	out += ",\"relation\":";
	json_string(out, name(e.what));
	out += ",\"rationale\":";
	json_string(out, e.rationale);
	out += "}\n";
	return out;
}

bool decode_span(json const &v, evidence_span &e)
{
	if (!exact_object(v, {"source", "title", "first", "last",
	                     "start", "end", "quote"})
	    || !text(v.member("source"), e.source, 1024)
	    || !text(v.member("title"), e.title, 4096, true)
	    || !integer(v.member("first"), e.first)
	    || !integer(v.member("last"), e.last)
	    || e.last < e.first || !number(v.member("start"), e.start)
	    || !number(v.member("end"), e.end) || e.end < e.start
	    || !text(v.member("quote"), e.quote, kMaxFile))
		return false;
	return true;
}

bool decode_record(std::string_view bytes, record &r)
{
	json root;
	if (!json_parser(bytes).parse(root)
	    || !exact_object(root, {"id", "kind", "subject", "relation",
	                            "object", "statement", "evidence"}))
		return false;
	std::string id;
	if (!text(root.member("id"), id, 16)
	    || !(r.id = agenda::id::from_hex(id)) || !assertion(root, r))
		return false;
	json const *const ev = root.member("evidence");
	if (!ev || ev->kind != json::type::array || ev->array.empty()
	    || ev->array.size() > kMaxCitations)
		return false;
	for (json const &j : ev->array) {
		r.evidence.emplace_back();
		if (!decode_span(j, r.evidence.back()))
			return false;
	}
	return true;
}

bool decode_edge(std::string_view bytes, edge &e)
{
	json root;
	if (!json_parser(bytes).parse(root)
	    || !exact_object(root, {"a", "b", "relation", "rationale"}))
		return false;
	std::string a, b, rel;
	bool const ok = text(root.member("a"), a, 16)
	    && text(root.member("b"), b, 16)
	    && bool(e.a = agenda::id::from_hex(a))
	    && bool(e.b = agenda::id::from_hex(b)) && e.a != e.b
	    && text(root.member("relation"), rel, 32)
	    && to_relation(rel, e.what)
	    && text(root.member("rationale"), e.rationale,
	            kMaxStatement, true);
	if (ok && e.b < e.a)
		std::swap(e.a, e.b);
	return ok;
}

bool atomic_write(fs::path const &path, std::string_view bytes)
{
	fs::path tmp = path;
	tmp += ".tmp." + std::to_string(getpid());
	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
		f.write(bytes.data(), std::streamsize(bytes.size()));
		f.close();
		if (!f) {
			std::error_code ec;
			fs::remove(tmp, ec);
			return false;
		}
	}
	std::error_code ec;
	fs::rename(tmp, path, ec);
	if (!ec)
		return true;
	fs::remove(tmp, ec);
	return false;
}

// The one lexical ranking: weighted overlap descending, the
// smaller vocabulary first at a tie (a record mostly about the
// shared words beats one that merely mentions them), id last for a
// total order.  candidates() and search() differ only in the pool.
std::vector<std::size_t> rank(vocab const &v,
	                          std::vector<record> const &pool,
	                          std::span<std::vector<std::uint32_t> const> words,
	                          std::vector<std::uint32_t> const &needle,
	                          agenda::id skip, std::size_t limit)
{
	struct scored { std::size_t at, words; double score; };
	auto const before = [&pool](scored const &a, scored const &b) {
		if (a.score != b.score)
			return a.score > b.score;
		if (a.words != b.words)
			return a.words < b.words;
		return pool[a.at].id < pool[b.at].id;
	};
	std::vector<scored> best;
	for (std::size_t i = 0; i < pool.size(); ++i) {
		if (pool[i].id == skip)
			continue;
		if (double const score = v.overlap(needle, words[i]); score > 0)
			keep_best(best, {i, words[i].size(), score}, limit,
			          before);
	}
	std::vector<std::size_t> out;
	for (scored const &s : best)
		out.push_back(s.at);
	return out;
}

std::string record_text(record const &r)
{
	return r.subject + '\n' + r.statement;
}

// The code point at i and its byte length, or zero for a byte that
// is no well-formed sequence's start: a malformed byte is content,
// never decoded.
std::size_t decode(std::string_view s, std::size_t i, char32_t &cp)
{
	unsigned char const c = s[i];
	if (c < 0x80) {
		cp = c;
		return 1;
	}
	std::size_t const n = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 0;
	if (!n || i + n > s.size())
		return 0;
	cp = c & (0xff >> (n + 1));
	for (std::size_t k = 1; k < n; ++k) {
		unsigned char const t = s[i + k];
		if ((t & 0xc0) != 0x80)
			return 0;
		cp = cp << 6 | (t & 0x3f);
	}
	return n;
}

// Whether a code point lies in one of a table's closed ranges.
template <std::size_t N>
bool within(unicode_range const (&table)[N], char32_t cp)
{
	auto const at = std::ranges::upper_bound(table, cp, {},
	                                         &unicode_range::lo);
	return at != std::begin(table) && cp <= (at - 1)->hi;
}

// Simple case folding of one code point: ASCII inline, every other
// cased script by Unicode's own table.
char32_t fold(char32_t cp)
{
	if (cp < 0x80)
		return cp >= 'A' && cp <= 'Z' ? cp + ('a' - 'A') : cp;
	auto const at = std::ranges::upper_bound(kUnicodeFolds, cp, {},
	                                         &unicode_fold::lo);
	if (at == std::begin(kUnicodeFolds))
		return cp;
	unicode_fold const &run = *(at - 1);
	char32_t const off = cp - run.lo;
	return off <= run.span && off % run.stride == 0
		? char32_t(std::int32_t(cp) + run.delta) : cp;
}

// Text as identity compares it: case folded in every script, one
// space for any run of blanks, none at the ends.
std::string folded(std::string_view text)
{
	std::string out;
	bool blank = true;
	for (std::size_t i = 0; i < text.size();) {
		char32_t cp;
		std::size_t const n = decode(text, i, cp);
		if (!n) {
			out += text[i++];
			blank = false;
			continue;
		}
		i += n;
		if (cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n') {
			blank = true;
			continue;
		}
		if (blank && !out.empty())
			out += ' ';
		blank = false;
		utf8(out, fold(cp));
	}
	return out;
}

// Whether a code point bounds a word: ASCII that is not
// alphanumeric, and past ASCII Unicode's punctuation, separator and
// symbol ranges, so a script's own comma bounds a word in that
// script and no block is named by hand.
bool bounds(char32_t cp)
{
	if (cp < 0x80)
		return !((cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z')
		         || (cp >= 'a' && cp <= 'z'));
	return within(kUnicodeMarks, cp);
}

} // namespace

std::vector<std::uint32_t> vocab::words(std::string_view text)
{
	std::vector<std::uint32_t> out;
	std::string word;
	auto const keep = [this, &out, &word] {
		if (word.empty())
			return;
		auto const [it, fresh] = m_ids.try_emplace(
			word, std::uint32_t(m_df.size()));
		if (fresh)
			m_df.push_back(0);
		out.push_back(it->second);
		word.clear();
	};
	// A word is a run of content between boundaries -- except in a
	// script that writes no spaces, where every character is a
	// word and only its combining marks stay attached; a malformed
	// sequence is content.
	std::string const low = folded(text);
	bool after_unspaced = false;
	for (std::size_t i = 0; i < low.size();) {
		char32_t cp;
		std::size_t const n = decode(low, i, cp);
		if (!n) {
			word += low[i++];
			after_unspaced = false;
			continue;
		}
		if (bounds(cp)) {
			keep();
			i += n;
			after_unspaced = false;
			continue;
		}
		bool const unspaced = within(kUnicodeUnspaced, cp);
		if ((unspaced || after_unspaced)
		    && !(unspaced && within(kUnicodeCombining, cp)))
			keep();
		word.append(low, i, n);
		i += n;
		after_unspaced = unspaced;
	}
	keep();
	std::ranges::sort(out);
	out.erase(std::ranges::unique(out).begin(), out.end());
	return out;
}

void vocab::count(std::vector<std::uint32_t> const &doc)
{
	++m_docs;
	for (std::uint32_t const id : doc)
		if (id < m_df.size())
			++m_df[id];
}

double vocab::overlap(std::vector<std::uint32_t> const &a,
	                  std::vector<std::uint32_t> const &b) const
{
	double sum = 0.0;
	std::size_t i = 0, j = 0;
	while (i < a.size() && j < b.size()) {
		if (a[i] < b[j]) {
			++i;
		} else if (b[j] < a[i]) {
			++j;
		} else {
			std::uint32_t const df = a[i] < m_df.size() ? m_df[a[i]] : 0;
			sum += 1.0 + std::log((m_docs + 1.0) / (df + 1.0));
			++i;
			++j;
		}
	}
	return sum;
}

std::string_view name(kind k)
{
	switch (k) {
	case kind::claim:          return "claim";
	case kind::relationship:   return "relationship";
	case kind::procedure_step: return "procedure_step";
	case kind::rule:           return "rule";
	case kind::definition:     return "definition";
	}
	return {};
}

std::string_view name(relation r)
{
	switch (r) {
	case relation::same:        return "same";
	case relation::related:     return "related";
	case relation::contradicts: return "contradicts";
	case relation::novel:       return "novel";
	}
	return {};
}

parse_result parse_records(std::string_view bytes, window const &source,
	                         hash8_fn h)
{
	parse_result out;
	json root;
	if (!h || !json_parser(bytes).parse(root)) {
		out.error = "malformed JSON";
		return out;
	}
	if (!exact_object(root, {"records"})) {
		out.error = "expected one records member";
		return out;
	}
	json const *const rows = root.member("records");
	if (!rows || rows->kind != json::type::array
	    || rows->array.size() > kMaxRecords) {
		out.error = "records is not a bounded array";
		return out;
	}
	for (json const &j : rows->array) {
		record r;
		if (!one_record(j, source, h, r)) {
			++out.rejected;
			continue;
		}
		out.records.push_back(std::move(r));
	}
	return out;
}

bool parse_verdict(std::string_view bytes, verdict &out)
{
	json root;
	std::string rel;
	verdict v;
	if (!json_parser(bytes).parse(root)
	    || !exact_object(root, {"relation", "rationale"})
	    || !text(root.member("relation"), rel, 32)
	    || !to_relation(rel, v.what)
	    || !text(root.member("rationale"), v.rationale,
	             kMaxStatement, true))
		return false;
	out = std::move(v);
	return true;
}

bool parse_answer(std::string_view bytes, answer &out)
{
	json root;
	answer a;
	if (!json_parser(bytes).parse(root)
	    || !exact_object(root, {"answer", "citations", "insufficient"})
	    || !text(root.member("answer"), a.text, kMaxFile))
		return false;
	json const *const insufficient = root.member("insufficient");
	json const *const cites = root.member("citations");
	if (!insufficient || insufficient->kind != json::type::boolean
	    || !cites || cites->kind != json::type::array
	    || cites->array.size() > kMaxCitations)
		return false;
	a.insufficient = insufficient->boolean;
	for (json const &j : cites->array) {
		citation c;
		// The source is a discovery identity, sixteen hex, as
		// answer_schema() demands: the parser states the same
		// contract as the request.
		if (!exact_object(j, {"source", "first", "last"})
		    || !text(j.member("source"), c.source, 16)
		    || !agenda::id::from_hex(c.source)
		    || !integer(j.member("first"), c.first)
		    || !integer(j.member("last"), c.last)
		    || c.last < c.first)
			return false;
		// A span cited twice is cited once: the model repeating
		// itself is not more evidence, and the pane would draw
		// the repeat as another row.
		if (std::ranges::find(a.citations, c) == a.citations.end())
			a.citations.push_back(std::move(c));
	}
	// Insufficient and cited are each other's negation: an answer
	// claims evidence or says it has none, never both, never
	// neither.
	if (a.insufficient != a.citations.empty())
		return false;
	out = std::move(a);
	return true;
}

catalog::catalog(std::string dir, hash8_fn h, vocab &words,
	             std::vector<std::string> order)
	: m_dir(std::move(dir)), m_order(std::move(order)), m_hash(h),
	  m_vocab(words)
{
	fs::create_directories(m_dir + "/records", m_error);
	if (!m_error)
		fs::create_directories(m_dir + "/edges", m_error);
}

void catalog::clear()
{
	m_records.clear();
	m_words.clear();
	m_at.clear();
	m_edges.clear();
	m_edgeAt.clear();
	m_staged.clear();
	m_pending.clear();
	m_stale = true;
}

// The pair's identity, which also names its file.
agenda::id catalog::pair_id(edge const &e) const
{
	return m_hash("semantic-edge-v1" + e.a.hex() + e.b.hex());
}

record const *catalog::find(agenda::id id) const
{
	std::size_t const at = m_at.find(id);
	return at == agenda::index::npos ? nullptr : &m_records[at];
}

bool catalog::linked(agenda::id a, agenda::id b) const
{
	if (b < a)
		std::swap(a, b);
	return m_edgeAt.find(pair_id({a, b, {}, {}}))
	    != agenda::index::npos;
}

// The record and its vocabulary move in step: every ranking reads
// the words beside the record instead of tokenizing it again.
bool catalog::adopt(record r)
{
	m_stale = true;
	m_at.add(r.id, m_records.size());
	m_words.push_back(m_vocab.words(record_text(r)));
	m_records.push_back(std::move(r));
	return true;
}

bool catalog::stage(agenda::id a, agenda::id b)
{
	if (!a || !b || a == b)
		return false;
	if (b < a)
		std::swap(a, b);
	edge const e{a, b, {}, {}};
	agenda::id const p = pair_id(e);
	if (m_edgeAt.find(p) != agenda::index::npos
	    || m_pending.find(p) != agenda::index::npos)
		return true;
	// The line is on disk before the pair is in memory: the append
	// is buffered, and only the close says whether it landed.
	{
		std::ofstream f(m_dir + "/staged", std::ios::app);
		f << a.hex() << ' ' << b.hex() << '\n';
		f.close();
		if (!f)
			return false;
	}
	m_pending.add(p, 0);
	m_staged.push_back(e);
	return true;
}

void catalog::load()
{
	clear();
	std::vector<fs::path> files;
	std::error_code ec;
	for (fs::directory_iterator it(m_dir + "/records", ec), end;
	     !ec && it != end; it.increment(ec))
		if (it->path().extension() == ".record")
			files.push_back(it->path());
	std::ranges::sort(files);
	// An entry lives under its own name: a file elsewhere is a
	// stray, not a record or relation, or the slot's rightful
	// owner would one day rewrite it and the stray's content
	// would vanish from disk while having been believed.
	for (fs::path const &p : files) {
		std::string bytes;
		record r;
		if (slurp(p, bytes, kMaxFile) && decode_record(bytes, r)
		    && record_id(r, m_hash) == r.id
		    && p.stem() == r.id.hex() && !find(r.id))
			adopt(std::move(r));
	}
	files.clear();
	for (fs::directory_iterator it(m_dir + "/edges", ec), end;
	     !ec && it != end; it.increment(ec))
		if (it->path().extension() == ".edge")
			files.push_back(it->path());
	std::ranges::sort(files);
	for (fs::path const &p : files) {
		std::string bytes;
		edge e;
		if (slurp(p, bytes, kMaxFile) && decode_edge(bytes, e)
		    && p.stem() == pair_id(e).hex()
		    && !linked(e.a, e.b)) {
			m_edgeAt.add(pair_id(e), m_edges.size());
			m_edges.push_back(std::move(e));
		}
	}
	// The staged list is pending work only: a pair that linked,
	// a duplicate or a torn line is dropped, and a list that lost
	// anything is rewritten, so the file stays the size of what
	// is still owed rather than of everything ever asked.
	// Our own line file, not a model reply: it is read whole,
	// however many pairs a session appended, and compacted below
	// to the pending ones.
	std::string lines;
	if (!slurp(m_dir + "/staged", lines))
		return;
	std::string kept;
	for (std::string_view rest = lines; !rest.empty();) {
		std::size_t const nl = rest.find('\n');
		std::string_view const line = rest.substr(0, nl);
		rest = nl == std::string_view::npos
		       ? std::string_view() : rest.substr(nl + 1);
		if (line.size() != 33 || line[16] != ' ')
			continue;
		edge const e{agenda::id::from_hex(line.substr(0, 16)),
		             agenda::id::from_hex(line.substr(17)), {}, {}};
		if (e.a && e.b && e.a != e.b && !(e.b < e.a)
		    && !linked(e.a, e.b)
		    && m_pending.find(pair_id(e)) == agenda::index::npos) {
			m_pending.add(pair_id(e), 0);
			m_staged.push_back(e);
			kept += line;
			kept += '\n';
		}
	}
	if (kept != lines)
		atomic_write(m_dir + "/staged", kept);
}

bool catalog::put(record r)
{
	if (!r.id || record_id(r, m_hash) != r.id
	    || r.subject.empty() || r.relation.empty() || r.object.empty()
	    || r.statement.empty() || r.evidence.empty())
		return false;
	fs::path const path = fs::path(m_dir) / "records"
	                    / (r.id.hex() + ".record");
	// A known id is the record, already published: the id hashes
	// the triple and the cited spans, while a span's title, times
	// and quote are read off the source as it stands and may differ
	// between two presentations of one assertion -- a renamed
	// video, a retimed subtitle -- without making it another.
	// Insisting on field equality here refused the rightful record
	// its second visit and, with it, the candidate pairs that visit
	// stages.  The presentation, though, follows the source: the
	// record takes the current one, in memory and on disk, so a
	// bundle names the file the reader has, not the one the record
	// was written under.
	if (std::size_t const at = m_at.find(r.id);
	    at != agenda::index::npos) {
		record &known = m_records[at];
		if (known.evidence != r.evidence) {
			known.evidence = std::move(r.evidence);
			m_stale = true;
			atomic_write(path, encode(known));
		}
		return true;
	}
	// The id is the content hash: a file under it that is not the
	// canonical encoding of a record with that identity is no
	// record at all (a torn write, a stale encoder) and is
	// rewritten; one that is needs no second write.
	std::error_code ec;
	std::string bytes;
	record old;
	bool const stored = fs::exists(path, ec)
	                 && slurp(path, bytes, kMaxFile)
	                 && decode_record(bytes, old)
	                 && record_id(old, m_hash) == old.id
	                 && old.id == r.id;
	if (!stored && !atomic_write(path, encode(r)))
		return false;
	return adopt(std::move(r));
}

bool catalog::link(edge e)
{
	if (!e.a || !e.b || e.a == e.b)
		return false;
	if (e.b < e.a)
		std::swap(e.a, e.b);
	// A known pair is linked: the first verdict stands, as the
	// first publication of a record does.
	agenda::id const p = pair_id(e);
	if (m_edgeAt.find(p) != agenda::index::npos)
		return true;
	fs::path const path = fs::path(m_dir) / "edges"
	                    / (p.hex() + ".edge");
	std::error_code ec;
	std::string bytes;
	edge old;
	bool const stored = fs::exists(path, ec)
	                 && slurp(path, bytes, kMaxFile)
	                 && decode_edge(bytes, old)
	                 && old.a == e.a && old.b == e.b;
	if (!stored && !atomic_write(path, encode(e)))
		return false;
	std::erase_if(m_staged, [&e](edge const &s) {
		return s.a == e.a && s.b == e.b;
	});
	m_edgeAt.add(p, m_edges.size());
	m_edges.push_back(std::move(e));
	m_stale = true;
	return true;
}

std::vector<record> const &catalog::consolidated()
{
	if (!m_stale)
		return m_view;
	m_stale = false;
	std::vector<std::size_t> parent(m_records.size());
	for (std::size_t i = 0; i < parent.size(); ++i)
		parent[i] = i;
	auto root = [&parent](std::size_t at) {
		while (parent[at] != at) {
			parent[at] = parent[parent[at]];
			at = parent[at];
		}
		return at;
	};
	auto unite = [&parent, &root](std::size_t i, std::size_t j) {
		std::size_t const a = root(i), b = root(j);
		if (a != b)
			parent[std::max(a, b)] = std::min(a, b);
	};
	// The same assertion to the byte is the same claim wherever it
	// was said: overlapping windows read one sentence twice.
	agenda::index twins;
	for (std::size_t i = 0; i < m_records.size(); ++i) {
		record const &r = m_records[i];
		agenda::id const k = m_hash(folded(
			std::string(name(r.what)) + '\n' + r.subject + '\n'
			+ r.relation + '\n' + r.object));
		if (std::size_t const at = twins.find(k);
		    at != agenda::index::npos)
			unite(i, at);
		else
			twins.add(k, i);
	}
	for (edge const &e : m_edges) {
		if (e.what != relation::same)
			continue;
		std::size_t const ai = m_at.find(e.a), bi = m_at.find(e.b);
		if (ai != agenda::index::npos && bi != agenda::index::npos)
			unite(ai, bi);
	}

	std::vector<record> out;
	std::vector<std::vector<std::size_t>> groups(m_records.size());
	for (std::size_t i = 0; i < m_records.size(); ++i)
		groups[root(i)].push_back(i);
	// A root's concept position, for the tie count below.
	std::vector<std::size_t> concept_at(m_records.size());
	m_conceptOf.assign(m_records.size(), 0);
	m_viewMembers.clear();
	for (auto const &members : groups) {
		if (members.empty())
			continue;
		concept_at[members.front()] = out.size();
		for (std::size_t const i : members)
			m_conceptOf[i] = out.size();
		std::size_t best = members.front();
		for (std::size_t const i : members)
			if (m_records[i].evidence.size()
			      > m_records[best].evidence.size()
			    || (m_records[i].evidence.size()
			          == m_records[best].evidence.size()
			        && (m_records[i].statement.size()
			              > m_records[best].statement.size()
			            || (m_records[i].statement.size()
			                  == m_records[best].statement.size()
			                && m_records[i].id < m_records[best].id))))
				best = i;
		record merged = m_records[best];
		std::vector<agenda::id> ids;
		ids.reserve(members.size());
		for (std::size_t const i : members) {
			ids.push_back(m_records[i].id);
			for (evidence_span const &e : m_records[i].evidence)
				if (std::ranges::find(merged.evidence, e)
				    == merged.evidence.end())
					merged.evidence.push_back(e);
		}
		if (members.size() > 1) {
			std::ranges::sort(ids);
			std::string key{"semantic-concept-v1"};
			for (agenda::id const id : ids)
				key += id.hex();
			merged.id = m_hash(key);
		}
		out.push_back(std::move(merged));
		m_viewMembers.push_back(std::move(ids));
	}
	m_view = std::move(out);
	m_viewWords.clear();
	m_viewWords.reserve(m_view.size());
	for (record const &r : m_view)
		m_viewWords.push_back(m_vocab.words(record_text(r)));
	// Names first: every distinct folded subject is a name, and a
	// same verdict between two names unites them into one entity,
	// titled as the first was.  Then each concept goes under its
	// entity's predicate, and an object that is a name links to
	// that name's entity.
	// Concepts in the order the corpus first states them -- the
	// sources as the corpus lists them, the cue within a source --
	// so an entity's predicates and objects read as the speakers
	// introduced them, not as the catalog happened to load them.
	// A merged concept lists its representative's spans first,
	// since they witness its statement, so its earliest span is
	// found across all of them rather than read off the front; and
	// two concepts first stated in one cue fall back to their ids,
	// since an unstable sort would otherwise order them by luck.
	struct mention {
		std::size_t          source;   // its place in the corpus
		evidence_span const *span;
	};
	auto const earlier = [](mention const &a, mention const &b) {
		if (a.source != b.source)
			return a.source < b.source;
		if (a.span->source != b.span->source)
			return a.span->source < b.span->source;
		return a.span->first < b.span->first;
	};
	std::vector<mention> lead;
	lead.reserve(m_view.size());
	for (record const &r : m_view) {
		mention first{place(r.evidence.front().source),
		              &r.evidence.front()};
		for (evidence_span const &e : r.evidence)
			if (mention const m{place(e.source), &e};
			    earlier(m, first))
				first = m;
		lead.push_back(first);
	}
	std::vector<std::size_t> spoken(m_view.size());
	for (std::size_t i = 0; i < spoken.size(); ++i)
		spoken[i] = i;
	std::ranges::sort(spoken, [this, &lead, &earlier](std::size_t a,
	                                                  std::size_t b) {
		if (earlier(lead[a], lead[b]))
			return true;
		if (earlier(lead[b], lead[a]))
			return false;
		return m_view[a].id < m_view[b].id;
	});
	std::vector<std::string> names;
	agenda::index named;
	for (std::size_t const i : spoken) {
		record const &r = m_view[i];
		if (named.find(entity_id(r.subject)) == agenda::index::npos) {
			named.add(entity_id(r.subject), names.size());
			names.push_back(r.subject);
		}
	}
	std::vector<std::size_t> name_root(names.size());
	for (std::size_t i = 0; i < names.size(); ++i)
		name_root[i] = i;
	auto nroot = [&name_root](std::size_t at) {
		while (name_root[at] != at)
			at = name_root[at] = name_root[name_root[at]];
		return at;
	};
	for (edge const &e : m_edges) {
		std::size_t const a = named.find(e.a), b = named.find(e.b);
		if (e.what != relation::same || a == agenda::index::npos
		    || b == agenda::index::npos)
			continue;
		std::size_t const ra = nroot(a), rb = nroot(b);
		if (ra != rb)
			name_root[std::max(ra, rb)] = std::min(ra, rb);
	}
	m_entities.clear();
	m_entityAt.clear();
	std::vector<std::size_t> entity_of_name(names.size());
	for (std::size_t i = 0; i < names.size(); ++i) {
		std::size_t const r = nroot(i);
		if (r == i) {
			entity_of_name[i] = m_entities.size();
			m_entities.push_back({entity_id(names[i]), names[i],
			                      npos, {}});
		} else {
			entity_of_name[i] = entity_of_name[r];
		}
		m_entityAt.add(entity_id(names[i]), entity_of_name[i]);
	}
	for (std::size_t const i : spoken) {
		record const &r = m_view[i];
		entity &e = m_entities[m_entityAt.find(entity_id(r.subject))];
		if (r.what == kind::definition && e.definition == npos)
			e.definition = i;
		std::string const key = folded(r.relation);
		auto p = std::ranges::find_if(e.predicates,
			[&key](predicate const &q) {
				return folded(q.title) == key;
			});
		if (p == e.predicates.end()) {
			e.predicates.push_back({r.relation, {}});
			p = e.predicates.end() - 1;
		}
		p->objects.push_back({i, m_entityAt.find(entity_id(r.object))});
	}
	// Every member's edges beyond the equivalences count for the
	// concept; an edge inside one concept is not a tie to another.
	m_viewTies.assign(m_view.size(), {});
	for (edge const &e : m_edges) {
		std::size_t const ai = m_at.find(e.a), bi = m_at.find(e.b);
		if (e.what == relation::same || ai == agenda::index::npos
		    || bi == agenda::index::npos)
			continue;
		std::size_t const a = root(ai), b = root(bi);
		if (a == b)
			continue;
		for (std::size_t const at : {a, b}) {
			tie_count &t = m_viewTies[concept_at[at]];
			if (e.what == relation::related)
				++t.related;
			else if (e.what == relation::contradicts)
				++t.contradicts;
		}
	}
	return m_view;
}

std::vector<catalog::entity> const &catalog::entities()
{
	consolidated();
	return m_entities;
}

// A source's place in the corpus; one past the last for a source
// the corpus does not list, which then sorts by name among its kind.
std::size_t catalog::place(std::string_view source) const
{
	return std::size_t(std::ranges::find(m_order, source)
	                   - m_order.begin());
}

agenda::id catalog::entity_id(std::string_view title) const
{
	return m_hash("semantic-entity-v1\n" + folded(title));
}

std::size_t catalog::entity_of(agenda::id id)
{
	consolidated();
	return m_entityAt.find(id);
}

bool catalog::equivalent(agenda::id a, agenda::id b)
{
	std::size_t const i = m_at.find(a), j = m_at.find(b);
	if (i == agenda::index::npos || j == agenda::index::npos)
		return false;
	consolidated();
	return m_conceptOf[i] == m_conceptOf[j];
}

catalog::tie_count catalog::ties(std::size_t at)
{
	return at < consolidated().size() ? m_viewTies[at] : tie_count{};
}

std::vector<agenda::id> catalog::partners(std::size_t at, relation what)
{
	if (at >= consolidated().size())
		return {};
	std::vector<agenda::id> const &members = m_viewMembers[at];
	std::vector<agenda::id> out;
	for (edge const &e : m_edges) {
		if (e.what != what)
			continue;
		bool const a = std::ranges::find(members, e.a) != members.end();
		bool const b = std::ranges::find(members, e.b) != members.end();
		if (a == b)
			continue;
		agenda::id const other = a ? e.b : e.a;
		if (std::ranges::find(out, other) == out.end())
			out.push_back(other);
	}
	return out;
}

std::vector<std::size_t> catalog::candidates(record const &r,
	                                          std::size_t limit) const
{
	return rank(m_vocab, m_records, m_words,
	            m_vocab.words(record_text(r)), r.id, limit);
}

std::vector<std::size_t> catalog::search(std::string_view query,
	                                      std::size_t limit)
{
	auto const needle = m_vocab.words(query);
	if (needle.empty())
		return {};
	// The view first, in its own statement: the words beside it
	// are rebuilt with it.
	std::vector<record> const &view = consolidated();
	return rank(m_vocab, view, m_viewWords, needle, {}, limit);
}

bool well_formed(std::string_view bytes)
{
	json root;
	return bytes.size() <= kMaxFile && json_parser(bytes).parse(root);
}

std::string_view records_schema()
{
	return R"({"type":"object","additionalProperties":false,"required":["records"],"properties":{"records":{"type":"array","maxItems":64,"items":{"type":"object","additionalProperties":false,"required":["kind","subject","relation","object","statement","cues"],"properties":{"kind":{"type":"string","enum":["claim","relationship","procedure_step","rule","definition"]},"subject":{"type":"string","minLength":1,"maxLength":256},"relation":{"type":"string","minLength":1,"maxLength":256},"object":{"type":"string","minLength":1,"maxLength":1024},"statement":{"type":"string","minLength":1,"maxLength":4096},"cues":{"type":"array","minItems":1,"maxItems":64,"uniqueItems":true,"items":{"type":"integer","minimum":0}}}}}}})";
}

std::string_view verdict_schema()
{
	return R"({"type":"object","additionalProperties":false,"required":["relation","rationale"],"properties":{"relation":{"type":"string","enum":["same","related","contradicts","novel"]},"rationale":{"type":"string","maxLength":4096}}})";
}

std::string_view answer_schema()
{
	return R"({"type":"object","additionalProperties":false,"required":["answer","citations","insufficient"],"properties":{"answer":{"type":"string","minLength":1},"citations":{"type":"array","maxItems":64,"items":{"type":"object","additionalProperties":false,"required":["source","first","last"],"properties":{"source":{"type":"string","pattern":"^[0-9a-f]{16}$"},"first":{"type":"integer","minimum":0},"last":{"type":"integer","minimum":0}}}},"insufficient":{"type":"boolean"}}})";
}

} // namespace semantic
