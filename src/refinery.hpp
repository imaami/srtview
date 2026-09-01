// refinery.hpp -- the semantic refinery: every machine that turns
// landed model artifacts into corpus adoptions -- term windows,
// spelling votes, the directory fold, topic dives and the
// probe/focus chains -- plus the completion-poked dispatch that
// drives them.  A controller in the PlaybackCtl mold: it owns its
// machines' state, borrows the pipeline (Facts), the engine, the
// corpus document and the transcript cache from the composition
// root, and reaches the UI only through refinery_host -- one
// "adoption is held" question and one "something changed" report.
// Everything runs on the owning (UI) thread; the Facts poke is
// marshaled here as one queued call per landing burst, so no
// polling timer touches these machines at all.
#ifndef SRTVIEW_SRC_REFINERY_HPP_
#define SRTVIEW_SRC_REFINERY_HPP_

#include <QHash>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QTimer>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "agenda.hpp"
#include "exporter.hpp"
#include "facts.hpp"
#include "semantic_engine.hpp"
#include "topics.hpp"

// What the refinery needs to know of the UI, and nothing more.
struct refinery_host {
	// Something the panes show has changed; called at most once
	// per dispatch pass.
	virtual void refineryChanged() = 0;

protected:
	~refinery_host() = default;
};

// One playlist entry as the refinery sees it: resolved paths, the
// video's content identity, and the subtitle leaf id the facts
// pipeline keys summaries under.  The owner rebuilds the list at
// every corpus load.
struct refinery_source {
	QString    video, srt, id;
	agenda::id leaf;
};

class Refinery
{
public:
	Refinery(Facts &facts, engine::SemanticEngine<Facts> &semantic,
	         topics::doc &corpus, exporter::transcripts &transcripts,
	         refinery_host *host);

	// Detaches the Facts poke under its mutex: no completion can
	// hold a pointer to a dead refinery, however teardown and the
	// model worker interleave.
	~Refinery();

	Refinery(Refinery const &) = delete;
	Refinery &operator=(Refinery const &) = delete;

	// A new corpus: fresh drops every machine's state; a re-adopt
	// keeps adoption history and reseeds only the name stems.
	void reset(bool fresh);
	void seedGenerated();
	void setSources(QList<refinery_source> sources);
	// The pyramid root of the current corpus: dives reference it
	// as background context when it exists.
	void setRoot(agenda::id root) { m_rootId = root; }

	// Corpus topic dives; searchCommitted stages one ad hoc.
	void queueDives(bool fresh);
	void stageDive(std::string const &pattern, bool exported,
	               bool generated);

	// The cut seam: preCut() collects the outgoing generation's
	// ask ids before the engine resets; postCut() -- after the
	// reset and the witness mark -- retires what the new cut did
	// not restage, restages term windows, and runs the dispatch.
	std::set<agenda::id> preCut();
	void postCut(std::set<agenda::id> stale);

	// The completion-poked dispatch: one pass over every machine,
	// early-out when nothing landed since the last pass unless
	// kicked (a re-cut or a user action changed other inputs).
	void harvestChain(bool kick);

	// One validated entry of a terms reply, and a reply's worth of
	// them: immutable facts keyed by window id, folded -- never
	// accumulated -- into the directory.
	// support is the floor scan's pattern, not its verdict: the
	// verdict counts hits across the current sources, so it is
	// corpus-relative and lives in the fold's memo, never in the
	// window-keyed fact.
	struct TermEntry {
		QString     term, kind, shown, support;
		QStringList kept;
		std::string tidied;
	};
	struct TermFact {
		QList<TermEntry> entries;
	};

	// The pane's read-only views.
	struct TermInfo {
		QString term;
		QString kind;
		QString gloss;
	};
	QHash<QString, TermInfo> const &termInfo() const
	{ return m_termInfo; }
	std::set<std::string> const &termTopics() const
	{ return m_termTopics; }
	std::set<std::string> const &generated() const
	{ return m_generated; }
	std::set<std::string> const &harvested() const
	{ return m_harvested; }
	// Whether a staged window's reply has been parsed into a fact.
	bool answered(std::string const &hex) const
	{ return m_termFacts.contains(QString::fromStdString(hex)); }
	// Per-video terms progress: staged windows and answered ones.
	struct TermsWork {
		agenda::id id;
		QString    video, srt;
		int        first = 0, last = 0;
	};
	std::vector<TermsWork> const &termsWork() const
	{ return m_termsWork; }

