// refinery.cpp -- see refinery.hpp.  The machine bodies moved here
// from the composition root verbatim; their per-function comments
// are the authoritative docs, as everywhere.
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <utility>

#include "dbg.hpp"
#include "hashq.hpp"
#include "refinery.hpp"
#include "srt.hpp"

namespace {

// Focus fan-out per new dive: the all-pairs plan is quadratic and
// would mostly buy paced NONEs from weakly related pairs.
constexpr std::size_t kFocusFan = 3;


// Excerpt budget per dive, in UTF-8 bytes: roughly half the llm
// clip so the context sections keep their share of the window.
constexpr std::size_t kDiveBudget = std::size_t{48} * 1024;

// Per-side slice of a dive's excerpts fed to a probe: calibration
// for the variant sweep, not coverage, so the head of the catch is
// enough and two sides fit beside the pair's prose.
constexpr std::size_t kProbeSample = std::size_t{16} * 1024;

// The head of an excerpt block, cut at a line boundary; excerpt
// lines are newline-terminated, so a no-fit only happens when the
// first line alone overflows the slice.
std::string_view sampleOf(std::string const &parts)
{
	if (parts.size() <= kProbeSample)
		return parts;
	std::size_t const cut = parts.rfind('\n', kProbeSample);
	return cut == std::string::npos
	       ? std::string_view()
	       : std::string_view(parts.data(), cut + 1);
}




// Pair identities from the unordered dive pair, sorted so the same
// two dives name the same artifacts whichever finished first: the
// focus is the written thread, the probe the ask for what to search
// toward it, and the probe's one retry is salted apart.
agenda::id pairId(std::string_view tag, agenda::id a, agenda::id b)
{
	if (b.b < a.b)
		std::swap(a, b);
	QCryptographicHash h(QCryptographicHash::Blake2b_256);
	h.addData(QByteArrayView(tag.data(), qsizetype(tag.size())));
	h.addData(QByteArrayView(
		reinterpret_cast<char const *>(a.b.data()),
		qsizetype(a.b.size())));
	h.addData(QByteArrayView(
		reinterpret_cast<char const *>(b.b.data()),
		qsizetype(b.b.size())));
	return takeId(h);
}

agenda::id focusId(agenda::id a, agenda::id b)
{
	return pairId("focus", a, b);
}

agenda::id probeId(agenda::id a, agenda::id b, bool retry)
{
	return pairId(retry ? "probe!" : "probe", a, b);
}

std::size_t sharedKeys(std::vector<agenda::id> const &a,
                       std::vector<agenda::id> const &b)
{
	std::size_t n = 0;
	for (agenda::id const k : a)
		n += std::ranges::find(b, k) != b.end();
	return n;
}


} // namespace

// Dive identity is the expanded pattern's hash: editing a topic
// re-dives it, and identical patterns share one cache file.
agenda::id Refinery::diveId(std::string const &pattern)
{
	QCryptographicHash h(QCryptographicHash::Blake2b_256);
	h.addData(QByteArrayView("dive\n"));
	h.addData(QByteArrayView(pattern.data(),
	                         qsizetype(pattern.size())));
	return takeId(h);
}

// The payload of a REGEX: line: [from, end) with blanks trimmed off
// the front and trailing controls off the back.
std::string Refinery::regexPayload(std::string const &text, std::size_t from,
                         std::size_t end)
{
	while (from < end && (text[from] == ' ' || text[from] == '\t'))
		++from;
	while (end > from
	       && static_cast<unsigned char>(text[end - 1]) <= 0x20)
		--end;
	return text.substr(from, end - from);
}

// The last line-anchored REGEX: line of a reply -- the probe prompt
// allows working notes above the answer, and the one-shot focus
// files closed with it.
std::string Refinery::regexLine(std::string const &text)
{
	std::size_t const at = text.rfind("REGEX:");
	if (at == std::string::npos || (at && text[at - 1] != '\n'))
		return {};
	std::size_t end = text.find('\n', at + 6);
	if (end == std::string::npos)
		end = text.size();
	return regexPayload(text, at + 6, end);
}

Refinery::Refinery(Facts &facts,
                   engine::SemanticEngine<Facts> &semantic,
                   topics::doc &corpus,
                   exporter::transcripts &transcripts,
                   refinery_host *host)
	: m_facts(facts), m_semantic(semantic), m_corpus(corpus),
	  m_transcripts(transcripts), m_host(host)
{
	m_diveTick.setInterval(1);
	m_diveTick.setSingleShot(false);
	QObject::connect(&m_diveTick, &QTimer::timeout,
	                 &m_diveTick, [this] { diveStep(); });
	// The completion-poked dispatch wires itself: Facts pokes on
	// whichever thread lands an artifact, and one queued call per
	// burst runs the chain on the owning thread (the timer is the
	// thread-affine context object).
	m_facts.setPoke(&Refinery::poke, this);
}

Refinery::~Refinery()
{
	// The set is the barrier: it returns only once no worker is
	// inside the callback, and a call it beat to the lock queued
	// onto m_diveTick, whose own destruction sweeps the queue.
	// The owner destroys Facts after this object, so the reference
	// is good for the detach.
	m_facts.setPoke(nullptr, nullptr);
}

void Refinery::poke(void *self) noexcept
{
	auto *const r = static_cast<Refinery *>(self);
	QMetaObject::invokeMethod(&r->m_diveTick,
		[r] { r->harvestChain(false); },
		Qt::QueuedConnection);
}

void Refinery::reset(bool fresh)
{
	if (!fresh)
		return;
	// A replaced corpus stops its scans on the spot.  The identity
	// seam leaves a hashing window between this reset and the new
	// queueDives(); a tick surviving into it would keep scanning
	// the old sources and submit the old corpus's dives into the
	// new pipeline, which nothing would ever retire.
	m_diveScans.clear();
	m_diveAt = 0;
	m_diveTick.stop();
	m_sources.clear();
	m_dives.clear();
	m_focusWork.clear();
	m_focusOrder.clear();
	m_focusFacts.clear();
	m_generated.clear();
	m_harvested.clear();
	m_termsWork.clear();
	m_termFacts.clear();
	m_supportSeen.clear();
	m_diveRetired.clear();
	m_termTopics.clear();
	m_termInfo.clear();
	m_termIndex.clear();
}

void Refinery::setSources(QList<refinery_source> sources)
{
	m_sources = std::move(sources);
	// The support verdicts scanned the old source set; an extended
	// corpus can lift a term over the floor, a shrunken one drop
	// it.  The parse facts stay -- they are window-pure.
	m_supportSeen.clear();
}

// Reloaded machine topics keep their roles by name stem: term* stay
// index-only (no dives, no pending badge) and focus* stay generated
// (supportive, never pairing), before the harvest re-attaches titles
// from the cached replies.  A hand topic borrowing the shape is
// treated as machine -- deterministic, and adopt() never mints a
// colliding name.  Name-keyed and idempotent, so extension rebuilds
// re-seed for free.
void Refinery::seedGenerated()
{
	for (topics::topic const &t : m_corpus.topics) {
		bool const term = topics::stem_name(t.name, "term");
		if (term)
			m_termTopics.insert(t.name);
		if (term || topics::stem_name(t.name, "focus"))
			m_generated.insert(t.name);
	}
}

