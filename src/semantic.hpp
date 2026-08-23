// semantic.hpp -- evidence-backed semantic records and catalog.
// Standard C++23, no Qt and no LLM client.  Model output enters only
// through parse_records()/parse_verdict(); both validate a closed
// schema before data can become durable.  A record is an atomic claim
// about the corpus, never a regex: evidence names exact source cues,
// and its quote is copied from those cues rather than trusted from the
// model.  Regexes and embeddings are retrieval mechanisms outside
// this ontology.
#ifndef SRTVIEW_SRC_SEMANTIC_HPP_
#define SRTVIEW_SRC_SEMANTIC_HPP_

#include "agenda.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace semantic {

using hash8_fn = agenda::id (*)(std::string_view);

// The largest document any of this reads or accepts: artifact,
// record, reply.
inline constexpr std::size_t kMaxFile = std::size_t{1024} * 1024;

enum class kind : std::uint8_t {
	claim,
	relationship,
	procedure_step,
	rule,
	definition,
};

std::string_view name(kind k);

// One source cue as presented to extraction.  number is the stable,
// zero-based cue index in that source; timestamps and text come from
// the parsed transcript, never model output.
struct cue {
	std::uint32_t number = 0;
	double        start  = 0.0;
	double        end    = 0.0;
	std::string   text;
};

// One contiguous run of a source's cues, numbers ascending, as
// presented to extraction.  A view: the cues and names belong to
// the source, which outlives every window cut from it.
struct window {
	std::string_view     source; // stable discovery identity
	std::string_view     title;  // human-readable video/source path
	std::span<cue const> cues;
};

// Consecutive cited cues collapse into one span.  quote is exact
// transcript text copied during validation; a retriever therefore
// never needs to ask the model what its citation meant.
struct evidence_span {
	std::string   source;
	std::string   title;
	std::uint32_t first = 0;
	std::uint32_t last  = 0;
	double        start = 0.0;
	double        end   = 0.0;
	std::string   quote;

	bool operator==(evidence_span const &) const = default;
};

// One assertion as a triple with its sentence: the subject is the
// thing it is about, named bare as the material names it; the
// relation the verb phrase; the object the complement, itself a
// bare name when it names a thing.  The triple is the identity
// (with the cited spans); the statement is the readable form.
struct record {
	agenda::id                 id;
	semantic::kind             what = kind::claim;
	std::string                subject;
	std::string                relation;
	std::string                object;
	std::string                statement;
	std::vector<evidence_span> evidence;

	bool operator==(record const &) const = default;
};

struct parse_result {
	std::vector<record> records;
	std::size_t         rejected = 0; // schema/citation-invalid items
	std::string         error;        // whole document was not JSON/schema

	explicit operator bool() const { return error.empty(); }
};

// Expected document:
// {"records":[{"kind":"claim|relationship|procedure_step|rule|definition",
//              "subject":"...","relation":"...","object":"...",
//              "statement":"...","cues":[1,2]}]}
// Extra object members are rejected.  Invalid records do not poison
// valid siblings; a malformed top-level document rejects the batch.
parse_result parse_records(std::string_view json, window const &source,
	                         hash8_fn h);

enum class relation : std::uint8_t {
	same,
	related,
	contradicts,
	novel,
};

std::string_view name(relation r);

struct verdict {
	relation    what = relation::novel;
	std::string rationale;
};

// Expected document:
// {"relation":"same|related|contradicts|novel","rationale":"..."}
bool parse_verdict(std::string_view json, verdict &out);

struct edge {
	agenda::id a, b;
	relation   what = relation::related;
	std::string rationale;

	bool operator==(edge const &) const = default;
};

struct citation {
	std::string source;
	std::uint32_t first = 0;
	std::uint32_t last  = 0;

	bool operator==(citation const &) const = default;
};

struct answer {
	std::string text;
	std::vector<citation> citations;
	bool insufficient = false;
};

// Expected document:
// {"answer":"...","citations":[{"source":"...","first":1,
//   "last":2}],"insufficient":false}
bool parse_answer(std::string_view json, answer &out);

