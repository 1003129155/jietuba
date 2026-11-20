#include "translation/line_grouping.h"
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace jietuba {
namespace {
// All distances are fractions of a line height, so one rule holds at every font size and DPI.
// How far above and below its own box a line looks for neighbours, and how far its edges may
// be from theirs.
constexpr double NEIGHBOUR_REACH = 0.5;
// Neighbours differ in height by less than this factor: a heading is set larger than its text.
constexpr double MAX_HEIGHT_RATIO = 1.5;
// An indented first line sits this far right of the lines below it. It must be most of a line
// itself, or it is a label and not the start of a paragraph.
constexpr double INDENT_MIN = 0.8;
constexpr double INDENT_MAX = 3.5;
constexpr double INDENT_GAP_BELOW = -0.2;
constexpr double INDENT_LINE_SHARE = 0.6;
// A paragraph's last line is short and ends on a full stop: what follows is the next paragraph.
constexpr double SHORT_LAST_LINE = 0.85;
// Longest paragraph kept: a run of unrelated lines that happens to line up stays readable.
constexpr int MAX_PARAGRAPH_LINES = 30;

// Whether other is a line self continues into: it overlaps self's span, lies within reach
// above or below it, is about as tall, and is flush with it on the left, on the right or in
// the middle (left for ragged text, right for justified or right-aligned, middle for centred).
bool reaches(const QRectF &self, const QRectF &other) {
    const double reach = NEIGHBOUR_REACH * self.height();
    if (!(other.right() > self.left() && other.left() < self.right() && other.bottom() > self.top() - reach && other.top() < self.bottom() + reach)) return false;
    if (std::abs(other.height() - self.height()) > reach) return false;
    const double larger = std::max(other.height(), self.height()), smaller = std::min(other.height(), self.height());
    if (larger / smaller >= MAX_HEIGHT_RATIO) return false;
    return std::abs(other.left() - self.left()) <= reach || std::abs(other.right() - self.right()) <= reach ||
           std::abs(other.center().x() - self.center().x()) <= reach;
}

// Lines linked by reaches() in either direction are one group; groups come in order of their
// first line.
QVector<QVector<int>> neighbour_groups(const QVector<GroupingLine> &lines) {
    const int count = static_cast<int>(lines.size());
    std::vector<int> parent(static_cast<size_t>(count));
    std::iota(parent.begin(), parent.end(), 0);
    const auto find = [&parent](int index) {
        while (parent[static_cast<size_t>(index)] != index) {
            parent[static_cast<size_t>(index)] = parent[static_cast<size_t>(parent[static_cast<size_t>(index)])];
            index = parent[static_cast<size_t>(index)];
        }
        return index;
    };
    for (int a = 0; a < count; ++a)
        for (int b = 0; b < count; ++b)
            if (a != b && reaches(lines[a].rect, lines[b].rect)) parent[static_cast<size_t>(find(a))] = find(b);
    QVector<QVector<int>> groups;
    std::vector<int> slot(static_cast<size_t>(count), -1);
    for (int index = 0; index < count; ++index) {
        int &place = slot[static_cast<size_t>(find(index))];
        if (place < 0) {
            place = static_cast<int>(groups.size());
            groups.append(QVector<int>());
        }
        groups[place].append(index);
    }
    return groups;
}

int topmost(const QVector<GroupingLine> &lines, const QVector<int> &group) {
    int best = group.first();
    for (const int index : group)
        if (lines[index].rect.top() < lines[best].rect.top()) best = index;
    return best;
}

// A paragraph that starts with an indent is a single line to the left-aligned lines below it,
// which are flush with each other but not with it.
void join_indented_first_lines(const QVector<GroupingLine> &lines, QVector<QVector<int>> &groups) {
    bool merged = true;
    while (merged) {
        merged = false;
        for (int single = 0; single < groups.size(); ++single) {
            if (groups[single].size() != 1) continue;
            const QRectF &first = lines[groups[single].first()].rect;
            const double height = first.height();
            for (int below = 0; below < groups.size(); ++below) {
                if (below == single || groups[below].isEmpty()) continue;
                const QRectF &next = lines[topmost(lines, groups[below])].rect;
                const double gap = next.top() - first.bottom(), indent = first.left() - next.left();
                if (gap >= INDENT_GAP_BELOW * height && gap <= NEIGHBOUR_REACH * height && indent >= INDENT_MIN * height && indent <= INDENT_MAX * height &&
                    first.width() >= INDENT_LINE_SHARE * next.width() && std::abs(next.height() - height) <= NEIGHBOUR_REACH * height) {
                    groups[below] += groups[single];
                    groups[single].clear();
                    merged = true;
                    break;
                }
            }
        }
        groups.erase(std::remove_if(groups.begin(), groups.end(), [](const QVector<int> &group) { return group.isEmpty(); }), groups.end());
    }
}

// "1." "2)" "3、" or a bullet: an item of a list, which is never the continuation of the
// line above however closely it follows.
bool starts_list_item(const QString &text) {
    static const QRegularExpression marker(QStringLiteral("^\\s*(?:\\(?\\d{1,3}[.)\\x{3001}\\x{FF0E}]\\s*\\S|[-*\\x{2022}\\x{00B7}\\x{25CF}\\x{25A0}\\x{25AA}\\x{2013}]\\s+\\S)"));
    return marker.match(text).hasMatch();
}

// Close, left-aligned labels in menus and tables can have the same pitch as prose.
bool looks_like_short_label(const GroupingLine &line) {
    const QString &text = line.text;
    if (text.isEmpty() || line.rect.width() > line.rect.height() * 8 || QStringLiteral(".!?:-。！？：").contains(text.back()) || starts_list_item(text)) return false;
    if (is_cjk(text.front())) return text.size() <= 6;
    return text.front().isUpper() && text.size() <= 24 && text.count(u' ') <= 2;
}

// Whether next, the line below a paragraph that has taken current so far, starts another one.
bool starts_new_paragraph(const QVector<GroupingLine> &lines, const QVector<int> &current, int next) {
    if (starts_list_item(lines[next].text)) return true;
    if (current.size() == 1 && looks_like_short_label(lines[current.first()]) && looks_like_short_label(lines[next])) return true;
    if (current.size() >= MAX_PARAGRAPH_LINES) return true;
    if (current.size() < 2) return false;
    const auto &last = lines[current.last()];
    double left = lines[current.first()].rect.left(), right = lines[current.first()].rect.right();
    for (const int index : current) {
        left = std::min(left, lines[index].rect.left());
        right = std::max(right, lines[index].rect.right());
    }
    static const QString stops = QString::fromUtf8(".!?。！？");
    return !last.text.isEmpty() && stops.contains(last.text.back()) && last.rect.width() < SHORT_LAST_LINE * (right - left);
}

// A group is lines that line up, not necessarily one paragraph: its lines are read top to
// bottom and cut where a list item starts or a paragraph visibly ended.
QVector<QVector<int>> split_into_paragraphs(const QVector<GroupingLine> &lines, QVector<QVector<int>> groups) {
    QVector<QVector<int>> paragraphs;
    for (auto &group : groups) {
        std::stable_sort(group.begin(), group.end(), [&lines](int a, int b) {
            if (lines[a].rect.top() != lines[b].rect.top()) return lines[a].rect.top() < lines[b].rect.top();
            return lines[a].rect.left() < lines[b].rect.left();
        });
        QVector<int> current{group.first()};
        for (int position = 1; position < group.size(); ++position) {
            if (starts_new_paragraph(lines, current, group[position])) {
                paragraphs.append(current);
                current = {group[position]};
            } else {
                current.append(group[position]);
            }
        }
        paragraphs.append(current);
    }
    return paragraphs;
}
}

bool is_cjk(QChar c) {
    const auto u = c.unicode();
    return (u >= 0x3000 && u <= 0x30FF) || (u >= 0x3400 && u <= 0x9FFF) || (u >= 0xAC00 && u <= 0xD7AF) || (u >= 0xF900 && u <= 0xFAFF) ||
           (u >= 0xFF00 && u <= 0xFFEF);
}

QVector<QVector<int>> group_lines_into_paragraphs(const QVector<GroupingLine> &lines) {
    if (lines.isEmpty()) return {};
    auto groups = neighbour_groups(lines);
    join_indented_first_lines(lines, groups);
    return split_into_paragraphs(lines, std::move(groups));
}
}