// Stage the corpus topic dives: one scan per exported grouping,
// chewed a video per tick.  Reopening a summarized corpus still
// scans (a few ms per cell, spread out); Facts drops the finished
// scan against its cache.
void Refinery::queueDives(bool fresh)
{
	if (fresh) {
		m_diveScans.clear();
		m_diveAt = 0;
	}
	for (topics::export_item const &e :
	     topics::export_plan(m_corpus)) {
		// Term topics dive too: their essays are what feed the
		// pairing layer, and pairs are where single spellings
		// crystallize into grouped regexes -- on a corpus with
		// no hand topics they are the only road there.  The
		// generated flag means "born from a focus" (those must
		// not re-pair into probe-of-probe loops); terms are
		// first-generation and pair like hand topics.
		bool const gen = m_generated.contains(e.name);
		stageDive(e.pattern, !gen,
		          gen && !m_termTopics.contains(e.name));
	}
	// The supportive layer: referenced topics dive too, a band
	// lower and unexported -- the nested regexes reveal semantic
	// structure inside the tops, and the queue can lean on it.
	for (topics::topic const *t : topics::components(m_corpus))
		stageDive(topics::expand(m_corpus, *t), false, false);
	if (m_diveAt < m_diveScans.size())
		m_diveTick.start();
	else
		m_diveTick.stop();
}

// Already-staged ids are left alone, finished or in flight: a merge
// restage must not reset a scan's progress.
void Refinery::stageDive(std::string const &pattern, bool exported,
                        bool generated)
{
	DiveScan s;
	s.re = QRegularExpression(QString::fromStdString(pattern));
	if (!s.re.isValid())
		return;
	s.id = diveId(pattern);
	// A pattern the fold retired and later re-blessed un-retires
	// here: the mark meant "this exact pattern is gone", and a
	// deliberate stage says otherwise -- without this, a term
	// topic that left the directory in one pass and returned in a
	// later one could never scan again for the session.  Its
	// abandoned husks -- excerpts dropped, cursor advanced -- are
	// removed so the fresh scan starts whole.
	if (m_diveRetired.erase(s.id.hex()))
		for (std::size_t i = m_diveScans.size(); i-- > 0;)
			if (m_diveScans[i].id == s.id) {
				m_diveScans.erase(m_diveScans.begin()
				                  + std::ptrdiff_t(i));
				if (i < m_diveAt)
					--m_diveAt;
			}
	for (DiveScan const &d : m_diveScans)
		if (d.id == s.id)
			return;
	s.pattern = pattern;
	s.exported = exported;
	s.generated = generated;
	m_diveScans.push_back(std::move(s));
	// The stager wakes its own worker: leaving the restart to
	// whoever happened to run later made the harvest chain's call
	// order load-bearing by accident.
	m_diveTick.start();
}

void Refinery::diveStep()
{
	if (m_diveAt >= m_diveScans.size() || m_sources.isEmpty()) {
		m_diveScans.clear();
		m_diveAt = 0;
		m_diveTick.stop();
		return;
	}
	DiveScan &s = m_diveScans[m_diveAt];
	// A scan retired mid-flight is abandoned at the next tick:
	// finishDive() would only discard its output anyway, and the
	// remaining per-video regex passes are pure waste.
	if (m_diveRetired.contains(s.id.hex())) {
		s.parts.clear();
		s.parts.shrink_to_fit();
		++m_diveAt;
		return;
	}
	if (s.video >= std::size_t(m_sources.size())) {
		finishDive(s);
		++m_diveAt;
		return;
	}
	scanDiveVideo(s, m_sources[qsizetype(s.video)]);
	++s.video;
}

// One (topic, video) cell: matched cue lines become an excerpt
// section and the video's leaf a dependency.  The budget binds per
// append: an oversized catch is cut at a line boundary, and a video
// none of whose lines fit is dropped whole, section and dependency
// both -- the dive never cites a video it did not quote.
void Refinery::scanDiveVideo(DiveScan &s, refinery_source const &it)
{
	if (s.parts.size() >= kDiveBudget)
		return;
	std::size_t const room = kDiveBudget - s.parts.size();
	QString const srt = it.srt;
	std::string hits;
	for (QString const &line :
	     exporter::load(m_transcripts, srt).lines) {
		if (!s.re.match(line).hasMatch())
			continue;
		hits += line.toStdString();
		hits += '\n';
		// Enough for any final cut: past the room, the trim
		// below only ever shrinks -- no point holding more.
		if (hits.size() >= room)
			break;
	}
	if (hits.empty())
		return;
	agenda::id const key = it.leaf;
	if (!key || std::ranges::find(s.deps, key) != s.deps.end())
		return;
	std::string head = "== ";
	head += QFileInfo(it.video).fileName().toStdString();
	head += '\n';
	if (head.size() >= room)
		return;
	if (hits.size() > room - head.size()) {
		std::size_t const cut =
			hits.rfind('\n', room - head.size() - 1);
		if (cut == std::string::npos)
			return;
		hits.resize(cut + 1);
	}
	s.deps.push_back(key);
	s.parts += head;
	s.parts += hits;
}

// A finished scan becomes a dive task: deps gate on the hit videos'
// leaf summaries, heat follows the same videos, and the pyramid
// root rides along as optional overview context.  The scan entry
// stays staged (its id blocks re-staging) but sheds its excerpts.
// A scan a pending focus record claims is that record's search, not
// a dive: it routes to finishProbe() instead.
void Refinery::finishDive(DiveScan &s)
{
	// A topic extended mid-scan supersedes this dive: asking or
	// pairing the pre-extension pattern would burn budget a warm
	// replay (which only ever sees the final pattern) never burns.
	if (m_diveRetired.contains(s.id.hex())) {
		s.parts.clear();
		s.parts.shrink_to_fit();
		return;
	}
	std::size_t const at = focusWorkOf(s.id);
	if (at < m_focusWork.size()) {
		if (!finishProbe(s, m_focusWork[at]))
			m_focusWork.erase(m_focusWork.begin()
			                  + std::ptrdiff_t(at));
	} else if (!s.deps.empty()) {
		agenda::task t;
		t.id = s.id;
		t.deps = s.deps;
		t.keys = s.deps;
		t.note = s.pattern;
		t.what = agenda::kind::dive;
		t.exported = s.exported;
		if (m_rootId)
			t.refs.push_back(m_rootId);
		m_facts.offer(std::move(t), s.parts);
		pairFocus(s);
	}
	s.parts.clear();
	s.parts.shrink_to_fit();
}

