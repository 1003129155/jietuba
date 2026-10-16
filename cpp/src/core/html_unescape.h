#pragma once
#include <QString>

namespace jietuba {
// html.unescape: named (HTML5) and numeric character references, including the
// longest-prefix match for names without a semicolon.
QString html_unescape(const QString &text);
}
