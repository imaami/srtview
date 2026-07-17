// concepts.hpp -- the contracts components require of their hosts.
// Each component is a thin header-only template constrained by one of
// these; controllers satisfy them.  Components never name a concrete
// host type.
#ifndef SRTVIEW_SRC_CONCEPTS_HPP_
#define SRTVIEW_SRC_CONCEPTS_HPP_

template <typename T>
concept playback_host = requires(T &h, int cue, bool b, double d) {
	h.seekCue(cue, b);
	h.setPause(b);
	h.togglePause();
	h.seekRel(d);
	h.toggleFollow();
};

template <typename T>
concept search_host = requires(T &h, bool b) {
	h.showSearch();
	h.hideSearch();
	h.commitSearch();
	h.searchChanged();
	h.findAgain(b);
};

template <typename T>
concept mpv_observer = requires(T &h, double t, bool b) {
	h.onMpvTime(t);
	h.onMpvState(b);
};

#endif // SRTVIEW_SRC_CONCEPTS_HPP_