// The focus trigger: a finished first-generation dive pairs with
// every earlier one sharing a hit video.  A pair no longer asks for
// its focus outright: it opens with a probe -- what would you
// search? -- and the pump chains the search and the write behind
// it.  Generated dives never pair: recursion stops one hop past the
// hypothesis.
void Refinery::pairFocus(DiveScan const &s)
{
	if (s.generated)
		return;
	// Overlap-ranked, capped: at most kFocusFan partners per new
	// dive, the shared-hit-video count as the relatedness prior
	// and recency breaking ties.  Old dives may still accumulate
	// pairs as later ones pick them, so the total stays linear.
	struct pick {
		std::size_t at;
		std::size_t overlap;
	};
	std::vector<pick> best;
	for (std::size_t i = 0; i < m_dives.size(); ++i) {
		std::size_t const n = sharedKeys(m_dives[i].keys, s.deps);
		if (n)
			best.push_back({i, n});
	}
	std::ranges::sort(best, [](pick const &a, pick const &b) {
		return a.overlap != b.overlap ? a.overlap > b.overlap
		                              : a.at > b.at;
	});
	if (best.size() > kFocusFan)
		best.resize(kFocusFan);
	for (pick const &p : best)
		stageProbe(m_dives[p.at], s);
	// Copied before finishDive() sheds the scan's excerpts: the
	// record grounds the probes of future partners.
	m_dives.push_back({s.id, s.deps, s.pattern, s.parts});
}

// One pair's opening move.  An existing focus file ends the pair's
// story -- the one-shot era's artifacts included -- and a pending
// record means the story is already moving; otherwise the probe is
// staged (a cached reply asks nothing) and a record starts tracking
// the chain.  The ask carries a TRANSCRIPT sample of both sides'
// matched lines -- raw speech-to-text to ground the variant sweep.
// The probe depends on the two dive files, so "at least two dives"
// still falls out of dependency gating.
void Refinery::stageProbe(FinishedDive const &a, DiveScan const &b)
{
	agenda::id const fid = focusId(a.id, b.id);
	for (PendingFocus const &w : m_focusWork)
		if (w.focus == fid)
			return;
	agenda::task written;
	written.id = fid;
	written.deps = {a.id, b.id};
	written.what = agenda::kind::focus;
	// A written thread re-enters the corpus only through its own
	// pair -- this door -- so another corpus's focus files can
	// never bleed in.  cached() adopts a stale name en passant
	// when the dive chain is computable; on a cold start the dive
	// prose may still be pending, so bare existence via locate()
	// must also count -- otherwise a cached thread gets re-probed.
	if (!std::ranges::any_of(m_focusOrder,
		[&fid](FocusPair const &p) { return p.id == fid; }))
		m_focusOrder.push_back({fid, a.id, b.id});
	if (m_facts.cached(written)
	    || !m_facts.locate(fid, agenda::kind::focus).empty())
		return;         // the fold reads the landed file
	PendingFocus w;
	w.probe = probeId(a.id, b.id, false);
	w.focus = fid;
	w.deps = {a.id, b.id};
	w.keys = a.keys;
	for (agenda::id const k : b.deps)
		if (std::ranges::find(w.keys, k) == w.keys.end())
			w.keys.push_back(k);
	w.note = a.pattern + " ~ " + b.pattern;
	w.raw = "TRANSCRIPT\n";
	w.raw += sampleOf(a.parts);
	w.raw += sampleOf(b.parts);
	m_facts.offer(probeTask(w, w.probe), w.raw);
	m_focusWork.push_back(std::move(w));
}

// The ask itself: probe and retry share everything but the id.
agenda::task Refinery::probeTask(PendingFocus const &w,
                                agenda::id ask) const
{
	agenda::task t;
	t.id = ask;
	t.deps = w.deps;
	t.keys = w.keys;
	t.note = w.note;
	t.what = agenda::kind::probe;
	t.exported = false;
	return t;
}

// The probe pump, the interactive half of a focus.  Each record
// waits on its ask's cache file: NONE retires the pair, a missing
// or broken REGEX line earns one corrected attempt with the failure
// as FEEDBACK, and a valid regex becomes a corpus search routed
// back to the record when it completes.  Cache files gate every
// step, so a chain interrupted by shutdown resumes where it stood.
void Refinery::pumpProbes()
{
	for (std::size_t i = 0; i < m_focusWork.size();) {
		if (pumpProbe(m_focusWork[i]))
			++i;
		else
			m_focusWork.erase(m_focusWork.begin()
			                  + std::ptrdiff_t(i));
	}
}

// False retires the record.
bool Refinery::pumpProbe(PendingFocus &w)
{
	if (w.scanning)
		return true;
	std::string const text = m_facts.fetch(
		probeTask(w, w.retry ? w.retry : w.probe));
	if (text.empty())
		return true;   // unanswered; waiting is free
	if (text.starts_with("NONE"))
		return false;
	// Tidied mechanically before anything consumes it: small models
	// repeat branches freely, and the searched, stored and
	// journaled pattern must not carry that.
	std::string const pat = topics::tidy(regexLine(text));
	if (pat.empty())
		return retryProbe(w,
			"FEEDBACK\nYour reply did not end with a REGEX: "
			"line. Reply with exactly one line of the form "
			"REGEX: <pattern>, or NONE.");
	QRegularExpression const re(QString::fromStdString(pat));
	if (!re.isValid())
		return retryProbe(w,
			"FEEDBACK\nYour regex\n  " + pat
			+ "\nis not valid PCRE2: "
			+ re.errorString().toStdString()
			+ ". Reply with a corrected REGEX: line, or NONE.");
	w.scanning = true;
	stageFocusScan(w.focus, pat, re);
	return true;
}

// The one corrected attempt: at temperature zero a bare re-ask is a
// re-run, so the retry exists only because FEEDBACK changes the
// prompt; the TRANSCRIPT sample rides along again so the evidence
// stays in view.  A second failure retires the pair -- false, like
// the pump's.
bool Refinery::retryProbe(PendingFocus &w, std::string const &feedback)
{
	if (w.retry)
		return false;
	w.retry = probeId(w.deps[0], w.deps[1], true);
	m_facts.offer(probeTask(w, w.retry), w.raw + "\n---\n" + feedback);
	return true;
}

// The probe's validated hypothesis becomes a corpus search staged
// under the write task's id; finishDive() routes it back through
// the pending record.  No m_diveScans dedupe here: the record's
// scanning latch is the guard, and a zero-match retry legitimately
// stages the same id again with a broader pattern.
void Refinery::stageFocusScan(agenda::id id, std::string const &pattern,
                             QRegularExpression const &re)
{
	DiveScan s;
	s.re = re;
	s.id = id;
	s.pattern = pattern;
	s.exported = false;
	s.generated = true;
	m_diveScans.push_back(std::move(s));
	m_diveTick.start();
}

