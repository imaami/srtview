// srt.hpp -- SRT cue model, parser, and display formatting.
//
// SubRip has no official specification; the format is defined by
// common practice.  The rules implemented here follow the de-facto
// references:
//   - Subtitle Edit format reference
//     https://subtitleedit.github.io/subtitleedit/reference/subrip.html
//   - Library of Congress format description fdd000569
//   - Wikipedia "SubRip" / Matroska subtitle notes
#pragma once

#include <QString>

#include <string_view>
#include <vector>

struct Cue {
	double  start = 0.0;
	double  end   = 0.0;
	QString text;          // internal line breaks as QChar::LineSeparator
};

// Parse SRT structure from UTF-8 bytes: a single-pass block state
// machine, no regular expressions, no dynamic dispatch; QString only
// appears at the per-cue output boundary.  See srt.cpp for the
// rule-by-rule mapping onto the references.
std::vector<Cue> parseSrtData(std::string_view utf8);

// File wrapper: reads the file, normalizes the encoding to UTF-8
// (UTF-8 with/without BOM, UTF-16 LE/BE by BOM, Windows-1252 as the
// documented legacy "ANSI" fallback for invalid UTF-8), then parses.
std::vector<Cue> parseSrt(const QString &path, QString *err);

// Escape a cue text for QTextDocument, letting the SRT inline-tag
// subset back through: <i> <b> <u> and <font> with color/face/size
// attributes, per the reference; {\...} ASS override blocks, which
// carry positioning irrelevant to a transcript view, are dropped.
// Hand-rolled scanner, no regex.
QString cueHtml(const QString &text);

// "m:ss", "h:mm:ss", optionally with ".mmm".  Feeds Qt paint/tooltip
// APIs directly, hence QString rather than std::format.
QString fmtTime(double t, bool withMs);
