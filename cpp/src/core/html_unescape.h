#pragma once
#include <QString>

namespace jietuba {
// Decodes named (HTML5) and numeric character references, including the
// longest-prefix match for names without a semicolon.
QString html_unescape(const QString &text);
}
