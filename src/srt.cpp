#include "srt.hpp"

#include <optional>

namespace srt {

// ---------------------------------------------------------- encoding --

namespace {

// UTF-8 validator: byte-class table + state transition table; the
// scan loop is two table loads per byte, no branches.  Strictness:
// rejects C0/C1 and F5..FF leads, overlongs (E0 A0.., F0 90..
// minima), surrogates (ED 9F.. maximum) and anything above U+10FFFF
// (F4 8F.. maximum).
enum : std::uint8_t {
	asc, c_80_8f, c_90_9f, c_a0_bf, l2, e0, l3, ed, f0, l4, f4, bad,
	nclass
};

constexpr auto u8cls = [] {
	std::array<std::uint8_t, 256> t{};
	for (int c = 0x00; c <= 0x7F; ++c) t[c] = asc;
	for (int c = 0x80; c <= 0x8F; ++c) t[c] = c_80_8f;
	for (int c = 0x90; c <= 0x9F; ++c) t[c] = c_90_9f;
	for (int c = 0xA0; c <= 0xBF; ++c) t[c] = c_a0_bf;
	for (int c = 0xC0; c <= 0xC1; ++c) t[c] = bad;
	for (int c = 0xC2; c <= 0xDF; ++c) t[c] = l2;
	t[0xE0] = e0;
	for (int c = 0xE1; c <= 0xEC; ++c) t[c] = l3;
	t[0xED] = ed;
	t[0xEE] = l3;
	t[0xEF] = l3;
	t[0xF0] = f0;
	for (int c = 0xF1; c <= 0xF3; ++c) t[c] = l4;
	t[0xF4] = f4;
	for (int c = 0xF5; c <= 0xFF; ++c) t[c] = bad;
	return t;
}();

enum : std::uint8_t { ok, one, two, e0_1, ed_1, f0_1, f4_1, three, rej,
                      nstate };

constexpr auto u8next = [] {
	std::array<std::array<std::uint8_t, nclass>, nstate> t{};
	for (auto &row : t)
		row.fill(rej);
	t[ok][asc] = ok;
	t[ok][l2] = one;
	t[ok][e0] = e0_1;
	t[ok][l3] = two;
	t[ok][ed] = ed_1;
	t[ok][f0] = f0_1;
	t[ok][l4] = three;
	t[ok][f4] = f4_1;
	for (std::uint8_t c : {c_80_8f, c_90_9f, c_a0_bf}) {
		t[one][c] = ok;
		t[two][c] = one;
		t[three][c] = two;
	}
	t[e0_1][c_a0_bf] = one;                // E0 needs A0..BF
	t[ed_1][c_80_8f] = one;                // ED allows 80..9F
	t[ed_1][c_90_9f] = one;
	t[f0_1][c_90_9f] = two;                // F0 needs 90..BF
	t[f0_1][c_a0_bf] = two;
	t[f4_1][c_80_8f] = two;                // F4 allows 80..8F
	return t;
}();

// Flat UTF-8 encoder shared by the transcoders.
void put_utf8(std::string &out, char32_t cp)
{
	if (cp < 0x80) {
		out += char(cp);
		return;
	}
	if (cp < 0x800) {
		out += char(0xC0 | (cp >> 6));
		out += char(0x80 | (cp & 0x3F));
		return;
	}
	if (cp < 0x10000) {
		out += char(0xE0 | (cp >> 12));
		out += char(0x80 | ((cp >> 6) & 0x3F));
		out += char(0x80 | (cp & 0x3F));
		return;
	}
	out += char(0xF0 | (cp >> 18));
	out += char(0x80 | ((cp >> 12) & 0x3F));
	out += char(0x80 | ((cp >> 6) & 0x3F));
	out += char(0x80 | (cp & 0x3F));
}

// Windows-1252 -> Unicode: full 256-entry table (identity except the
// C1 block), so the transcoding loop selects nothing.
constexpr auto kCp1252 = [] {
	std::array<char32_t, 256> t{};
	for (int c = 0; c < 256; ++c)
		t[c] = char32_t(c);
	constexpr char32_t c1[32]{
		0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
		0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
		0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
		0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
	};
	for (int c = 0; c < 32; ++c)
		t[0x80 + c] = c1[c];
	return t;
}();

std::string cp1252_to_utf8(std::string_view raw)
{
	std::string out;
	out.reserve(raw.size() + raw.size() / 4);
	for (char const c : raw)
		put_utf8(out, kCp1252[static_cast<unsigned char>(c)]);
	return out;
}

// UTF-16 code units assembled bytewise (the payload sits at an odd
// offset, so a char16_t* cast would be misaligned); hi/lo are the
// byte offsets of the high and low octet within a unit.
struct u16_cursor {
	std::string_view b;
	std::size_t      i;
	int              hi, lo;