// The searched evidence stages the write: the pair's dives ride as
// FIRST/SECOND deps, the excerpts as the snapshot, and the regex as
// the note the REGEX head and the journal carry.  An empty search
// is the probe's failure to answer for -- one broadening retry,
// then the pair retires.  False retires the record.
bool Refinery::finishProbe(DiveScan const &s, PendingFocus &w)
{
	if (s.parts.empty()) {
		w.scanning = false;
		return retryProbe(w,
			"FEEDBACK\nYour regex\n  " + s.pattern
			+ "\nis valid but matched nothing in the "
			"collection's subtitles. Broaden the variants: "
			"loosen separators and word joints, allow "
			"sound-alike respellings and optional inflections "
			"-- or reply NONE.");
	}
	agenda::task t;
	t.id = w.focus;
	t.deps = w.deps;
	t.keys = w.keys;
	t.note = s.pattern;
	t.what = agenda::kind::focus;
	t.exported = false;
	m_facts.offer(std::move(t), s.parts);
	// The write lands asynchronously; its landing pokes the
	// dispatch and the fold reads the file by pair id.
	return false;
}

// The focus facts stage: one landed pair file parsed once into
// its pattern -- empty for NONE, a buried hypothesis, a malformed
// or invalid regex.  artifact() resolves by bare id on purpose:
// it adopts a stale name the moment the pair's prose makes the
// chain computable, so the journaled adoption lands in the
// session that owns it.
bool Refinery::focusFactOf(agenda::id id)
{
	QString const hex = QString::fromStdString(id.hex());
	if (m_focusFacts.contains(hex))
		return true;
	std::string const p = m_facts.artifact(id);
	if (p.empty())
		return false;                // not yet landed or ripe
	QFile f(QString::fromStdString(p));
	if (!f.open(QIODevice::ReadOnly))
		return false;
	std::string const text = f.readAll().toStdString();
	QString pattern;
	std::string pat;
	if (text.starts_with("REGEX:")) {
		// The interactive shape: a machine-written head names
		// the searched regex and prose follows -- unless the
		// model saw the evidence and still judged the thread
		// hollow, which buries the hypothesis with it.
		std::size_t nl = text.find('\n');
		if (nl == std::string::npos)
			nl = text.size();
		pat = regexPayload(text, 6, nl);
		std::size_t body = nl;
		while (body < text.size() && text[body] == '\n')
			++body;
		if (text.compare(body, 4, "NONE") == 0)
			pat.clear();
	} else if (!text.starts_with("NONE")) {
		// The one-shot shape closed with the line instead.
		pat = regexLine(text);
	}
	if (!pat.empty()
	    && QRegularExpression(
		QString::fromStdString(pat)).isValid())
		pattern = QString::fromStdString(pat);
	m_focusFacts.insert(hex, pattern);
	return true;
}

// The completion-poked dispatch: one pass over every harvest
// machine, in the one canonical order -- probes advance first,
// terms adopt before focus writes fold (the stated invariant:
// every answered window has adopted by the time focus runs), the
// lexicon feeds before the next semantic tick, and the pane
// paints last.  The swept counter is the engine's own idiom: a
// wake that finds nothing landed since the last pass returns at
// once, so a burst of completions costs one pass and idle time
// costs none.  kick runs the pass regardless -- a re-cut or a
// user action changed inputs no landing counts.
void Refinery::harvestChain(bool kick)
{
	std::uint64_t const landed = m_facts.landed();
	if (!kick && landed == m_harvestSwept)
		return;
	m_harvestSwept = landed;
	dbgHop(QStringLiteral("harvest: wake (landed %1%2)")
	       .arg(landed)
	       .arg(kick ? QStringLiteral(", kicked") : QString()));
	pumpProbes();
	refold();
	feedLexicon();
	m_host->refineryChanged();
}

// Stage the terms windows: cue-boundary slices of each transcript,
// numbered with absolute cue indices and timestamps.  Offers dedupe
// against the plan and the cache; records re-derive every session
// so cached replies stay mappable to their cue ranges.
void Refinery::queueTerms()
{
	// No adoption gate remains: the witness gates the asks, so no
	// facts can exist for an unread cut, and the fold reads facts
	// only for the windows THIS cut staged -- a stale generation's
	// replies exclude themselves by identity.
	// Staged ids as a set, built once: a per-window linear scan of
	// m_termsWork would go quadratic as the corpus grows.
	std::set<agenda::id> staged;
	for (TermsWork const &w : m_termsWork)
		staged.insert(w.id);
	// The engine's windows, by the same source identity
	// rebuildSemantic() cut them under -- the (video, subtitle)
	// pair, the subtitle hash alone for the unresolvable: one cut
	// of the corpus serves extraction and terms, and the model
	// sees the identical text for both.
	QHash<QString, qsizetype> bySource;
	for (qsizetype i = 0; i < m_sources.size(); ++i) {
		refinery_source const &it = m_sources[i];
		QString const srt = it.srt;
		if (srt.isEmpty())
			continue;
		agenda::id const subtitles = it.leaf;
		if (agenda::id const source =
			semanticSourceId(it.id, subtitles))
			bySource.insert(QString::fromStdString(
				source.hex()), i);
	}
	for (std::size_t at = 0; at < m_semantic.windows(); ++at) {
		semantic::window const &w = m_semantic.window(at);
		agenda::id const id = m_semantic.key("terms", w);
		qsizetype const i = bySource.value(QString::fromUtf8(
			w.source.data(), qsizetype(w.source.size())), -1);
		if (i < 0 || !staged.insert(id).second)
			continue;
		refinery_source const &it = m_sources[i];
		agenda::task t;
		t.id = id;
		t.keys = {agenda::id::from_hex(w.source)};
		t.note = QFileInfo(it.video).fileName().toStdString()
		       + " #" + std::to_string(w.cues.front().number)
		       + "-" + std::to_string(w.cues.back().number);
		t.what = agenda::kind::terms;
		t.exported = false;
		// Frame-sensitive like extract: the ask waits for the
		// cut's ground witness (the harvest gate above already
		// guards adoption -- that half stays until adoption
		// writes per-cut projections).
		if (agenda::id const wit = m_semantic.witness())
			t.deps.push_back(wit);
		m_facts.offer(std::move(t), engine::window_body(w));
		m_termsWork.push_back({id, it.video, it.srt,
		                       int(w.cues.front().number),
		                       int(w.cues.back().number)});
	}
}