// The retrieval vocabulary of one corpus.  Words are alphanumeric
// runs, case-folded (ASCII and Latin-1), interned to ids; a
// document is its sorted distinct ids.  count() tallies the corpus
// documents a word occurs in, and overlap() weighs every shared
// word by 1 + log((N + 1) / (df + 1)): a function word in whatever
// language the corpus speaks weighs next to nothing, a rare term
// nearly everything, and no word list is ever written down.  Every
// lexical ranking -- record against record, question against
// record or cue -- scores on this one vocabulary.
class vocab {
public:
	std::vector<std::uint32_t> words(std::string_view text);
	void count(std::vector<std::uint32_t> const &doc);
	double overlap(std::vector<std::uint32_t> const &a,
	               std::vector<std::uint32_t> const &b) const;

private:
	std::unordered_map<std::string, std::uint32_t> m_ids;
	std::vector<std::uint32_t> m_df;
	std::uint32_t m_docs = 0;
};

// Whether bytes form one complete JSON document: the gate a
// schema-constrained reply passes before it may be cached.  A
// generation cut off at the token cap is garbage under every
// recipe, and caching it would make the cut permanent.
bool well_formed(std::string_view json);

// Append-only on-disk catalog.  Each record and edge is one canonical
// JSON file published by rename.  load() tolerates corrupt strangers
// by skipping them; put()/link() dedupe by semantic identity -- a
// record's id, which hashes its assertion and cited spans but not
// the title, times or quote beside a span -- and are idempotent.
class catalog {
public:
	// The hash names every record, pair and concept and is never
	// null; the vocabulary is the corpus's, shared with whoever
	// ranks cues against records, and must outlive the catalog.
	// order lists the corpus's sources as it does: the view states
	// concepts source by source in that order and cue by cue
	// within one, sources the corpus does not list after them.
	catalog(std::string dir, hash8_fn h, vocab &words,
	        std::vector<std::string> order = {});

	// The first failure laying out the directories: a catalog that
	// cannot publish must not be fed, or every put() and link()
	// fails and is retried for as long as the session runs.
	std::error_code const &error() const { return m_error; }

	void load();
	bool put(record r);
	bool link(edge e);
	void clear();

	// A judgment asked is remembered until it links: the pair
	// list outlives the session that asked, so a verdict finishing
	// after that session's end still has someone to harvest it.
	// Idempotent; order is the order of asking; linking retires
	// the pair, and load() compacts the file to what is pending.
	bool stage(agenda::id a, agenda::id b);

	std::vector<record> const &records() const { return m_records; }
	std::vector<edge> const &edges() const { return m_edges; }
	std::vector<edge> const &staged() const { return m_staged; }
	record const *find(agenda::id id) const;
	bool linked(agenda::id a, agenda::id b) const;
	// Equivalence materializes a deterministic concept view: one
	// representative assertion with the union of every member's
	// evidence, concepts in the order their first member holds in
	// records(), evidence in the order the members contribute it.
	// Two records are equivalent by a same verdict, or by being the
	// same assertion to the byte (folded for case and spacing) --
	// one claim observed in two places is identity, not a judgment.
	// Atomic records remain untouched underneath it.  Derived once
	// per change and kept: the pane reads it on every refresh, the
	// retriever on every question.
	std::vector<record> const &consolidated();

	// Cheap mechanical routing, as positions.  candidates() ranks
	// records() by lexical overlap with a new record; search() ranks
	// consolidated() by overlap with a question.  No result is a
	// truth judgment.
	std::vector<std::size_t> candidates(record const &r,
	                                    std::size_t limit) const;
	std::vector<std::size_t> search(std::string_view query,
	                                std::size_t limit);