	bool next(char32_t &u)
	{
		if (i + 1 >= b.size())
			return false;
		u = char32_t(byte(i + hi)) << 8 | byte(i + lo);
		i += 2;
		return true;
	}

	// Consume the next unit iff it is a low surrogate.
	bool take_low(char32_t &v)
	{
		u16_cursor const save = *this;
		if (next(v) && v >= 0xDC00 && v < 0xE000)
			return true;
		*this = save;
		return false;
	}

private:
	unsigned byte(std::size_t k) const
	{
		return static_cast<unsigned char>(b[k]);
	}
};

std::string utf16_to_utf8(std::string_view payload, int hi, int lo)
{
	std::string out;
	out.reserve(payload.size() * 3 / 2);
	u16_cursor cu{payload, 0, hi, lo};
	for (char32_t u; cu.next(u);) {
		char32_t v;
		if (u >= 0xD800 && u < 0xDC00 && cu.take_low(v))
			u = 0x10000 + ((u - 0xD800) << 10) + (v - 0xDC00);
		if (u >= 0xD800 && u < 0xE000)
			u = 0xFFFD;                  // lone surrogate
		put_utf8(out, u);
	}
	return out;
}

} // namespace

bool valid_utf8(std::string_view v)
{
	std::uint8_t st = ok;
	for (char const c : v)
		st = u8next[st][u8cls[static_cast<unsigned char>(c)]];
	return st == ok;
}

std::string to_utf8(std::string_view raw)
{
	if (raw.starts_with("\xFF\xFE"))
		return utf16_to_utf8(raw.substr(2), 1, 0);
	if (raw.starts_with("\xFE\xFF"))
		return utf16_to_utf8(raw.substr(2), 0, 1);
	if (raw.starts_with("\xEF\xBB\xBF"))
		raw.remove_prefix(3);
	if (!valid_utf8(raw))
		return cp1252_to_utf8(raw);
	return std::string(raw);
}

// ------------------------------------------------------------ parser --

namespace {

struct collector : parser<collector> {
	std::vector<cue> cues;

	void on_cue(double a, double b, std::string &&t)
	{
		cues.push_back({a, b, std::move(t)});
	}
};

} // namespace

std::vector<cue> parse(std::string_view utf8)
{
	collector c;
	c.parse(utf8);
	return std::move(c.cues);
}

// ------------------------------------------------------------ markup --

namespace {

// Escape table: nullptr means the byte passes through unchanged, so
// UTF-8 multibyte sequences flow untouched.
constexpr auto esc_tab = [] {
	std::array<char const *, 256> t{};
	t[static_cast<unsigned char>('&')] = "&amp;";
	t[static_cast<unsigned char>('<')] = "&lt;";
	t[static_cast<unsigned char>('>')] = "&gt;";
	t[static_cast<unsigned char>('"')] = "&quot;";
	t[static_cast<unsigned char>('\n')] = "<br>";
	return t;
}();

bool ieq(std::string_view a, std::string_view b)
{
	auto const lower = [](char c) {
		return c >= 'A' && c <= 'Z' ? char(c + 32) : c;
	};
	return a.size() == b.size()
	    && std::ranges::equal(a, b, {}, lower, lower);
}

constexpr std::string_view simple_tags[]{
	"i", "b", "u", "/i", "/b", "/u", "/font"};

// Attribute value classes, one table, per-attribute masks.
enum : std::uint8_t {
	a_color = 1 << 0,                      // # and alphanumerics
	a_face  = 1 << 1,                      // family names, incl. UTF-8
	a_size  = 1 << 2,                      // digits
};

constexpr auto attr_cls = [] {
	std::array<std::uint8_t, 256> t{};
	for (int c = '0'; c <= '9'; ++c) t[c] = a_color | a_face | a_size;
	for (int c = 'a'; c <= 'z'; ++c) t[c] = a_color | a_face;
	for (int c = 'A'; c <= 'Z'; ++c) t[c] = a_color | a_face;
	t[static_cast<unsigned char>('#')] |= a_color;
	t[static_cast<unsigned char>(' ')] |= a_face;
	t[static_cast<unsigned char>('-')] |= a_face;
	t[static_cast<unsigned char>('_')] |= a_face;
	for (int c = 0x80; c <= 0xFF; ++c) t[c] |= a_face;
	return t;
}();

bool all_in(std::string_view v, std::uint8_t mask)
{
	return !v.empty() && std::ranges::all_of(v, [mask](char c) {
		return (attr_cls[static_cast<unsigned char>(c)] & mask) != 0;
	});
}

// Attribute scanner with an explicit receiver, like detail::scanner.
struct attr_cursor {
	std::string_view s;

