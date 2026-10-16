#pragma once
#include "ocr/ocr_manager.h"
#include <QString>
#include <QStringList>
#include <QVector>

// Lays recognized lines out as text: joins wrapped lines, removes spaces, converts
// punctuation width.
//
// Recognition carries no paragraph marks, but the two halves of a wrapped line have
// a stable shape: the upper reaches the right edge, the lower starts at the left
// edge, they are close, and the upper does not end a sentence. Lines are joined only
// when all four hold; a missed join is visible at a glance, a wrong one is not.
//
// Pure functions over ocr::group_ocr_lines' lines (text plus bounding box).
namespace jietuba::text_layout {
inline const QString MODE_AUTO = "auto";
inline const QString MODE_SINGLE_LINE = "single_line";
inline const QString MODE_MULTILINE = "multiline";
inline const QStringList LAYOUT_MODES = {MODE_AUTO, MODE_SINGLE_LINE, MODE_MULTILINE};

inline const QString PUNCT_KEEP = "keep";
inline const QString PUNCT_HALFWIDTH = "halfwidth";
inline const QString PUNCT_FULLWIDTH = "fullwidth";
inline const QStringList PUNCTUATION_MODES = {PUNCT_KEEP, PUNCT_HALFWIDTH, PUNCT_FULLWIDTH};

// Edge tolerance in line heights: the larger the font, the more ragged a paragraph's edges.
constexpr double EDGE_TOLERANCE = 0.5;
// A gap above this many line heights separates paragraphs.
constexpr double GAP_LIMIT = 1.2;

// The lines as one text. mode is one of LAYOUT_MODES, punctuation one of PUNCTUATION_MODES.
QString render(const QVector<OcrTextLine> &lines, const QString &mode = MODE_AUTO, bool remove_spaces = false, const QString &punctuation = PUNCT_KEEP);
}
