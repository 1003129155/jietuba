#include "clipboard/core/manager.h"
#include "core/constants.h"
#include "core/global_hotkey.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "settings/tool_settings.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <stdexcept>
#include <windows.h>

using namespace jietuba;

// Expected values come from the Python reference (scripts/generate_core_fixtures.py).
class CoreTests : public QObject {
    Q_OBJECT
    QJsonObject fixtures_;
private slots:
    void initTestCase() {
        QFile file(JIETUBA_CORE_FIXTURE_PATH);
        QVERIFY(file.open(QIODevice::ReadOnly));
        fixtures_ = QJsonDocument::fromJson(file.readAll()).object();
        QVERIFY(!fixtures_.isEmpty());
    }
    void floatRepr() {
        for (const auto &value : fixtures_["float_repr"].toArray()) {
            const auto item = value.toObject();
            QCOMPARE(python_str(item["value"].toDouble()), item["expected"].toString());
        }
    }
    void levelNames() {
        const auto names = fixtures_["level_names"].toObject();
        for (auto it = names.begin(); it != names.end(); ++it) QCOMPARE(LogLevel::name(it.key().toInt()), it.value().toString());
    }
    void renderTranslatedTemplates() {
        int checked = 0;
        for (const auto &value : fixtures_["render"].toArray()) {
            const auto item = value.toObject();
            // The reference raises KeyError for a missing field; the translation logs the raw template instead.
            const auto expected = item["expected"].isNull() ? item["template"].toString() : item["expected"].toString();
            LogMsg::Args args;
            const auto object = item["args"].toObject();
            for (auto it = object.begin(); it != object.end(); ++it) args.emplace_back(it.key(), it.value().toVariant());
            set_language(item["language"].toString());
            QCOMPARE(LogMsg(item["template"].toString(), args, true).render(), expected);
            ++checked;
        }
        QVERIFY(checked > 100);
        set_language("ja");
    }

    void globalHotkeyRoutesAndParsing() {
        const auto cases = fixtures_["global_hotkey"].toObject();
        int checked = 0;
        for (const auto &value : cases["route"].toArray()) {
            const auto item = value.toObject();
            const QString text = item["text"].toString();
            QCOMPARE(global_hotkey::route_name(global_hotkey::route(text)), item["route"].toString());
            std::optional<std::pair<int, int>> parsed;
            try {
                parsed = global_hotkey::parse_hotkey(text);
            } catch (const std::invalid_argument &) {
            }
            if (item["parsed"].isNull()) {
                QVERIFY2(!parsed, qPrintable(text));
            } else {
                QVERIFY2(parsed, qPrintable(text));
                QCOMPARE(parsed->first, item["parsed"].toArray()[0].toInt());
                QCOMPARE(parsed->second, item["parsed"].toArray()[1].toInt());
            }
            ++checked;
        }
        QVERIFY(checked > 500);
    }
    void globalHotkeyFormatting() {
        const auto cases = fixtures_["global_hotkey"].toObject();
        for (const auto &value : cases["format"].toArray()) {
            const auto item = value.toObject();
            const auto text = global_hotkey::format_hotkey(item["mods"].toInt(), item["vk"].toInt());
            if (item["expected"].isNull()) QVERIFY(!text);
            else QCOMPARE(text.value_or("<none>"), item["expected"].toString());
        }
        for (const auto &value : cases["route_of"].toArray()) {
            const auto item = value.toObject();
            QCOMPARE(global_hotkey::route_name(global_hotkey::route_of(item["mods"].toInt(), item["vk"].toInt())), item["expected"].toString());
        }
        const auto names = cases["modifier_names"].toObject();
        for (auto it = names.begin(); it != names.end(); ++it) {
            QStringList expected;
            for (const auto &name : it.value().toArray()) expected.append(name.toString());
            QCOMPARE(global_hotkey::modifier_names(it.key().toInt()), expected);
            QCOMPARE(global_hotkey::modifier_mask(expected), it.key().toInt());
        }
    }

    // Not from the reference: settings and data are shared with the Python version, so the
    // test executables' defaults must land in the sandbox (tests/test_sandbox.cpp).
    void testDefaultsStayInTheSandbox() {
        const QString sandbox = QDir::toNativeSeparators(test_sandbox_dir());
        QVERIFY(!sandbox.isEmpty());
        QVERIFY(QDir::toNativeSeparators(get_app_data_dir()).startsWith(sandbox));
        QVERIFY(QDir::toNativeSeparators(clipboard::ClipboardManager::default_db_path()).startsWith(sandbox));
        auto &store = get_tool_settings_manager()->qsettings.settings();
        QCOMPARE(store.format(), QSettings::IniFormat);
        QVERIFY(QDir::toNativeSeparators(store.fileName()).startsWith(sandbox));
    }
    // The registry is shared with the Python version, which writes its ints as REG_DWORD.
    void integersAreStoredLikePythonInts() {
        const QString path = QString("Software\\JietubaCppTest\\Store%1").arg(QCoreApplication::applicationPid());
        {
            QSettings native("HKEY_CURRENT_USER\\" + path, QSettings::NativeFormat);
            SettingsStore store(native);
            store.setValue("small", QVariant::fromValue(qint64(139)));
            store.setValue("negative", QVariant::fromValue(qint64(-5)));
            store.setValue("large", QVariant::fromValue(qint64(1) << 40));
            store.setValue("text", QString("12"));
            native.sync();
        }
        const auto type_of = [&path](const wchar_t *name) {
            DWORD type = 0;
            RegGetValueW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(path.utf16()), name, RRF_RT_ANY, &type, nullptr, nullptr);
            return type;
        };
        const DWORD small_type = type_of(L"small"), negative_type = type_of(L"negative"), large_type = type_of(L"large"),
                    text_type = type_of(L"text");
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\JietubaCppTest");
        QCOMPARE(small_type, DWORD(REG_DWORD));
        QCOMPARE(negative_type, DWORD(REG_DWORD));
        QCOMPARE(large_type, DWORD(REG_QWORD));
        QCOMPARE(text_type, DWORD(REG_SZ));
    }
private:
    static void set_language(const QString &language) {
        // load_language also installs catalogs; the logger only reads the current code.
        I18nManager::load_language(language);
    }
};

QTEST_MAIN(CoreTests)
#include "test_core.moc"