// The facts stage: parse and validate one landed terms reply into
// an immutable fact -- the mechanical gate unchanged (every cited
// cue inside the window, every SEEN spelling on a cited line, an
// entry failing either drops whole), plus the support floor's
// corpus scan memoized per entry, since it depends on transcripts
// alone.  A fact never invalidates: window ids are content-
// addressed, so the same id always names the same text.
bool Refinery::factOf(TermsWork const &w)
{
	QString const hex = QString::fromStdString(w.id.hex());
	if (m_termFacts.contains(hex))
		return true;
	std::string const path = m_facts.locate(w.id,
	                                        agenda::kind::terms);
	if (path.empty())
		return false;   // unanswered: QFile("") would gripe
	QFile f(QString::fromStdString(path));
	if (!f.open(QIODevice::ReadOnly))
		return false;
	QString const text = QString::fromUtf8(f.readAll());
	TermFact fact;
	if (text.startsWith(QStringLiteral("NONE"))) {
		m_termFacts.insert(hex, fact);
		return true;
	}
	exporter::transcript const &tx =
		exporter::load(m_transcripts, w.srt);
	auto const line = [&](int cue) -> QString const * {
		return cue >= w.first && cue <= w.last
		       && cue < int(tx.lines.size())
		       ? &tx.lines[cue] : nullptr;
	};
	for (QString const &block :
	     text.split(QStringLiteral("\n\n"), Qt::SkipEmptyParts)) {
		QString term, kind, gloss, means;
		QStringList seen;
		QList<int> cues;
		for (QString const &l : block.split(QLatin1Char('\n'))) {
			if (l.startsWith(QStringLiteral("TERM:")))
				term = l.mid(5).trimmed();
			else if (l.startsWith(QStringLiteral("KIND:")))
				kind = l.mid(5).trimmed().toLower();
			else if (l.startsWith(QStringLiteral("MEANS:")))
				means = l.mid(6).trimmed();
			else if (l.startsWith(QStringLiteral("GLOSS:")))
				gloss = l.mid(6).trimmed();
			else if (l.startsWith(QStringLiteral("SEEN:"))) {
				for (QString const &v : l.mid(5)
				     .split(QLatin1Char('|')))
					if (QString const s = v.trimmed();
					    !s.isEmpty())
						seen << s;
			} else if (l.startsWith(QStringLiteral("CUES:"))) {
				for (QString const &c : l.mid(5)
				     .split(QLatin1Char(' '),
				            Qt::SkipEmptyParts)) {
					bool ok = false;
					int const n = QStringView(c)
						.sliced(c.startsWith(
							QLatin1Char('#')))
						.toInt(&ok);
					if (ok)
						cues << n;
				}
			}
		}
		if (term.isEmpty() || seen.isEmpty() || gloss.isEmpty()
		    || cues.isEmpty())
			continue;
		if (!std::ranges::all_of(cues, [&](int c) {
				return line(c) != nullptr;
			}))
			continue;
		QStringList kept;
		for (QString const &v : seen) {
			bool const found = std::ranges::any_of(cues,
				[&](int c) {
					return line(c)->contains(v,
						Qt::CaseInsensitive);
				});
			if (found)
				kept << v;
		}
		if (kept.isEmpty())
			continue;
		QString pat = QStringLiteral("(?i:");
		for (qsizetype i = 0; i < kept.size(); ++i) {
			if (i)
				pat += QLatin1Char('|');
			pat += QRegularExpression::escape(kept[i]);
		}
		pat += QLatin1Char(')');
		TermEntry e;
		e.term = term;
		e.kind = kind;
		e.tidied = topics::tidy(pat.toStdString());
		e.kept = kept;
		// The row's title already IS the term: the gloss text
		// never repeats it, an acronym's expansion just leads.
		e.shown = means.isEmpty()
		    || !QString::compare(means, term, Qt::CaseInsensitive)
			? gloss
			: means + QStringLiteral(". ") + gloss;
		// Only the floor scan's pattern is a fact of the window;
		// its verdict counts hits across the current sources and
		// belongs to the fold, memoized until the sources change.
		e.support = QString::fromStdString(e.tidied)
			+ QLatin1Char('|') + termMatcher(term).pattern();
		fact.entries.push_back(std::move(e));
	}
	m_termFacts.insert(hex, std::move(fact));
	return true;
}

// The fold's adoption of one fact entry -- the same one-term-one-
// topic logic adoption always had, minus the dive side effects,
// which diveSync() derives from the final directory instead.
void Refinery::adoptEntry(TermEntry const &e)
{
	QString const folded = e.term.toCaseFolded();
	// The support floor: a novel term below it founds nothing;
	// known terms are exempt, so a rare variant still merges.
	if (!m_termIndex.contains(folded) && !supportedOf(e)) {
		dbgHop(QStringLiteral("terms: floored [%1]")
		       .arg(e.term));
		return;
	}
	QString own = m_termIndex.value(folded);
	if (own.isEmpty())
		own = QString::fromStdString(
			topics::cover_of(m_corpus, e.tidied, "term"));
	if (!own.isEmpty()) {
		topics::extend(m_corpus, own.toStdString(), e.tidied);
		TermInfo &info = m_termInfo[own];
		if (info.term.isEmpty())
			info.term = e.term;
		if (info.kind.isEmpty())
			info.kind = e.kind;
		if (info.gloss.isEmpty())
			info.gloss = e.shown;
		if (!m_termIndex.contains(folded))
			m_termIndex.insert(folded, own);
		indexSpellings(e.kept, own);
		return;
	}
	std::string const adopted = topics::adopt_novel(
		m_corpus, e.tidied, "term");
	if (adopted.empty())
		return;         // hand-covered whole: no twin, no title
	QString const name = QString::fromStdString(
		m_corpus.topics.back().name);
	m_generated.insert(name.toStdString());
	m_termTopics.insert(name.toStdString());
	m_termInfo.insert(name, {e.term, e.kind, e.shown});
	m_termIndex.insert(folded, name);
	indexSpellings(e.kept, name);
}

// The support floor's corpus half: two hits anywhere in the current
// sources.  Cached per pattern and cleared when the source set
// changes -- the verdict is corpus-relative, a fact of the corpus
// the fold runs against, never of the window that surfaced the term.
bool Refinery::supportedOf(TermEntry const &e)
{
	auto const it = m_supportSeen.constFind(e.support);
	if (it != m_supportSeen.constEnd())
		return it.value();
	bool const ok = corpusHits(QRegularExpression(e.support,
		QRegularExpression::CaseInsensitiveOption
		| QRegularExpression::UseUnicodePropertiesOption),
		2) >= 2;
	m_supportSeen.insert(e.support, ok);
	return ok;
}

// Corpus-wide match count for a pattern, stopping at cap: the
// support floor needs "fewer than two", never the full tally.
int Refinery::corpusHits(QRegularExpression const &re, int cap)
{
	if (!re.isValid())
		return 0;
	int n = 0;
	for (refinery_source const &it : m_sources) {
		QString const srt = it.srt;
		if (srt.isEmpty())
			continue;
		for (QString const &line :
		     exporter::load(m_transcripts, srt).lines) {
			n += re.match(line).hasMatch();
			if (n >= cap)
				return n;
		}
	}
	return n;
}

// The expanded pattern of a named topic; empty when the name is
// not currently a topic.
std::string Refinery::expandOf(std::string const &name) const
{
	topics::topic const *const tp = topics::find(m_corpus, name);
	return tp ? topics::expand(m_corpus, *tp) : std::string();
}

// A superseded dive neither records nor pairs nor asks -- and any
// probe chain already staged on it stops before concluding a focus
// from the stale pattern: budget a warm replay never burns.
void Refinery::retireDive(agenda::id id)
{
	m_diveRetired.insert(id.hex());
	std::erase_if(m_dives, [&id](FinishedDive const &d) {
		return d.id == id;
	});
	std::erase_if(m_focusWork, [&id](PendingFocus const &w) {
		return std::ranges::find(w.deps, id) != w.deps.end();
	});
	// A retired dive's pairs leave the fold order: their files
	// stay as cache, but the fold stops adopting from the stale
	// pattern -- a pair a warm replay never stages.
	std::erase_if(m_focusOrder, [&id](FocusPair const &p) {
		return p.a == id || p.b == id;
	});
}