	// The concept view as a graph of entities.  An entity is a
	// subject as the corpus names it -- folded for case and spacing,
	// titled as first seen; its predicates are the distinct relation
	// phrases asserted of it, in first-seen order, each holding the
	// concepts that say something through it; and an object that
	// names an entity links to it, which is how the tree a pane
	// draws turns out to be a graph: expanding an object that is an
	// entity shows that entity's own predicates.  A definition's
	// object is the entity's gloss.  Rebuilt with the view.
	static constexpr std::size_t npos = std::size_t(-1);
	struct object {
		std::size_t at;            // into consolidated()
		std::size_t entity = npos; // the object names this entity
	};
	struct predicate {
		std::string         title;
		std::vector<object> objects;
	};
	struct entity {
		agenda::id             id;    // the name's identity
		std::string            title;
		std::size_t            definition = npos; // into consolidated()
		std::vector<predicate> predicates;
	};
	std::vector<entity> const &entities();
	// An entity's identity is its folded name; a same edge between
	// two of them -- a verdict that SLAWE and SLEIGH name one thing
	// -- makes them one entity, titled as the first was.
	agenda::id entity_id(std::string_view title) const;
	// Names the corpus lexicon spells as one term, as groups of
	// entity ids: they unite in the view as a same verdict unites
	// them.  The terms pass asked the model which observed
	// spellings belong together and the harvest saw every spelling
	// on a cited cue; that answer is identity here, and no judge
	// is asked what the lexicon already said.  Replaces the groups
	// before it; a name no group holds is an entity of its own.
	void aliases(std::vector<std::vector<agenda::id>> groups);
	std::size_t entity_of(agenda::id id);
	// Whether two records sit in one concept of the view -- no
	// verdict is owed for a pair identity already united.
	bool equivalent(agenda::id a, agenda::id b);

	// The relation graph's readers, over the concept view.  ties()
	// counts a concept's edges of each kind beyond the equivalences
	// that formed it; partners() names the records a concept's
	// members stand in the given relation to.  A judgment the model
	// spent work on is only worth asking for if something reads it:
	// contradictions reach the answer bundle, counts reach the pane.
	struct tie_count {
		std::uint32_t related = 0;
		std::uint32_t contradicts = 0;
	};
	// Positions are consolidated()'s; both refresh the view first
	// and answer nothing for a position it does not hold.
	tie_count ties(std::size_t at);
	std::vector<agenda::id> partners(std::size_t at, relation what);

private:
	bool adopt(record r);
	agenda::id pair_id(edge const &e) const;
	std::size_t place(std::string_view source) const;

	std::string m_dir;
	std::vector<std::string> m_order; // sources, corpus order
	std::error_code m_error;
	hash8_fn m_hash;
	vocab &m_vocab;
	std::vector<record> m_records;
	std::vector<std::vector<std::uint32_t>> m_words; // per record
	agenda::index m_at;      // record id -> m_records
	std::vector<record> m_view;                 // consolidated()
	std::vector<std::vector<std::uint32_t>> m_viewWords;
	std::vector<std::vector<agenda::id>> m_viewMembers;
	std::vector<tie_count> m_viewTies;
	std::vector<entity> m_entities;
	agenda::index m_entityAt; // entity id -> m_entities, merged
	std::vector<std::vector<agenda::id>> m_aliases; // lexicon groups
	std::vector<std::size_t> m_conceptOf; // record index -> concept
	bool m_stale = true;                        // view behind records/edges
	std::vector<edge> m_edges;
	agenda::index m_edgeAt;  // pair id -> m_edges
	std::vector<edge> m_staged; // pairs asked; what and rationale unused
	agenda::index m_pending; // pairs staged since load: membership
	                         // only, m_staged shrinks as pairs link
};

// The best `limit` items of a stream, kept in order by insertion
// into a short vector.  The candidate pool is the whole corpus and
// the survivors are a dozen: neither memory nor time should scale
// with what a corpus has to say about a common word, and a sorted
// dozen costs no sort at all.  before(a, b) orders the best first.
template <class T, class Before>
void keep_best(std::vector<T> &best, T item, std::size_t limit,
               Before before)
{
	if (!limit || (best.size() == limit && !before(item, best.back())))
		return;
	auto at = best.begin();
	while (at != best.end() && !before(item, *at))
		++at;
	best.insert(at, std::move(item));
	if (best.size() > limit)
		best.pop_back();
}

// JSON Schemas passed verbatim to llama-server response_format.
std::string_view records_schema();
std::string_view verdict_schema();
std::string_view answer_schema();

} // namespace semantic

#endif // SRTVIEW_SRC_SEMANTIC_HPP_