	// The spelling matcher the support floor and the pane's match
	// scans share.
	static QRegularExpression termMatcher(QString const &term);

	// Shared identity/parse vocabulary the pane leans on: a dive's
	// id is its expanded pattern's hash, and focus artifacts close
	// with a REGEX: line.
	static agenda::id diveId(std::string const &pattern);
	static std::string regexPayload(std::string const &text,
	                                std::size_t from,
	                                std::size_t end);
	static std::string regexLine(std::string const &text);

	// Pane progress: how many videos a topic's dive scan has
	// covered (all when done), and a pending focus id's position.
	int scanned(agenda::id dive, int videos) const;
	std::size_t focusWorkOf(agenda::id id) const;

private:
	struct DiveScan {
		QRegularExpression      re;
		std::string             pattern;
		std::string             parts;
		std::vector<agenda::id> deps;
		agenda::id              id;
		std::size_t             video     = 0;
		bool                    exported  = true;
		bool                    generated = false;
	};

	struct FinishedDive {
		agenda::id              id;
		std::vector<agenda::id> keys;
		std::string             pattern;
		std::string             parts;
	};

	struct PendingFocus {
		agenda::id              probe;
		agenda::id              retry;
		agenda::id              focus;
		std::vector<agenda::id> deps;
		std::vector<agenda::id> keys;
		std::string             note;
		std::string             raw;
		bool                    scanning = false;
	};

	static void poke(void *self) noexcept;

	void diveStep();
	void scanDiveVideo(DiveScan &s, refinery_source const &it);
	void finishDive(DiveScan &s);
	void pairFocus(DiveScan const &s);
	void stageProbe(FinishedDive const &a, DiveScan const &b);
	agenda::task probeTask(PendingFocus const &w,
	                       agenda::id ask) const;
	void pumpProbes();
	bool pumpProbe(PendingFocus &w);
	bool retryProbe(PendingFocus &w, std::string const &feedback);
	void stageFocusScan(agenda::id id, std::string const &pattern,
	                    QRegularExpression const &re);
	bool finishProbe(DiveScan const &s, PendingFocus &w);
	void queueTerms();
	bool factOf(TermsWork const &w);
	bool focusFactOf(agenda::id id);
	void adoptEntry(TermEntry const &e);
	void refold();
	void foldLine(QString const &line,
	              QHash<QString, QString> const &stagedSet);
	void diveSync(std::map<std::string, bool> const &oldPats);
	void dirHash();
	int spellVerdict(QString const &a, QString const &b);
	int corpusHits(QRegularExpression const &re, int cap);
	bool supportedOf(TermEntry const &e);
	QStringList termLines(QString const &term, int cap);
	std::string expandOf(std::string const &name) const;
	void retireDive(agenda::id id);
	void indexSpellings(QStringList const &seen,
	                    QString const &owner);
	void tallySpellVote(agenda::id vote, int &same, int &diff);
	void dropTopic(QString const &name);
	bool mergeSpelling(QString const &owner, QString const &victim);
	void feedLexicon();

	Facts                          &m_facts;
	engine::SemanticEngine<Facts>  &m_semantic;
	topics::doc                    &m_corpus;
	exporter::transcripts          &m_transcripts;
	refinery_host                  *m_host;
	QList<refinery_source>          m_sources;
	agenda::id                      m_rootId{};
	QTimer                          m_diveTick;
	std::vector<DiveScan>           m_diveScans;
	std::size_t                     m_diveAt = 0;
	std::uint64_t                   m_harvestSwept = 0;
	std::vector<FinishedDive>       m_dives;
	std::set<std::string>           m_diveRetired;
	std::vector<PendingFocus>       m_focusWork;
	// The deterministic pair order the staged-scan sequence
	// produced: the fold adopts focuses in exactly this order.
	struct FocusPair {
		agenda::id id, a, b;
	};
	std::vector<FocusPair>          m_focusOrder;
	QHash<QString, QString>         m_focusFacts;
	std::set<std::string>           m_generated;
	std::set<std::string>           m_harvested;
	std::vector<TermsWork>          m_termsWork;
	QHash<QString, TermFact>        m_termFacts;
	QHash<QString, bool>            m_supportSeen; // per source set
	std::set<std::string>           m_termTopics;
	QHash<QString, TermInfo>        m_termInfo;
	QHash<QString, QString>         m_termIndex;
	std::vector<std::vector<std::string>> m_lexicon;
};

#endif // SRTVIEW_SRC_REFINERY_HPP_