// Every validated spelling joins the index, first owner wins: a
// later window proposing a known VARIANT as its term ("TERM:
// gidger" after gidger was seen under ghidra) must grow the owner,
// not mint a titled twin.
void Refinery::indexSpellings(QStringList const &seen,
                             QString const &owner)
{
	for (QString const &v : seen)
		if (QString const k = v.toCaseFolded();
		    !m_termIndex.contains(k))
			m_termIndex.insert(k, owner);
}

// A term occurrence never starts mid-word: "AI" must not count
// inside "said", and substitution must not rewrite the middle of
// "start".  The boundary is left-only -- suffix-inflected corpora
// ("Jiran" for Jira) still match -- and \p{L} keeps it orthography-
// neutral rather than ASCII-bound.
QRegularExpression Refinery::termMatcher(QString const &term)
{
	return QRegularExpression(
		QStringLiteral("(?<!\\p{L})")
		+ QRegularExpression::escape(term),
		QRegularExpression::CaseInsensitiveOption
		| QRegularExpression::UseUnicodePropertiesOption);
}

// Up to cap transcript lines containing the term, in corpus order:
// the verdict's evidence.  Deterministic for a fixed corpus, so the
// ask ids built over it replay from cache.
QStringList Refinery::termLines(QString const &term, int cap)
{
	QStringList out;
	QRegularExpression const re = termMatcher(term);
	for (refinery_source const &it : m_sources) {
		QString const srt = it.srt;
		if (srt.isEmpty())
			continue;
		for (QString const &line :
		     exporter::load(m_transcripts, srt).lines) {
			if (!re.match(line).hasMatch())
				continue;
			out << line;
			if (out.size() >= cap)
				return out;
		}
	}
	return out;
}

// The three-vote spelling verdict for one nominated pair, from
// cache: +1 when two votes say SAME, -1 when two say DIFFERENT,
// 0 while votes are missing -- and the missing ones are offered,
// idempotently, so the verdict completes as replies land.  Vote
// ids hash the evidence body alone (term strings and transcript
// lines), never directory state, so a refolded directory replays
// the same cached votes.
int Refinery::spellVerdict(QString const &a, QString const &b)
{
	QString const la = termLines(a, 4).join(QLatin1Char('\n'));
	QStringList raw = termLines(b, 4);
	QString const lb = raw.join(QLatin1Char('\n'));
	QRegularExpression const rb = termMatcher(b);
	for (QString &l : raw)
		l.replace(rb, a);
	QString const ls = raw.join(QLatin1Char('\n'));
	int same = 0, diff = 0;
	for (int v = 0; v < 3; ++v) {
		QString const body = QStringLiteral(
			"TERM A (established): %1\n%2\n\n"
			"TERM B (rare, suspect): %3\n%4\n\n"
			"B's lines with A substituted in B's place:\n%5\n\n"
			"Do the substituted lines read as natural speech "
			"about A? Is B the speaker saying A? (pass %6)")
			.arg(a, la, b, lb, ls).arg(v);
		QCryptographicHash h(QCryptographicHash::Blake2b_256);
		h.addData(QByteArrayView("spell\n"));
		h.addData(body.toUtf8());
		agenda::id const vote = takeId(h);
		int const before = same + diff;
		tallySpellVote(vote, same, diff);
		if (same + diff == before) {
			agenda::task w;
			w.id = vote;
			w.note = (a + QStringLiteral(" ~ ") + b
			          + QStringLiteral(" #")
			          + QString::number(v)).toStdString();
			w.what = agenda::kind::spell;
			w.exported = false;
			m_facts.offer(std::move(w), body.toStdString());
		}
	}
	return same >= 2 ? 1 : diff >= 2 ? -1 : 0;
}

// One vote file: the last SAME/DIFFERENT word decides it; an
// unanswered or wordless reply counts for neither side.
void Refinery::tallySpellVote(agenda::id vote, int &same, int &diff)
{
	std::string const p = m_facts.locate(vote,
	                                     agenda::kind::spell);
	if (p.empty())
		return;
	QFile f(QString::fromStdString(p));
	if (!f.open(QIODevice::ReadOnly))
		return;
	QString const text = QString::fromUtf8(f.readAll());
	static QRegularExpression const word(
		QStringLiteral("\\b(SAME|DIFFERENT)\\b"));
	QString last;
	for (auto it = word.globalMatch(text); it.hasNext();)
		last = it.next().captured(1);
	same += last == QStringLiteral("SAME");
	diff += last == QStringLiteral("DIFFERENT");
}

