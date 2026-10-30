#pragma once
#include <QList>
#include <QString>
#include <QStringList>

namespace jietuba::clipboard {
// How each kind a filter rule can name is presented: the content types, the text sub-kinds and
// file:<file sub-kind> (group_kind.rs filter_kinds). Which kinds exist and which an item gets is
// decided in the Rust core; this table only adds labels and icons, and a test keeps both in step.
struct KindInfo {
    QString key;
    // Translation source; null while the kind is not offered in the type lists.
    const char *label;
    // Default icon of a filter group on this kind.
    QString filter_icon;
};

const QList<KindInfo> &kind_catalog();
const KindInfo *find_kind(const QString &key);
// The kinds the type lists offer, in order; the "all types" entry is the caller's.
QList<const KindInfo *> offered_kinds();
// The filter rule key of an item with this content type and sub-kind.
QString kind_key(const QString &content_type, const QString &sub_kind);
// The keys as the Rust core lists them.
QStringList native_filter_kinds();
}
