#include "clipboard/core/kind_catalog.h"
#include "clipboard/core/models.h"
#include "core/native.h"
#include <QJsonArray>
#include <QJsonDocument>

namespace jietuba::clipboard {
const QList<KindInfo> &kind_catalog() {
    static const QList<KindInfo> catalog = {
        {"text", "Text", QString::fromUtf8("📝")},
        {SUB_KIND_URL, "Link", QString::fromUtf8("🔗")},
        {SUB_KIND_COLOR, "Color", QString::fromUtf8("🎨")},
        {SUB_KIND_PATH, "Path", QString::fromUtf8("📂")},
        {"image", "Image", QString::fromUtf8("🖼️")},
        {"file", "File", QString::fromUtf8("📄")},
        {"file:image", nullptr, QString::fromUtf8("🖼️")},
        {"file:spreadsheet", nullptr, QString::fromUtf8("📊")},
        {"file:document", nullptr, QString::fromUtf8("📄")},
        {"file:presentation", nullptr, QString::fromUtf8("📽️")},
        {"file:pdf", nullptr, QString::fromUtf8("📕")},
        {"file:markdown", nullptr, QString::fromUtf8("📝")},
        {"file:text", nullptr, QString::fromUtf8("📄")},
        {"file:archive", nullptr, QString::fromUtf8("🗜️")},
        {"file:audio", nullptr, QString::fromUtf8("🎵")},
        {"file:video", nullptr, QString::fromUtf8("🎬")},
        {"file:folder", nullptr, QString::fromUtf8("📁")},
    };
    return catalog;
}

const KindInfo *find_kind(const QString &key) {
    for (const auto &kind : kind_catalog())
        if (kind.key == key) return &kind;
    return nullptr;
}

QList<const KindInfo *> offered_kinds() {
    QList<const KindInfo *> kinds;
    for (const auto &kind : kind_catalog())
        if (kind.label) kinds.append(&kind);
    return kinds;
}

QString kind_key(const QString &content_type, const QString &sub_kind) {
    if (sub_kind.isEmpty()) return content_type;
    return content_type == "file" ? "file:" + sub_kind : sub_kind;
}

QStringList native_filter_kinds() {
    QStringList kinds;
    char *json = jb_clipboard_filter_kinds();
    if (!json) return kinds;
    for (const auto &value : QJsonDocument::fromJson(json).array()) kinds.append(value.toString());
    jb_clipboard_text_free(json);
    return kinds;
}
}
