#include "clipboard/ui/resources/emoji_data.h"
#include "core/resource_manager.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace jietuba::clipboard {
std::pair<QStringList, QHash<QString, QStringList>> get_emoji_groups() {
    static QStringList cached_order;
    static QHash<QString, QStringList> cached_groups;
    if (!cached_groups.isEmpty()) return {cached_order, cached_groups};

    QFile file(ResourceManager::get_resource_path("svg/emoji_groups.json"));
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto data = QJsonDocument::fromJson(file.readAll()).object();
    for (const auto &name : data.value("order").toArray()) cached_order.append(name.toString());
    const auto groups = data.value("groups").toObject();
    for (auto it = groups.begin(); it != groups.end(); ++it) {
        QStringList emojis;
        for (const auto &emoji : it.value().toArray()) emojis.append(emoji.toString());
        cached_groups.insert(it.key(), emojis);
    }
    return {cached_order, cached_groups};
}

QString get_group_icon(const QString &group_name) {
    static const QHash<QString, QString> icons{
        {"Smileys & Emotion", QString::fromUtf8("😀")}, {"People & Body", QString::fromUtf8("👋")}, {"Animals & Nature", QString::fromUtf8("🐱")},
        {"Food & Drink", QString::fromUtf8("🍎")},      {"Travel & Places", QString::fromUtf8("✈️")}, {"Activities", QString::fromUtf8("⚽")},
        {"Objects", QString::fromUtf8("💡")},           {"Symbols", QString::fromUtf8("❤️")},
    };
    return icons.value(group_name, QString::fromUtf8("📁"));
}
}
