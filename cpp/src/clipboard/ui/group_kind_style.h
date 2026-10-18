#pragma once
#include <QColor>
#include <QRectF>
#include <QString>

class QPainter;

namespace jietuba {
struct UIThemeTokens;
}

// Identifying colours and line icons of the group types, shared by the sidebar rows and the type cards.
namespace jietuba::clipboard {
const QString &KIND_NORMAL();
const QString &KIND_FILE();
const QString &KIND_HIDDEN();
const QString &KIND_FILTER();

// group_type is a GroupType value.
QString group_kind(int group_type);
QColor kind_color(const QString &kind);
// A tinted background: the dark theme's base is darker, so the same alpha would look too faint; add a little.
QColor kind_tint(const QString &kind, double alpha, const UIThemeTokens &tokens);
// Text on the tint: a darker shade in the light theme, a lighter one in the dark theme.
QColor kind_text_color(const QString &kind, const UIThemeTokens &tokens);
void paint_kind_icon(QPainter *painter, const QRectF &rect, const QString &kind, const QColor &color);
}