// The projection fold: the term directory rebuilt from scratch as
// a pure function of (the current cut's staged windows, the cached
// artifacts, the non-term topic set).  Names, first-nonempty
// fields and merge outcomes stop depending on arrival order --
// and a stale generation's replies exclude themselves, because
// the fold reads facts only for the windows this cut staged.
void Refinery::refold()
{
	// Facts for whatever has landed; missing replies stay
	// unanswered and simply leave gaps this fold.
	bool changed = false;
	for (TermsWork const &w : m_termsWork)
		changed |= factOf(w);
	for (FocusPair const &p : m_focusOrder)
		changed |= focusFactOf(p.id);
	// The outgoing directory's dive patterns, for the sync diff:
	// terms exported, focuses supportive.
	std::map<std::string, bool> oldPats;
	for (std::string const &n : m_termTopics)
		oldPats.emplace(expandOf(n), true);
	std::set<std::string> oldFocus;
	for (std::string const &n : m_generated)
		if (topics::stem_name(n, "focus")) {
			oldFocus.insert(n);
			oldPats.emplace(expandOf(n), false);
		}
	// Tear down BOTH machine families at once -- a fold that
	// erased one at a time would subtract each against the
	// other's stale generation.  A referenced topic is
	// load-bearing and stays, exactly dropTopic's guard; adhocN
	// stays too: a committed search is a user action, and user
	// actions are base-plane input in the order they happened.
	std::set<std::string> keep;
	for (topics::topic const *r : topics::components(m_corpus))
		keep.insert(r->name);
	std::erase_if(m_corpus.topics, [&](topics::topic const &tp) {
		return (m_termTopics.contains(tp.name)
		        || oldFocus.contains(tp.name))
		    && !keep.contains(tp.name);
	});
	// A kept topic keeps its ROLE too: the replay finds it already
	// covering its pattern and extends instead of re-minting, so
	// nothing downstream would ever re-register it -- and a termN
	// suddenly counted as a hand topic would have its dive retired
	// by the sync below, permanently.
	for (std::string const &n : m_termTopics)
		if (!keep.contains(n))
			m_generated.erase(n);
	for (std::string const &n : oldFocus)
		if (!keep.contains(n))
			m_generated.erase(n);
	m_harvested.clear();
	std::erase_if(m_termTopics, [&](std::string const &n) {
		return !keep.contains(n);
	});
	m_termInfo.clear();
	m_termIndex.clear();
	// Replay in staging order: the one order every session
	// shares.
	for (TermsWork const &w : m_termsWork) {
		auto const it = m_termFacts.constFind(
			QString::fromStdString(w.id.hex()));
		if (it == m_termFacts.constEnd())
			continue;
		for (TermEntry const &e : it->entries)
			adoptEntry(e);
	}
	// The merge/spell fixpoint, cache-hits only: each link's ask
	// id hashes the sorted term titles -- never termN names -- so
	// a deterministically folded directory replays the same chain.
	// The missing link is offered and the fold stops there; the
	// landing pokes the next pass.
	agenda::id prev{};
	for (int guard = 0; guard < 32; ++guard) {
		QStringList terms;
		for (TermInfo const &i : m_termInfo)
			if (!i.term.isEmpty())
				terms << i.term;
		if (terms.size() < 2)
			break;
		// Total order: equal-folded terms tiebreak on the exact
		// string, or the id would drift between sessions.
		std::ranges::sort(terms,
			[](QString const &a, QString const &b) {
				QString const fa = a.toCaseFolded();
				QString const fb = b.toCaseFolded();
				return fa != fb ? fa < fb : a < b;
			});
		QByteArray text;
		for (QString const &s : terms) {
			text += s.toUtf8();
			text += '\n';
		}
		QCryptographicHash h(QCryptographicHash::Blake2b_256);
		h.addData(QByteArrayView("merge\n"));
		h.addData(text);
		agenda::id const id = takeId(h);
		if (id == prev)
			break;          // applied, nothing folded: stable
		prev = id;
		std::string const path = m_facts.locate(
			id, agenda::kind::merge);
		if (path.empty()) {
			agenda::task ask;
			ask.id = id;
			ask.note = std::to_string(terms.size())
			         + " terms";
			ask.what = agenda::kind::merge;
			ask.exported = false;
			m_facts.offer(std::move(ask),
			              text.toStdString());
			break;
		}
		QFile f(QString::fromStdString(path));
		if (!f.open(QIODevice::ReadOnly))
			break;
		QString const reply = QString::fromUtf8(f.readAll());
		if (reply.startsWith(QStringLiteral("NONE")))
			break;
		QHash<QString, QString> stagedSet;
		for (QString const &s : terms)
			stagedSet.insert(s.toCaseFolded(), s);
		for (QString const &l :
		     reply.split(QLatin1Char('\n')))
			foldLine(l, stagedSet);
	}
	// The focus fold: adoption in the deterministic pair order the
	// staged-scan sequence produced, against the freshly folded
	// term directory -- adopt_novel() stays the gate, so branches
	// the corpus already covers still adopt nothing.
	for (FocusPair const &p : m_focusOrder) {
		auto const it = m_focusFacts.constFind(
			QString::fromStdString(p.id.hex()));
		if (it == m_focusFacts.constEnd())
			continue;
		m_harvested.insert(p.id.hex());
		if (it->isEmpty())
			continue;
		std::string const kept = topics::adopt_novel(
			m_corpus, it->toStdString(), "focus");
		if (!kept.empty())
			m_generated.insert(m_corpus.topics.back().name);
	}
	diveSync(oldPats);
	if (changed)
		dirHash();
}

// One judgment line against the staged set of the directory that
// asked it.  MERGE lines are nominations: each member must survive
// its three-vote spelling verdict before it folds.  DROP removes
// the named everyday-vocabulary term wholesale.
void Refinery::foldLine(QString const &line,
                        QHash<QString, QString> const &stagedSet)
{
	auto const staged = [&stagedSet](QString const &s) {
		return stagedSet.contains(s.toCaseFolded());
	};
	if (line.startsWith(QStringLiteral("DROP:"))) {
		QString const s = line.mid(5).trimmed();
		if (!staged(s)) {
			dbgHop(QStringLiteral(
				"terms: judgment rejected [%1]").arg(s));
			return;
		}
		QString const name = m_termIndex.value(s.toCaseFolded());
		if (name.isEmpty())
			return;
		dropTopic(name);
		dbgHop(QStringLiteral("terms: dropped %1 [%2]")
		       .arg(name, s));
		return;
	}
	if (!line.startsWith(QStringLiteral("MERGE:")))
		return;
	QStringList parts;
	for (QString const &p : line.mid(6).split(QLatin1Char('|')))
		if (QString const s = p.trimmed(); !s.isEmpty())
			parts << s;
	if (parts.size() < 2)
		return;
	for (QString const &p : parts)
		if (!staged(p)) {
			dbgHop(QStringLiteral(
				"terms: judgment rejected [%1]").arg(p));
			return;
		}
	QString anchor;
	for (QString const &p : parts) {
		if (!m_termIndex.value(p.toCaseFolded()).isEmpty()) {
			anchor = p;
			break;
		}
	}
	if (anchor.isEmpty())
		return;
	QString const title =
		stagedSet.value(parts.front().toCaseFolded());
	for (QString const &p : parts) {
		if (p.toCaseFolded() == anchor.toCaseFolded())
			continue;
		if (spellVerdict(anchor, p) != 1)
			continue;
		QString const owner =
			m_termIndex.value(anchor.toCaseFolded());
		if (!owner.isEmpty() && mergeSpelling(owner, p)) {
			dbgHop(QStringLiteral("terms: sounded %1 <- %2")
			       .arg(anchor, p));
			if (!title.isEmpty())
				m_termInfo[owner].term = title;
		}
	}
}

// Dives derive from the FINAL directory, once per fold: patterns
// the fold produced stage, patterns it dissolved retire -- and no
// intermediate pattern ever touches the retired set, which is what
// used to poison later folds.
void Refinery::diveSync(std::map<std::string, bool> const &oldPats)
{
	// exported terms, supportive focuses -- the flags each family
	// has always carried.
	std::map<std::string, bool> newPats;
	for (std::string const &n : m_termTopics)
		newPats.emplace(expandOf(n), true);
	for (std::string const &n : m_generated)
		if (topics::stem_name(n, "focus"))
			newPats.emplace(expandOf(n), false);
	for (auto const &[p, x] : oldPats)
		if (!newPats.contains(p) && !p.empty())
			retireDive(diveId(p));
	for (auto const &[p, x] : newPats)
		if (!oldPats.contains(p) && !p.empty())
			stageDive(p, x, !x);
}

// The fold's acceptance instrument: one line naming the directory
// deterministically, so two sessions -- or two ask orders -- can
// be diffed mechanically.
void Refinery::dirHash()
{
	QStringList rows;
	for (std::string const &n : m_termTopics) {
		TermInfo const info =
			m_termInfo.value(QString::fromStdString(n));
		rows << QString::fromStdString(n) + QLatin1Char('\t')
			+ info.term + QLatin1Char('\t') + info.kind
			+ QLatin1Char('\t') + info.gloss
			+ QLatin1Char('\t')
			+ QString::fromStdString(expandOf(n));
	}
	for (std::string const &n : m_generated)
		if (topics::stem_name(n, "focus"))
			rows << QString::fromStdString(n)
				+ QLatin1Char('\t')
				+ QString::fromStdString(expandOf(n));
	rows.sort();
	QCryptographicHash h(QCryptographicHash::Blake2b_256);
	h.addData(QByteArrayView("dir\n"));
	h.addData(rows.join(QLatin1Char('\n')).toUtf8());
	dbgHop(QStringLiteral("refold: %1 topics, dir %2")
	       .arg(rows.size())
	       .arg(QString::fromLatin1(
		h.result().left(8).toHex())));
}

