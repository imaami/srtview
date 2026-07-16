// srt.hpp -- SRT cue model, parser, and display formatting.
#pragma once

#include <QString>
#include <vector>

struct Cue {
	double  start = 0.0;
	double  end   = 0.0;
	QString text;          // internal line breaks as QChar::LineSeparator
};

// Native line-oriented parser: no regular expressions, no dynamic
// dispatch; QString only appears at the per-cue output boundary.
// Accepts UTF-8 (with or without BOM), UTF-16 LE/BE (BOM), CRLF/LF,
// ',' or '.' millisecond separators, and missing cue index lines.
std::vector<Cue> parseSrt(const QString &path, QString *err);

// Escape a cue text for QTextDocument, letting the SRT inline-tag
// subset (<i> <b> <u> <font color=...>) back through and dropping
// {\...} ASS override blocks.  Hand-rolled scanner, no regex.
QString cueHtml(const QString &text);

// "m:ss", "h:mm:ss", optionally with ".mmm".  Feeds Qt paint/tooltip
// APIs directly, hence QString rather than std::format.
QString fmtTime(double t, bool withMs);
