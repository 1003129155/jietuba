#include "core/text_utils.h"

namespace jietuba {
QString left_chars(const QString &text, qsizetype count) {
    qsizetype end = 0;
    for (qsizetype n = 0; n < count && end < text.size(); ++n)
        end += text[end].isHighSurrogate() && end + 1 < text.size() && text[end + 1].isLowSurrogate() ? 2 : 1;
    return text.left(end);
}
}
