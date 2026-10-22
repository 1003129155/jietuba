#pragma once
#include <QString>

namespace jietuba {
// The first count characters (code points) of text; a surrogate pair is never split.
QString left_chars(const QString &text, qsizetype count);
}
