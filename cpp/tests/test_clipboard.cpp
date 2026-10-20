#include "clipboard/controllers/clipboard_controller.h"
#include "clipboard/core/manager.h"
#include "clipboard/core/text_transform.h"
#include "clipboard/services/file_payload_service.h"
#include "clipboard/services/group_service.h"
#include "clipboard/services/import_export_service.h"
#include "clipboard/services/manage_dialog_service.h"
#include "clipboard/ui/group_kind_style.h"
#include "clipboard/ui/image_item_actions.h"
#include "clipboard/ui/layout_scale.h"
#include "clipboard/ui/panels/setting_panel.h"
#include "core/py_text.h"
#include "core/ui_scale.h"
#include "settings/special_paste.h"
#include "settings/tool_settings.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

using namespace jietuba;
using namespace jietuba::clipboard;

namespace {
QJsonObject load_fixture() {
    QFile file(JIETUBA_CLIPBOARD_FIXTURE_PATH);
    if (!file.open(QIODevice::ReadOnly)) qFatal("missing clipboard fixture");
    return QJsonDocument::fromJson(file.readAll()).object();
}

QJsonArray entries_json(const QList<special_paste::Entry> &entries) {
    QJsonArray result;
    for (const auto &[key, enabled] : entries) result.append(QJsonArray{key, enabled});
    return result;
}

QList<special_paste::Entry> entries_from(const QJsonArray &array) {
    QList<special_paste::Entry> result;
    for (const auto &value : array) result.append({value[0].toString(), value[1].toBool()});
    return result;
}

std::optional<QString> optional_string(const QJsonValue &value) { return value.isNull() ? std::nullopt : std::optional<QString>(value.toString()); }

// The fixtures name filter groups by their former types 3, 4 and 5 (text, image, file); now one type with a rule.
std::pair<int, std::optional<FilterRule>> group_args(const QJsonValue &fixture_type) {
    static const QHash<int, QString> kinds = {{3, "text"}, {4, "image"}, {5, "file"}};
    const int type = fixture_type.toInt();
    if (kinds.contains(type)) return {static_cast<int>(GroupType::FILTER), FilterRule::of_kind(kinds.value(type))};
    return {type, std::nullopt};
}

QString describe(const QJsonValue &value) {
    if (value.isArray()) return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    if (value.isObject()) return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    return QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
}
}

// Pure clipboard logic and the context menus against the Python version (generate_clipboard_fixtures.py).
class ClipboardParityTests : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    QJsonObject fixture_;
    ToolSettingsManager *config_ = nullptr;

