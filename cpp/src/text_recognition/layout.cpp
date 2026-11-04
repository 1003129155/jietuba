#include "text_recognition/layout.h"
#include <QChar>
#include <QHash>
#include <algorithm>

namespace jietuba::text_layout {
namespace {
using CodePoints = std::u32string;

// A line ending in one of these is a paragraph of its own. The Western full stop is
// also a decimal point and abbreviation mark, so only line ends count.
const CodePoints TERMINATORS = U"。．.！!？?；;…";

const QHash<char32_t, char32_t> &to_halfwidth() {
    static const QHash<char32_t, char32_t> table = {
        {U'，', U','}, {U'。', U'.'}, {U'！', U'!'}, {U'？', U'?'}, {U'；', U';'}, {U'：', U':'},
        {U'、', U','}, {U'（', U'('}, {U'）', U')'}, {U'【', U'['}, {U'】', U']'}, {U'《', U'<'},
        {U'》', U'>'}, {U'“', U'"'}, {U'”', U'"'}, {U'‘', U'\''}, {U'’', U'\''}, {U'　', U' '},
    };
    return table;
}

// Full-width quotes have opening and closing forms the character alone cannot
// decide, so quotes are not converted back.
const QHash<char32_t, char32_t> &to_fullwidth() {
    static const QHash<char32_t, char32_t> table = {
        {U',', U'，'}, {U'.', U'。'}, {U'!', U'！'}, {U'?', U'？'}, {U';', U'；'},
        {U':', U'：'}, {U'(', U'（'}, {U')', U'）'}, {U'[', U'【'}, {U']', U'】'},
    };
    return table;
}

CodePoints to_code_points(const QString &text) {
    const auto ucs = text.toUcs4();
    return CodePoints(ucs.begin(), ucs.end());
}

// Text with where each character came from, kept in step through every layout step.
struct Tagged {
    CodePoints text;
    std::vector<OcrCharSource> sources;
    bool empty() const { return text.empty(); }
    void push(char32_t c, OcrCharSource source = {}) {
        text.push_back(c);
        sources.push_back(source);
    }
    void append(const Tagged &other, size_t from = 0, size_t count = CodePoints::npos) {
        from = std::min(from, other.text.size());
        const size_t n = std::min(count, other.text.size() - from);
        text.append(other.text, from, n);
        sources.insert(sources.end(), other.sources.begin() + from, other.sources.begin() + from + n);
    }
};

Tagged tagged(const OcrTextLine &line) {
    Tagged out{to_code_points(line.text), {}};
    if (line.sources.size() == static_cast<qsizetype>(out.text.size())) out.sources.assign(line.sources.begin(), line.sources.end());
    else out.sources.assign(out.text.size(), OcrCharSource{});
    return out;
}

bool is_cjk(char32_t c) {
    return (c >= 0x3000 && c <= 0x303f) || (c >= 0x3040 && c <= 0x30ff) || (c >= 0x3400 && c <= 0x4dbf) || (c >= 0x4e00 && c <= 0x9fff) ||
           (c >= 0xac00 && c <= 0xd7af) || (c >= 0xf900 && c <= 0xfaff) || (c >= 0xff00 && c <= 0xffef);
}

// str.isalpha for one character: Unicode letter categories.
bool is_alpha(char32_t c) { return QChar::isLetter(c); }

// str.isspace for one character, minus line breaks: inline whitespace.
bool is_inline_space(char32_t c) {
    if (c == U'\r' || c == U'\n') return false;
    switch (c) {
    case 0x09: case 0x0b: case 0x0c: case 0x1c: case 0x1d: case 0x1e: case 0x1f: case 0x20: case 0x85: case 0xa0: case 0x1680:
    case 0x2028: case 0x2029: case 0x202f: case 0x205f: case 0x3000: return true;
    default: return c >= 0x2000 && c <= 0x200a;
    }
}

// Removes only spaces next to CJK text: recognition often inserts spaces between
// ideographs, which is noise, while spaces between Western words are content.
Tagged strip_spaces(const Tagged &tagged) {
    const auto &text = tagged.text;
    Tagged result;
    size_t i = 0;
    while (i < text.size()) {
        if (!is_inline_space(text[i])) {
            result.push(text[i], tagged.sources[i]);
            ++i;
            continue;
        }
        size_t end = i;
        while (end < text.size() && is_inline_space(text[end])) ++end;
        const bool cjk_before = i > 0 && is_cjk(text[i - 1]);
        const bool cjk_after = end < text.size() && is_cjk(text[end]);
        if (!cjk_before && !cjk_after) result.append(tagged, i, end - i);
        i = end;
    }
    return result;
}

// Joins the halves of a wrapped line: CJK directly, Western with a space, and a
// hyphen splitting a word is dropped.
Tagged merge(Tagged left, const Tagged &right) {
    if (left.empty() || right.empty()) return left.empty() ? right : left;
    const auto &l = left.text;
    if (l.back() == U'-' && l.size() >= 2 && is_alpha(l[l.size() - 2]) && is_alpha(right.text.front())) {
        left.text.pop_back();
        left.sources.pop_back();
    } else if (!is_cjk(l.back()) && !is_cjk(right.text.front())) {
        left.push(U' ');
    }
    left.append(right);
    return left;
}

struct Metrics {
    double height, left, right;
};

// The block's left and right edges and median line height. The median is steadier
// than the mean: headings and footnotes are outliers.
Metrics metrics(const QVector<OcrTextLine> &lines) {
    std::vector<double> heights;
    for (const auto &line : lines) heights.push_back(line.bottom - line.top);
    std::sort(heights.begin(), heights.end());
    double height = heights[heights.size() / 2];
    if (height == 0) height = 1.0;
    double left = lines.front().left, right = lines.front().right;
    for (const auto &line : lines) {
        left = std::min(left, line.left);
        right = std::max(right, line.right);
    }
    return {height, left, right};
}

// Whether current is the wrapped lower half of previous.
bool is_wrapped(const OcrTextLine &previous, const OcrTextLine &current, const Metrics &m) {
    const double tolerance = m.height * EDGE_TOLERANCE;
    if (previous.right < m.right - tolerance) return false;  // short of the right edge: a paragraph's last line
    if (current.left > m.left + tolerance) return false;     // indented: a new paragraph
    if (current.top - previous.bottom > m.height * GAP_LIMIT) return false;  // widened line spacing
    const auto text = previous.text.toUcs4();
    return !text.isEmpty() && TERMINATORS.find(static_cast<char32_t>(text.back())) == CodePoints::npos;
}

Tagged layout(const QVector<OcrTextLine> &lines, const QString &mode) {
    if (lines.isEmpty()) return {};
    if (mode == MODE_MULTILINE) {
        Tagged text;
        for (int i = 0; i < lines.size(); ++i) {
            if (i) text.push(U'\n');
            text.append(tagged(lines[i]));
        }
        return text;
    }
    auto text = tagged(lines.front());
    if (mode == MODE_SINGLE_LINE) {
        for (int i = 1; i < lines.size(); ++i) text = merge(std::move(text), tagged(lines[i]));
        return text;
    }
    const auto m = metrics(lines);
    for (int i = 1; i < lines.size(); ++i) {
        if (is_wrapped(lines[i - 1], lines[i], m)) {
            text = merge(std::move(text), tagged(lines[i]));
        } else {
            text.push(U'\n');
            text.append(tagged(lines[i]));
        }
    }
    return text;
}

// Converts only punctuation next to CJK text: decimal points, Western brackets and
// abbreviation dots stay.
CodePoints convert_punctuation(const CodePoints &text, const QString &punctuation) {
    CodePoints result = text;
    if (punctuation == PUNCT_HALFWIDTH) {
        for (auto &c : result) c = to_halfwidth().value(c, c);
    } else if (punctuation == PUNCT_FULLWIDTH) {
        for (size_t i = 0; i < text.size(); ++i) {
            const auto full = to_fullwidth().constFind(text[i]);
            if (full == to_fullwidth().constEnd()) continue;
            const bool before = i > 0 && is_cjk(text[i - 1]);
            const bool after = i + 1 < text.size() && is_cjk(text[i + 1]);
            if (before || after) result[i] = *full;
        }
    }
    return result;
}
}

Rendered render_with_sources(const QVector<OcrTextLine> &lines, const QString &mode, bool remove_spaces, const QString &punctuation) {
    auto text = layout(lines, mode);
    if (remove_spaces) text = strip_spaces(text);
    const auto converted = convert_punctuation(text.text, punctuation);
    Rendered out;
    for (size_t i = 0; i < converted.size(); ++i) {
        const char32_t c = converted[i];
        const QString piece = QString::fromUcs4(&c, 1);
        out.text += piece;
        // A character outside the BMP is two UTF-16 units; both point at it.
        for (int k = 0; k < piece.size(); ++k) out.sources.append(text.sources[i]);
    }
    return out;
}

QString render(const QVector<OcrTextLine> &lines, const QString &mode, bool remove_spaces, const QString &punctuation) {
    return render_with_sources(lines, mode, remove_spaces, punctuation).text;
}
}
