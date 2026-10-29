#pragma once
#include <QList>
#include <QString>

namespace jietuba {
// Where words begin and end in text, as UTF-16 positions from 0 to its length: what a double
// click selects. With the system's ICU, Chinese and Japanese split into dictionary words;
// without it, as punctuation_word_boundaries.
QList<int> word_boundaries(const QString &text);
// Spaces and punctuation, half or full width, separate words and are pieces of their own;
// letters, digits and Chinese characters running between them are one word, so Chinese runs up
// to its punctuation and mixed text is not cut where the script changes.
QList<int> punctuation_word_boundaries(const QString &text);
}