private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        // Settings and the clipboard database live in the throwaway folder.
        config_ = get_tool_settings_manager(new QSettings(dir_.filePath("settings.ini"), QSettings::IniFormat, this));
        config_->set_clipboard_db_path(QDir::toNativeSeparators(dir_.filePath("clipboard/clipboard.db")));
        fixture_ = load_fixture();
    }

    void pythonStringAndPathSemanticsMatch() {
        const auto py = fixture_["py_text"].toObject();
        for (const auto &value : py["strings"].toArray()) {
            const auto c = value.toObject();
            const QString text = c["text"].toString();
            const auto check = [&](const QString &actual, const char *key) {
                QVERIFY2(actual == c[key].toString(), qPrintable(QString(key) + ": " + describe(text) + " -> " + describe(actual) + " != " + describe(c[key])));
            };
            QCOMPARE(py_len(text), c["len"].toInteger());
            check(py_strip(text), "strip");
            check(py_lstrip(text), "lstrip");
            check(py_rstrip(text), "rstrip");
            check(py_strip_chars(text, "x"), "strip_x");
            check(py_upper(text), "upper");
            check(py_lower(text), "lower");
            check(py_title(text), "title");
            check(py_swapcase(text), "swapcase");
            check(py_casefold(text), "casefold");
            check(py_slice(text, 2, 5), "slice_2_5");
            check(py_slice(text, -3), "slice_neg");
            check(py_slice(text, 0, -2), "slice_neg_end");
        }
        for (const auto &value : py["isspace"].toArray())
            QVERIFY2(py_isspace(static_cast<char32_t>(value.toArray()[0].toInt())) == value.toArray()[1].toBool(), qPrintable(QString::number(value.toArray()[0].toInt(), 16)));
        for (const auto &value : py["paths"].toArray()) {
            const auto c = value.toObject();
            const QString path = c["path"].toString();
            QCOMPARE(py_basename(path), c["basename"].toString());
            QCOMPARE(py_dirname(path), c["dirname"].toString());
            QCOMPARE(py_normpath(path), c["normpath"].toString());
            QCOMPARE(py_normcase(path), c["normcase"].toString());
        }
        for (const auto &value : py["joins"].toArray()) {
            const auto c = value.toObject();
            QCOMPARE(py_path_join(c["a"].toString(), c["b"].toString()), c["expected"].toString());
        }
    }

    void textTransformsMatchTheReference() {
        for (const auto &value : fixture_["transforms"].toArray()) {
            const auto c = value.toObject();
            const QString actual = TRANSFORM_REGISTRY().at(c["key"].toString())(c["input"].toString());
            QVERIFY2(actual == c["expected"].toString(), qPrintable(c["key"].toString() + ": " + describe(c["input"]) + " -> " + describe(actual) + " != " + describe(c["expected"])));
        }
        for (const auto &value : fixture_["sql_in"].toArray()) {
            const auto c = value.toObject();
            QCOMPARE(to_sql_in_clause(c["input"].toString(), c["max"].toInt()), c["expected"].toString());
        }
        const auto append = fixture_["append_time"].toObject();
        const QDateTime now = QDateTime::fromString(append["now"].toString(), Qt::ISODate);
        for (const auto &value : append["cases"].toArray()) {
            const auto c = value.toObject();
            QCOMPARE(append_current_time(c["input"].toString(), now), c["expected"].toString());
        }
    }

    void filterGroupIconsFollowTheirRule() {
        const int filter = static_cast<int>(GroupType::FILTER);
        const FilterRule app_only{std::nullopt, QString("chrome.exe")};
        QCOMPARE(get_default_group_icon(filter, FilterRule::of_kind(SUB_KIND_URL)), QString::fromUtf8("🔗"));
        QCOMPARE(get_default_group_icon(filter, FilterRule::of_kind(SUB_KIND_COLOR)), QString::fromUtf8("🎨"));
        QCOMPARE(get_default_group_icon(filter, FilterRule::of_kind(SUB_KIND_PATH)), QString::fromUtf8("📂"));
        QCOMPARE(get_default_group_icon(filter, app_only), QString::fromUtf8("🧩"));
        // A kind wins over the source app; an unreadable rule looks like a text filter.
        QCOMPARE(get_default_group_icon(filter, FilterRule{QString("image"), QString("chrome.exe")}), QString::fromUtf8("🖼️"));
        QCOMPARE(get_default_group_icon(filter), QString::fromUtf8("📝"));
        // These defaults follow a type change like the others; a custom icon stays.
        QCOMPARE(get_toggled_default_group_icon(QString::fromUtf8("🔗"), static_cast<int>(GroupType::NORMAL)), GENERAL_GROUP_ICON());
        QCOMPARE(get_toggled_default_group_icon(QString::fromUtf8("🧩"), filter, FilterRule::of_kind(SUB_KIND_PATH)), QString::fromUtf8("📂"));
        QCOMPARE(get_toggled_default_group_icon(QString::fromUtf8("⭐"), filter, app_only), QString::fromUtf8("⭐"));
        // Quick launch takes path text as well as files.
        QVERIFY(group_accepts(static_cast<int>(GroupType::FILE), "text", SUB_KIND_PATH));
        QVERIFY(group_accepts(static_cast<int>(GroupType::FILE), "text", SUB_KIND_URL));
        QVERIFY(!group_accepts(filter, "text", SUB_KIND_PATH));
    }

    void filterRulesRoundTripAsJson() {
        const FilterRule both{QString("image"), QString("Weixin.exe")};
        QCOMPARE(FilterRule::from_json(QJsonDocument::fromJson(both.to_json()).object()), std::optional(both));
        QCOMPARE(FilterRule::of_kind("url").to_json(), QByteArray(R"({"kind":"url"})"));
        QCOMPARE(FilterRule::from_json(QJsonValue()), std::optional<FilterRule>());
        QCOMPARE(FilterRule::from_json(QJsonObject{{"app", "x.exe"}}), std::optional(FilterRule{std::nullopt, QString("x.exe")}));
    }

    void filePayloadsMatchTheReference() {
        const auto f = fixture_["file_payload"].toObject();
        for (const auto &value : f["normalize"].toArray()) QCOMPARE(normalize_file_path(value.toObject()["input"].toString()), value.toObject()["expected"].toString());
        for (const auto &value : f["build"].toArray()) QCOMPARE(build_file_payload(value.toObject()["input"].toString()), value.toObject()["expected"].toString());
        for (const auto &value : f["extract"].toArray())
            QVERIFY2(extract_first_file_path_from_content(optional_string(value.toObject()["input"])) == value.toObject()["expected"].toString(), qPrintable(describe(value.toObject()["input"])));
    }

    void groupHelpersMatchTheReference() {
        const auto g = fixture_["groups"].toObject();
        QCOMPARE(GENERAL_GROUP_ICON(), g["icons"].toObject()["general"].toString());
        QCOMPARE(QUICK_LAUNCH_GROUP_ICON(), g["icons"].toObject()["quick_launch"].toString());
        QCOMPARE(HIDDEN_GROUP_ICON(), g["icons"].toObject()["hidden"].toString());
        for (const auto &value : g["default"].toArray()) {
            const auto [type, rule] = group_args(value.toObject()["type"]);
            QCOMPARE(get_default_group_icon(type, rule), value.toObject()["expected"].toString());
        }
        for (const auto &value : g["kinds"].toArray()) QCOMPARE(group_kind(group_args(value.toObject()["type"]).first), value.toObject()["expected"].toString());
        for (const auto &value : g["display"].toArray()) {
            const auto [type, rule] = group_args(value.toObject()["type"]);
            QCOMPARE(get_group_display_icon(optional_string(value.toObject()["icon"]), type, rule), value.toObject()["expected"].toString());
        }
        for (const auto &value : g["toggled"].toArray()) {
            const auto [type, rule] = group_args(value.toObject()["type"]);
            QCOMPARE(get_toggled_default_group_icon(value.toObject()["icon"].toString(), type, rule), value.toObject()["expected"].toString());
        }
        for (const auto &value : g["accepts"].toArray())
            QCOMPARE(group_accepts(group_args(value.toObject()["type"]).first, value.toObject()["content_type"].toString()), value.toObject()["expected"].toBool());
        QList<Group> existing;
        for (const auto &value : g["existing"].toArray()) {
            Group group;
            group.id = value[0].toInteger();
            group.name = value[1].toString();
            existing.append(group);
        }
        for (const auto &value : g["name_exists"].toArray()) {
            const auto exclude = value.toObject()["exclude"].isNull() ? std::nullopt : std::optional<qint64>(value.toObject()["exclude"].toInteger());
            QCOMPARE(group_name_exists(existing, value.toObject()["name"].toString(), exclude), value.toObject()["expected"].toBool());
        }
        for (const auto &value : g["unique"].toArray()) {
            QSet<QString> used;
            for (const auto &name : value.toObject()["used"].toArray()) used.insert(name.toString());
            QCOMPARE(make_unique_group_name(value.toObject()["base"].toString(), used), value.toObject()["expected"].toString());
        }
        QCOMPARE(build_delete_group_confirm_message([](const char *text) { return "<" + QString::fromUtf8(text) + ">"; }), g["delete_message"].toString());
    }

    void specialPasteEntriesMatchTheReference() {
        const auto s = fixture_["special_paste"].toObject();
        const auto options = s["options"].toArray();
        QCOMPARE(special_paste::BUILTIN_OPTIONS().size(), options.size());
        for (int i = 0; i < options.size(); ++i) {
            const auto &option = special_paste::BUILTIN_OPTIONS()[i];
            const auto expected = options[i].toObject();
            QCOMPARE(option.key, expected["key"].toString());
            QCOMPARE(QString::fromUtf8(option.label), expected["label"].toString());
            QCOMPARE(QString::fromUtf8(option.description), expected["description"].toString());
            QCOMPARE(option.content_type, expected["content_type"].toString());
            QCOMPARE(option.group, expected["group"].toString());
            QCOMPARE(option.enabled, expected["enabled"].toBool());
        }
        QCOMPARE(entries_json(special_paste::default_entries()), s["defaults"].toArray());
        for (const auto &value : s["normalize"].toArray())
            QVERIFY2(entries_json(special_paste::normalize(value.toObject()["input"])) == value.toObject()["expected"].toArray(), qPrintable(describe(value.toObject()["input"])));
        for (const auto &value : s["load"].toArray()) {
            config_->set_app_setting(special_paste::SETTING_KEY, value.toObject()["raw"].toString());
            QVERIFY2(entries_json(special_paste::load(config_)) == value.toObject()["expected"].toArray(), qPrintable(value.toObject()["raw"].toString()));
        }
        // What save() stores is the reference's json.dumps text.
        special_paste::save(config_, entries_from(QJsonDocument::fromJson(s["saved"].toString().toUtf8()).array()));
        QCOMPARE(config_->get_app_setting(special_paste::SETTING_KEY).toString(), s["saved"].toString());
        config_->set_app_setting(special_paste::SETTING_KEY, "");
    }

    void separatorsTimeRangesAndFiltersMatchTheReference() {
        for (const auto &value : fixture_["separators"].toArray()) {
            QCOMPARE(escape_separator(value.toObject()["text"].toString()), value.toObject()["escaped"].toString());
            QCOMPARE(unescape_separator(value.toObject()["text"].toString()), value.toObject()["unescaped"].toString());
        }
        for (const auto &value : fixture_["time_ranges"].toArray()) {
            const auto parsed = ClipboardController::parse_time_range_text(value.toObject()["text"].toString());
            const QJsonValue actual = parsed ? QJsonValue(QJsonArray{parsed->first.toString(Qt::ISODate), parsed->second.toString(Qt::ISODate)}) : QJsonValue();
            QVERIFY2(actual == value.toObject()["expected"], qPrintable(value.toObject()["text"].toString() + " -> " + describe(actual)));
        }
        for (const auto &value : fixture_["save_filters"].toArray())
            QCOMPARE(format_from_save_filter(value.toObject()["path"].toString(), value.toObject()["filter"].toString()), value.toObject()["expected"].toString());
    }

    void layoutSizesMatchTheReference() {
        for (const auto &value : fixture_["capacities"].toArray()) {
            get_ui_scale().set_percent(value.toObject()["percent"].toInt());
            for (const auto &pair : value.toObject()["sidebar"].toArray()) QCOMPARE(calc_sidebar_capacity(pair[0].toInt()), pair[1].toInt());
            for (const auto &pair : value.toObject()["topbar"].toArray()) QCOMPARE(calc_topbar_capacity(pair[0].toInt()), pair[1].toInt());
        }
        get_ui_scale().set_percent(100);
        for (const auto &value : fixture_["dialog_sizes"].toArray()) {
            get_dialog_scale().set_percent(value.toObject()["percent"].toInt());
            QCOMPARE(manage_dialog_width(), value.toObject()["width"].toInt());
            QCOMPARE(manage_dialog_height(), value.toObject()["height"].toInt());
            QCOMPARE(manage_dialog_min_width(), value.toObject()["min_width"].toInt());
            QCOMPARE(manage_dialog_min_height(), value.toObject()["min_height"].toInt());
            for (const auto &fit : value.toObject()["fit"].toArray()) QVERIFY(fit_manage_dialog_size(fit[0].toInt(), fit[1].toInt()) == std::make_pair(fit[2].toInt(), fit[3].toInt()));
            for (const auto &fit : value.toObject()["min_fit"].toArray()) QVERIFY(fit_manage_dialog_min_size(fit[0].toInt(), fit[1].toInt()) == std::make_pair(fit[2].toInt(), fit[3].toInt()));
            const double factor = value.toObject()["percent"].toInt() / 100.0;
            for (const auto &size : value.toObject()["size"].toArray()) QVERIFY(manage_dialog_size(size[0].toInt(), size[1].toInt(), factor) == std::make_pair(size[2].toInt(), size[3].toInt()));
        }
        get_dialog_scale().set_percent(100);
    }

    void csvMatchesTheReference() {
        const auto c = fixture_["csv"].toObject();
        for (const auto &value : c["format"].toArray()) {
            QStringList row;
            for (const auto &cell : value.toObject()["row"].toArray()) row << cell.toString();
            QCOMPARE(format_csv_row(row), value.toObject()["expected"].toString());
        }
        auto rows_json = [](const QList<QStringList> &rows) {
            QJsonArray result;
            for (const auto &row : rows) result.append(QJsonArray::fromStringList(row));
            return result;
        };
        for (const auto &value : c["parse"].toArray())
            QVERIFY2(rows_json(parse_csv(value.toObject()["content"].toString())) == value.toObject()["expected"].toArray(), qPrintable(describe(value.toObject()["content"])));
        int index = 0;
        for (const auto &value : c["import"].toArray()) {
            const QString path = dir_.filePath(QString("import_%1.csv").arg(index++));
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(value.toObject()["content"].toString().toUtf8());
            file.close();
            QJsonArray actual;
            for (const auto &[group, content, title] : read_import_rows(path)) actual.append(QJsonArray{group, content, title});
            QVERIFY2(actual == value.toObject()["expected"].toArray(), qPrintable(describe(value.toObject()["content"])));
        }
    }

    // Exports in each offered encoding and imports of files saved in other encodings.
    void csvEncodingsMatchTheReference() {
        const auto c = fixture_["csv"].toObject();
        QStringList header;
        for (const auto &cell : c["header"].toArray()) header << cell.toString();
        QList<QStringList> rows;
        for (const auto &row : c["rows"].toArray()) {
            QStringList cells;
            for (const auto &cell : row.toArray()) cells << cell.toString();
            rows << cells;
        }
        int index = 0;
        for (const auto &value : c["export_encoded"].toArray()) {
            const auto object = value.toObject();
            const QString encoding = object["encoding"].toString();
            const QString path = dir_.filePath(QString("export_%1.csv").arg(index++));
            write_csv_rows(path, header, rows, encoding);
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QByteArray actual = file.readAll();
            const QByteArray expected = QByteArray::fromBase64(object["bytes"].toString().toLatin1());
            QVERIFY2(actual == expected, qPrintable(encoding + "\n" + QString::fromLatin1(actual.toHex(' ')) + "\n" + QString::fromLatin1(expected.toHex(' '))));
        }
        for (const auto &value : c["import_encoded"].toArray()) {
            const auto object = value.toObject();
            const QByteArray data = QByteArray::fromBase64(object["bytes"].toString().toLatin1());
            const QString path = dir_.filePath(QString("import_encoded_%1.csv").arg(index++));
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(data);
            file.close();
            QJsonArray actual;
            for (const auto &[group, content, title] : read_import_rows(path)) actual.append(QJsonArray{group, content, title});
            QVERIFY2(actual == object["expected"].toArray(),
                     qPrintable(QString::fromUtf8(data.toHex(' ') + "\n" + QJsonDocument(actual).toJson(QJsonDocument::Compact) + "\n" +
                                                  QJsonDocument(object["expected"].toArray()).toJson(QJsonDocument::Compact))));
        }
    }

    // The same records as the generator, in the same order, so ids match a fresh database.
    void contextMenusMatchTheReference() {
        const auto menus = fixture_["menus"].toObject();
        auto &manager = ClipboardManager::instance();
        QVERIFY(manager.is_available());
        QCOMPARE(QDir::cleanPath(manager.get_db_path().value_or(QString())), QDir::cleanPath(dir_.filePath("clipboard/clipboard.db")));
        // A repeated run (check.ps1 -Mode Leak) finds the first run's records; start a new database.
        if (!manager.get_groups().isEmpty()) {
            QVERIFY(manager.release_storage());
            QVERIFY(QDir(dir_.filePath("clipboard")).removeRecursively());
            QVERIFY(manager.reset_storage(QDir::toNativeSeparators(dir_.filePath("clipboard/clipboard.db"))));
        }
        QList<qint64> group_ids, item_ids;
        for (const auto &value : menus["groups"].toArray()) {
            const auto [type, rule] = group_args(value.toObject()["type"]);
            const auto id = manager.create_group(value.toObject()["name"].toString(), std::nullopt, optional_string(value.toObject()["icon"]), type, rule);
            QVERIFY(id);
            group_ids << *id;
        }
        for (const auto &value : menus["items"].toArray()) {
            const auto id = manager.add_item(value.toObject()["content"].toString(), value.toObject()["type"].toString(), optional_string(value.toObject()["title"]));
            QVERIFY(id);
            if (value.toObject()["pinned"].toBool()) manager.toggle_pin(*id);
            if (!value.toObject()["group"].isNull()) QVERIFY(manager.move_to_group(*id, group_ids[value.toObject()["group"].toInt()]));
            item_ids << *id;
        }
        std::function<QJsonArray(const QList<MenuAction> &)> menu_json = [&](const QList<MenuAction> &actions) {
            QJsonArray result;
            for (const auto &action : actions) {
                QString key = action.key;
                if (key.startsWith("move_to_group_")) key = QString("move_to_group_{G%1}").arg(group_ids.indexOf(key.mid(14).toLongLong()));
                result.append(QJsonObject{{"label", action.label}, {"key", key}, {"enabled", action.enabled}, {"checkable", action.checkable}, {"checked", action.checked},
                                          {"separator", action.is_separator}, {"translate", action.translate_label}, {"children", menu_json(action.children)}});
            }
            return result;
        };

        const auto configs = menus["configs"].toObject();
        const auto queries = menus["queries"].toArray();
        for (const QString &config_name : {QString("default"), QString("custom"), QString("text_off")}) {
            special_paste::save(config_, entries_from(configs[config_name].toArray()));
            ClipboardController controller(manager);
            bool loaded = false;
            std::optional<qint64> loaded_group;
            for (const auto &value : queries) {
                const auto query = value.toObject();
                if (query["special_paste"].toString() != config_name) continue;
                const auto group = query["group"].isNull() ? std::nullopt : std::optional<qint64>(group_ids[query["group"].toInt()]);
                QJsonValue actual;
                const QString kind = query["kind"].toString();
                if (kind == "group") {
                    actual = menu_json(controller.build_group_context_menu_data(*group));
                } else {
                    if (!loaded || loaded_group != group) {
                        controller.switch_to_group(group);
                        controller.load_history();
                        loaded = true;
                        loaded_group = group;
                    }
                    QList<qint64> ids;
                    for (const auto &index : query["items"].toArray()) ids << (index.toInt() < item_ids.size() ? item_ids[index.toInt()] : 999999);
                    if (kind == "item") {
                        const auto data = controller.build_context_menu_data(ids.first());
                        if (data) actual = menu_json(data->actions);
                    } else {
                        const auto data = controller.build_marked_context_menu_data(ids);
                        if (data) actual = QJsonObject{{"item", item_ids.indexOf(data->item_id)}, {"actions", menu_json(data->actions)}};
                    }
                }
                QVERIFY2(actual == query["expected"], qPrintable(describe(query) + "\nactual: " + describe(actual)));
            }
        }
        config_->set_app_setting(special_paste::SETTING_KEY, "");
    }
};

QTEST_MAIN(ClipboardParityTests)
#include "test_clipboard.moc"
