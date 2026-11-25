#pragma once
#include <QColor>
#include <QRectF>
#include <QString>

class QPainter;

// The group types and their line icons, drawn on the type cards.
namespace jietuba::clipboard {
const QString &KIND_NORMAL();
const QString &KIND_FILE();
const QString &KIND_HIDDEN();
const QString &KIND_FILTER();

// group_type is a GroupType value.
QString group_kind(int group_type);
void paint_kind_icon(QPainter *painter, const QRectF &rect, const QString &kind, const QColor &color);
}