	bool done() const { return s.empty(); }

	void spaces()
	{
		s.remove_prefix(std::min(s.find_first_not_of(" \t"), s.size()));
	}

	std::string_view ident()
	{
		std::size_t n = 0;
		while (n < s.size() && (attr_cls[static_cast<unsigned char>(s[n])]
		                        & (a_color | a_face)) && !detail::in(s[n], detail::b_digit))
			++n;
		std::string_view const v = s.substr(0, n);
		s.remove_prefix(n);
		return v;
	}

	bool ch(char c)
	{
		if (!s.starts_with(c))
			return false;
		s.remove_prefix(1);
		return true;
	}

	std::optional<std::string_view> value()
	{
		if (s.empty())
			return std::nullopt;
		if (s.front() == '"' || s.front() == '\'')
			return quoted();
		std::size_t const n = std::min(s.find_first_of(" \t"), s.size());
		std::string_view const v = s.substr(0, n);
		s.remove_prefix(n);
		return v.empty() ? std::nullopt : std::optional(v);
	}

private:
	std::optional<std::string_view> quoted()
	{
		char const q = s.front();
		s.remove_prefix(1);
		std::size_t const n = s.find(q);
		if (n == std::string_view::npos)
			return std::nullopt;
		std::string_view const v = s.substr(0, n);
		s.remove_prefix(n + 1);
		return v;
	}
};

// Emit one sanitized attribute, table-dispatched by name.
bool append_attr(std::string &out, std::string_view key,
                 std::string_view val)
{
	static constexpr struct {
		std::string_view name;
		std::uint8_t     mask;
	} kAttrs[]{{"color", a_color}, {"face", a_face}, {"size", a_size}};

	for (auto const &a : kAttrs) {
		if (!ieq(key, a.name))
			continue;
		if (!all_in(val, a.mask))
			return false;
		out += ' ';
		out += a.name;
		out += "=\"";
		out += val;
		out += '"';
		return true;
	}
	return false;
}

// Sanitized " key=\"value\"..." for a <font ...> tag, empty on
// anything off-whitelist or malformed (caller then escapes the tag).
std::string font_attrs(std::string_view s)
{
	std::string out;
	attr_cursor cu{s};
	while (true) {
		cu.spaces();
		if (cu.done())
			return out;
		std::string_view const key = cu.ident();
		cu.spaces();
		if (key.empty() || !cu.ch('='))
			return {};
		cu.spaces();
		std::optional<std::string_view> const val = cu.value();
		if (!val || !append_attr(out, key, *val))
			return {};
	}
}

// "{\...}" ASS override block: consumed length, or 0.
std::size_t skip_ass(std::string_view s)
{
	if (!s.starts_with("{\\"))
		return 0;
	std::size_t const close = s.find('}');
	return close == std::string_view::npos ? 0 : close + 1;
}

// "<...>" whitelisted tag: emits and returns consumed length, or 0.
std::size_t emit_tag(std::string_view s, std::string &out)
{
	std::size_t const close = s.find('>', 1);
	if (close == std::string_view::npos || close > 96)
		return 0;
	std::string_view const inner = detail::trim(s.substr(1, close - 1));
	auto const same = [inner](std::string_view t) { return ieq(inner, t); };
	if (std::ranges::any_of(simple_tags, same)) {
		out += '<';
		out += inner;
		out += '>';
		return close + 1;
	}
	if (inner.size() < 5 || !ieq(inner.substr(0, 4), "font")
	    || !detail::in(inner[4], detail::b_space))
		return 0;
	std::string const attrs = font_attrs(inner.substr(5));
	if (attrs.empty())
		return 0;
	out += "<font";
	out += attrs;
	out += '>';
	return close + 1;
}

// One piece of output; returns bytes consumed (always >= 1).
std::size_t piece(std::string_view s, std::string &out)
{
	unsigned char const c = static_cast<unsigned char>(s.front());
	if (c == '{') {
		std::size_t const n = skip_ass(s);
		if (n)
			return n;
	}
	if (c == '<') {
		std::size_t const n = emit_tag(s, out);
		if (n)
			return n;
	}
	if (esc_tab[c]) {
		out += esc_tab[c];
		return 1;
	}
	out += char(c);
	return 1;
}

} // namespace

std::string cue_html(std::string_view text)
{
	std::string out;
	out.reserve(text.size() + text.size() / 8 + 8);
	for (std::size_t i = 0; i < text.size();)
		i += piece(text.substr(i), out);
	return out;
}

} // namespace srt