// Remove one machine topic wholesale: corpus entry, directory
// entry, every index spelling, its dive.  Cached extraction
// replies re-mint it next session and the cached judgment drops it
// again -- deterministic and invisible.
void Refinery::dropTopic(QString const &name)
{
	std::string const victim = name.toStdString();
	std::string const pat = expandOf(victim);
	if (pat.empty())
		return;
	// A topic other topics reference is load-bearing structure:
	// erasing it would dangle their fragments.  Never a victim.
	for (topics::topic const *r : topics::components(m_corpus))
		if (r->name == victim)
			return;
	std::erase_if(m_corpus.topics,
		[&victim](topics::topic const &tp) {
			return tp.name == victim;
		});
	m_termTopics.erase(victim);
	m_generated.erase(victim);
	m_termInfo.remove(name);
	m_termIndex.removeIf([&name](auto it) {
		return it.value() == name;
	});
	// Dives re-derive from the final directory in diveSync().
}

// Fold the topic owning one spelling into the group owner: its
// branches join the owner's alternation, the twin topic leaves the
// corpus, and every index entry follows.  True when the spelling
// ends up belonging to the owner; false when the fold refused.
bool Refinery::mergeSpelling(QString const &owner,
                            QString const &spell)
{
	QString const k = spell.toCaseFolded();
	QString const name = m_termIndex.value(k);
	if (name.isEmpty()) {
		m_termIndex.insert(k, owner);
		return true;
	}
	if (name == owner)
		return true;
	std::string const victim = name.toStdString();
	std::string const vpat = expandOf(victim);
	std::string const opat = expandOf(owner.toStdString());
	if (vpat.empty() || opat.empty())
		return false;
	// Referenced topics are structure, not spellings: folding one
	// away would dangle the fragments that name it.
	for (topics::topic const *r : topics::components(m_corpus))
		if (r->name == victim)
			return false;
	// The victim is erased before the extend so it cannot cover
	// its own branches -- which makes a refused extend a silent
	// loss.  Ask first.
	if (!topics::extendable(m_corpus, owner.toStdString(), vpat))
		return false;
	// The victim leaves the corpus BEFORE the extend subtracts,
	// or it would cover its own branches and refuse the fold.
	std::erase_if(m_corpus.topics,
		[&victim](topics::topic const &tp) {
			return tp.name == victim;
		});
	std::string const grown = topics::extend(
		m_corpus, owner.toStdString(), vpat);
	m_termTopics.erase(victim);
	m_generated.erase(victim);
	TermInfo const gone = m_termInfo.take(name);
	TermInfo &info = m_termInfo[owner];
	if (info.kind.isEmpty())
		info.kind = gone.kind;
	if (info.gloss.isEmpty())
		info.gloss = gone.gloss;
	for (QString &v : m_termIndex)
		if (v == name)
			v = owner;
	// Dives re-derive from the final directory in diveSync().
	dbgHop(QStringLiteral("terms: merged %1 <- %2 [%3]")
	       .arg(owner, name, spell));
	return true;
}

// The term directory as the engine's lexicon: every spelling the
// terms pass proposed and the harvest saw on a cited line, grouped
// by the term topic that owns it.  Pushed whenever the index has
// grown; the names one group holds are one entity on the model's
// word, which is how GIDRA meets Ghidra.
void Refinery::feedLexicon()
{
	// The groups as the index spells them now, in one order
	// whatever order the hash walks them in, compared whole with
	// what the engine has: the index changes by insert, by removal,
	// and by a merge that re-owns spellings without a count moving.
	QHash<QString, std::size_t> groupOf;
	std::vector<std::vector<std::string>> groups;
	for (auto it = m_termIndex.cbegin(); it != m_termIndex.cend(); ++it) {
		std::size_t const g = groupOf.value(it.value(), groups.size());
		if (g == groups.size()) {
			groupOf.insert(it.value(), g);
			groups.emplace_back();
		}
		groups[g].push_back(it.key().toStdString());
	}
	for (std::vector<std::string> &group : groups)
		std::ranges::sort(group);
	std::ranges::sort(groups);
	if (groups == m_lexicon)
		return;
	m_lexicon = groups;
	m_semantic.lexicon(std::move(groups));
}

std::size_t Refinery::focusWorkOf(agenda::id id) const
{
	for (std::size_t i = 0; i < m_focusWork.size(); ++i)
		if (m_focusWork[i].focus == id)
			return i;
	return m_focusWork.size();
}

// Pane progress for one topic's dive scan: complete behind the
// cursor, mid-flight at it, unstarted past it; a cleared list
// means they all finished.
int Refinery::scanned(agenda::id dive, int videos) const
{
	for (std::size_t k = 0; k < m_diveScans.size(); ++k) {
		if (m_diveScans[k].id != dive)
			continue;
		return k < m_diveAt ? videos
		     : k == m_diveAt ? int(m_diveScans[k].video) : 0;
	}
	return videos;
}

// The outgoing generation's ask ids, gathered before the engine
// resets: whatever the new cut does not restage retires below.
std::set<agenda::id> Refinery::preCut()
{
	std::set<agenda::id> stale;
	for (std::size_t i = 0; i < m_semantic.windows(); ++i)
		stale.insert(m_semantic.key("semantic-extract-v1",
		                            m_semantic.window(i)));
	for (TermsWork const &w : m_termsWork)
		stale.insert(w.id);
	return stale;
}

// After the reset and the witness mark.  Each cut retires the last
// cut's term work: windows re-keyed by arriving frames leave their
// old ids behind, and a stale pre-frames reply harvesting late
// would adopt guessed spellings first -- the first-nonempty
// TermInfo fields would then pin them over the frame-anchored
// ones.  queueTerms() restages the current windows from scratch
// (offers dedupe against plan and cache, m_termsSeen persists),
// the surviving ids subtract -- identical windows carry identical
// ids, and parking one would block its own re-offer -- and only
// the truly abandoned asks retire.  The kicked dispatch then
// adopts, feeds the lexicon and paints, all against this cut.
void Refinery::postCut(std::set<agenda::id> stale)
{
	m_lexicon.clear();
	m_termsWork.clear();
	queueTerms();
	for (std::size_t i = 0; i < m_semantic.windows(); ++i)
		stale.erase(m_semantic.key("semantic-extract-v1",
		                           m_semantic.window(i)));
	for (TermsWork const &w : m_termsWork)
		stale.erase(w.id);
	m_facts.retire({stale.begin(), stale.end()});
	harvestChain(true);
}
