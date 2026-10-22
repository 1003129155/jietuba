#include "core/constants.h"
#include "core/data_location.h"
#include "settings/data_folder.h"
#include "settings/tool_settings.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <optional>
#include <thread>
#include <utility>
#include <windows.h>

using namespace jietuba;
using data_folder::Source;
using data_folder::StartupEntries;

namespace {
void write_file(const QString &path, const QByteArray &content) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) file.write(content);
}

// A value as text, lists one item per line: the registry keeps types the INI file does not.
QString text_of(const QVariant &value) {
    const int type = value.typeId();
    if (type == QMetaType::QStringList || type == QMetaType::QVariantList) return value.toStringList().join('\n');
    return value.toString();
}

// A different value of the default's type.
QVariant changed(const QVariant &value) {
    switch (value.typeId()) {
    case QMetaType::Bool: return !value.toBool();
    case QMetaType::Int:
    case QMetaType::LongLong: return int(value.toLongLong() + 3);
    case QMetaType::Double: return value.toDouble() + 0.25;
    case QMetaType::QStringList: return QStringList{"a", "b, c"};
    case QMetaType::QString: return value.toString() + ", \"q\" \\ ; # = é";
    default: return {};
    }
}

bool registry_key_exists(const QString &subkey) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(subkey.utf16()), 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    RegCloseKey(key);
    return true;
}

// A source with its settings in an INI file of folder.
Source file_source(const QDir &folder, const QVariantMap &settings) {
    QSettings store(folder.filePath("old.ini"), QSettings::IniFormat);
    for (auto it = settings.begin(); it != settings.end(); ++it) store.setValue(it.key(), it.value());
    store.sync();
    return {store.fileName(), {}, {}};
}
}

// Where the data lives, which settings stay in the registry, taking in older versions' settings,
// and the startup entries.
class DataFolderTests : public QObject {
    Q_OBJECT
private:
    // Under HKCU\Software\JietubaTest, removed when the tests end.
    const QString registry_ = QString("Software\\JietubaTest\\DataFolder%1").arg(QCoreApplication::applicationPid());
    QString registry_path(const QString &name) const { return "HKEY_CURRENT_USER\\" + registry_ + "\\" + name; }
private slots:
    void init() { data_folder::write_deferred_log(); }
    void cleanupTestCase() {
        RegDeleteTreeW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(registry_.utf16()));
        RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\JietubaTest");
        QVERIFY(!registry_key_exists(registry_));
    }

    void aSettingsFileBesideTheProgramComesFirst() {
        QTemporaryDir program, roaming;
        const auto installed = resolve_data_location(program.path(), roaming.path());
        QVERIFY(!installed.beside_program);
        QCOMPARE(installed.dir, QDir(roaming.path()).filePath("Jietuba"));
        QCOMPARE(installed.settings_file(), QDir(roaming.path()).filePath("Jietuba/settings.ini"));
        // A folder of that name is not the settings file.
        QVERIFY(QDir(program.path()).mkdir("settings.ini"));
        QVERIFY(!resolve_data_location(program.path(), roaming.path()).beside_program);
        QVERIFY(QDir(program.path()).rmdir("settings.ini"));
        write_file(QDir(program.path()).filePath("settings.ini"), {});
        const auto beside = resolve_data_location(QDir::toNativeSeparators(program.path()), roaming.path());
        QVERIFY(beside.beside_program);
        QCOMPARE(beside.settings_file(), QDir(program.path()).filePath("settings.ini"));
        QVERIFY(!resolve_data_location({}, roaming.path()).beside_program);
        // The sandbox never looks beside the test program.
        QVERIFY(!data_location().beside_program);
        QVERIFY(QDir::toNativeSeparators(data_location().dir).startsWith(QDir::toNativeSeparators(test_sandbox_dir())));
    }

    // The settings bound to this computer, by the table's scope column.
    void localSettingsAreTheTablesLocalRows() {
        const QSet<QString> expected{"app/screenshot_save_path", "app/save_dialog_last_dir", "clipboard/db_path", "app/log_dir",
                                     "app/autostart_enabled", "app/run_as_admin", "app/has_run_before", "app/welcome_wizard_done"};
        QCOMPARE(LOCAL_SETTING_PATHS(), expected);
        QCOMPARE(LOCAL_SETTINGS_KEY, QString("HKEY_CURRENT_USER\\Software\\Jietuba\\ToolSettings"));
    }

    // Local settings are read and written in the registry, the rest in the settings file; deleting the
    // file resets only the rest.
    void localSettingsLiveInTheRegistry() {
        QTemporaryDir data;
        const QString ini_path = QDir(data.path()).filePath("settings.ini");
        QSettings registry(registry_path("Split\\ToolSettings"), QSettings::NativeFormat);
        {
            QSettings ini(ini_path, QSettings::IniFormat);
            ToolSettingsManager config(ini, &registry);
            config.set_log_dir(data.path());
            config.set_clipboard_db_path(QDir(data.path()).filePath("history/clipboard.db"));
            config.set_autostart_enabled(false);
            config.set_hotkey("ctrl+9");
            config.qsettings.sync();
            QCOMPARE(registry.value("app/log_dir").toString(), data.path());
            QVERIFY(registry.contains("clipboard/db_path"));
            QVERIFY(registry.contains("app/autostart_enabled"));
            QVERIFY(!registry.contains("app/hotkey"));
            QCOMPARE(ini.value("app/hotkey").toString(), QString("ctrl+9"));
            for (const auto &key : LOCAL_SETTING_PATHS()) QVERIFY2(!ini.contains(key), qPrintable(key));
        }
        QVERIFY(QFile::remove(ini_path));
        QSettings ini(ini_path, QSettings::IniFormat);
        ToolSettingsManager config(ini, &registry);
        QCOMPARE(config.get_log_dir(), data.path());
        QCOMPARE(config.get_clipboard_db_path(), QDir(data.path()).filePath("history/clipboard.db"));
        QVERIFY(!config.get_autostart_enabled());
        QCOMPARE(config.get_hotkey(), config.app_defaults().value("hotkey").toString());
    }

    void olderStoresAreTheRegistry() {
        const auto installed = data_folder::sources_for({"C:/Users/u/AppData/Roaming/Jietuba", false});
        QCOMPARE(installed.size(), 1);
        QCOMPARE(installed[0].settings, QString("HKEY_CURRENT_USER\\Software\\Jietuba\\ToolSettings"));
        QCOMPARE(installed[0].clipboard_window, QString("HKEY_CURRENT_USER\\Software\\Jietuba\\ClipboardWindow"));
        QCOMPARE(installed[0].migrated, QString("HKEY_CURRENT_USER\\Software\\Jietuba\\Migrated"));
        // A settings file beside the program, even empty, is used as it is.
        QVERIFY(data_folder::sources_for({"D:/tools/jietuba", true}).isEmpty());
        // Tests never read the user's registry.
        QVERIFY(data_folder::default_sources(data_location()).isEmpty());
    }

    // Every setting written to the registry as older versions did reads the same from the file.
    void registrySettingsReadAlikeFromTheFile() {
        QTemporaryDir data, old;
        const QString tool_path = registry_path("Alike\\ToolSettings"), window_path = registry_path("Alike\\ClipboardWindow");
        QSettings registry(tool_path, QSettings::NativeFormat);
        {
            ToolSettingsManager writer(registry);
            for (const auto &setting : APP_SETTINGS()) {
                const QVariant value = changed(writer.app_defaults().value(setting.name));
                if (value.isValid()) registry.setValue(setting.path, value);
            }
            for (auto tool = writer.tool_defaults().begin(); tool != writer.tool_defaults().end(); ++tool) {
                const auto defaults = tool.value().toMap();
                for (auto it = defaults.begin(); it != defaults.end(); ++it) writer.set_setting(tool.key(), it.key(), changed(it.value()), false);
            }
            writer.save_all();
        }
        // Keys outside the table, as their writers store them.
        registry.setValue("translation/usage/2026-10/deepl", 1234);
        registry.setValue("inapp/inapp_confirm", "ctrl+enter");
        registry.setValue("app/ocr_result_last_center", "10,20");
        registry.setValue("app/save_dialog_path_tags", QStringList{"only"});
        registry.setValue("clipboard/db_path", old.filePath("custom.db"));
        registry.sync();
        {
            QSettings window(window_path, QSettings::NativeFormat);
            window.setValue("window/x", 10);
            window.setValue("window/width", 500);
        }
        const DataLocation target{data.path(), false};
        data_folder::migrate(target, {{registry_path("Alike\\Missing"), {}, {}}, {tool_path, window_path, {}}});

        QSettings file(target.settings_file(), QSettings::IniFormat);
        const QStringList keys = registry.allKeys();
        QVERIFY(keys.size() > 300);
        for (const auto &key : keys) {
            if (LOCAL_SETTING_PATHS().contains(key)) QVERIFY2(!file.contains(key), qPrintable(key));
            else QVERIFY2(text_of(file.value(key)) == text_of(registry.value(key)), qPrintable(key));
        }
        // With the Local settings read from the registry, as the program does.
        ToolSettingsManager from_registry(registry), from_file(file, &registry);
        for (const auto &setting : APP_SETTINGS()) {
            QVERIFY2(from_file.get_app_setting(setting.name) == from_registry.get_app_setting(setting.name), setting.name);
            QVERIFY2(from_file.accessor_value(setting.name) == from_registry.accessor_value(setting.name), setting.name);
        }
        for (auto tool = from_registry.tool_defaults().begin(); tool != from_registry.tool_defaults().end(); ++tool) {
            for (const auto &key : tool.value().toMap().keys()) {
                const QVariant expected = from_registry.get_setting(tool.key(), key);
                QVERIFY2(expected != from_registry.tool_defaults().value(tool.key()).toMap().value(key), qPrintable(tool.key() + "/" + key));
                QVERIFY2(from_file.get_setting(tool.key(), key) == expected, qPrintable(tool.key() + "/" + key));
            }
        }
        QCOMPARE(file.value(CLIPBOARD_WINDOW_SETTINGS_GROUP + "/window/x").toInt(), 10);
        QCOMPARE(file.value(CLIPBOARD_WINDOW_SETTINGS_GROUP + "/window/width").toInt(), 500);
        // Copied, not moved; a missing source is skipped without being created.
        QCOMPARE(registry.value("inapp/inapp_confirm").toString(), QString("ctrl+enter"));
        QVERIFY(registry_key_exists(registry_ + "\\Alike\\ClipboardWindow"));
        QVERIFY(!registry_key_exists(registry_ + "\\Alike\\Missing"));
        QVERIFY(data_folder::deferred_log_text().join('\n').contains(QDir::toNativeSeparators(target.settings_file())));
    }

    // Once copied, the registry is not taken in again: a settings file deleted later starts from defaults.
    void theRegistryIsMigratedOnce() {
        QTemporaryDir data;
        const QString tool_path = registry_path("Once\\ToolSettings"), marker = registry_path("Once\\Migrated");
        QSettings(tool_path, QSettings::NativeFormat).setValue("app/hotkey", "ctrl+9");
        QSettings(tool_path, QSettings::NativeFormat).setValue("app/log_dir", data.path());
        const DataLocation target{data.path(), false};
        const Source source{tool_path, {}, marker};
        data_folder::migrate(target, {source});
        QCOMPARE(QSettings(target.settings_file(), QSettings::IniFormat).value("app/hotkey").toString(), QString("ctrl+9"));
        QVERIFY(!QSettings(target.settings_file(), QSettings::IniFormat).contains("app/log_dir"));
        QVERIFY(registry_key_exists(registry_ + "\\Once\\Migrated"));
        QCOMPARE(QSettings(marker, QSettings::NativeFormat).value("settings_file").toString(), QDir::toNativeSeparators(target.settings_file()));
        QVERIFY(!QFileInfo::exists(target.settings_file() + ".migrating"));

        QVERIFY(QFile::remove(target.settings_file()));
        data_folder::write_deferred_log();
        data_folder::migrate(target, {source});
        QVERIFY(QSettings(target.settings_file(), QSettings::IniFormat).allKeys().isEmpty());
        QVERIFY(data_folder::deferred_log_text().join('\n').contains(tool_path));
        // The old settings stay for older versions, the Local ones for this one too.
        QCOMPARE(QSettings(tool_path, QSettings::NativeFormat).value("app/hotkey").toString(), QString("ctrl+9"));
        QCOMPARE(QSettings(tool_path, QSettings::NativeFormat).value("app/log_dir").toString(), data.path());
    }

    // A registry holding only settings bound to this computer has nothing to copy.
    void onlyLocalSettingsAreNotMigrated() {
        QTemporaryDir data;
        const QString tool_path = registry_path("LocalOnly\\ToolSettings"), marker = registry_path("LocalOnly\\Migrated");
        QSettings(tool_path, QSettings::NativeFormat).setValue("app/has_run_before", true);
        const DataLocation target{data.path(), false};
        data_folder::migrate(target, {{tool_path, {}, marker}});
        QVERIFY(QSettings(target.settings_file(), QSettings::IniFormat).allKeys().isEmpty());
        QVERIFY(!registry_key_exists(registry_ + "\\LocalOnly\\Migrated"));
    }

    void settingsAlreadyThereAreKept() {
        QTemporaryDir data, old;
        const DataLocation target{data.path(), false};
        {
            QSettings settings(target.settings_file(), QSettings::IniFormat);
            settings.setValue("app/hotkey", "ctrl+1");
        }
        data_folder::migrate(target, {file_source(QDir(old.path()), {{"app/hotkey", "ctrl+9"}, {"app/language", "ja"}})});
        QSettings settings(target.settings_file(), QSettings::IniFormat);
        QCOMPARE(settings.allKeys(), QStringList{"app/hotkey"});
        QCOMPARE(settings.value("app/hotkey").toString(), QString("ctrl+1"));
    }

    // A start that finds another one migrating waits for it and then leaves the filled file alone.
    void aSecondStartWaitsForTheFirst() {
        QTemporaryDir data, old;
        const DataLocation target{data.path(), false};
        const auto source = file_source(QDir(old.path()), {{"app/hotkey", "ctrl+9"}});
        QLockFile first(target.settings_file() + ".migrating");
        QVERIFY(first.lock());
        std::thread second([&] { data_folder::migrate(target, {source}); });
        QTest::qWait(300);
        {
            QSettings filled(target.settings_file(), QSettings::IniFormat);
            filled.setValue("app/hotkey", "ctrl+1");
        }
        first.unlock();
        second.join();
        QSettings settings(target.settings_file(), QSettings::IniFormat);
        QCOMPARE(settings.allKeys(), QStringList{"app/hotkey"});
        QCOMPARE(settings.value("app/hotkey").toString(), QString("ctrl+1"));
    }

    void anUnwritableSettingsFileIsLogged() {
        QTemporaryDir data, old;
        const DataLocation target{data.path(), false};
        QVERIFY(QDir(data.path()).mkdir("settings.ini"));
        data_folder::migrate(target, {file_source(QDir(old.path()), {{"app/hotkey", "ctrl+9"}})});
        QVERIFY(data_folder::deferred_log_text().join('\n').contains(QDir::toNativeSeparators(target.settings_file())));
    }

    void startupEntriesFollowTheSwitch() {
        QTemporaryDir dir;
        const QString program = QDir(dir.path()).filePath("new/jietuba.exe"), other = QDir(dir.path()).filePath("other.exe");
        write_file(other, "exe");
        const QString gone = QDir(dir.path()).filePath("gone/jietuba.exe");
        using Fix = std::pair<bool, std::optional<bool>>;
        const auto fix = [&program](bool autostart, bool admin, std::optional<QString> run, std::optional<QString> task) {
            const auto result = data_folder::fix_startup(autostart, admin, program, {std::move(run), std::move(task)});
            return Fix{result.run_as_admin, result.run_entry};
        };
        // On: a missing or dangling entry runs this program.
        QCOMPARE(fix(true, false, std::nullopt, std::nullopt), Fix(false, true));
        QCOMPARE(fix(true, false, gone, std::nullopt), Fix(false, true));
        QCOMPARE(fix(true, false, QDir::toNativeSeparators(program).toUpper(), std::nullopt), Fix(false, std::nullopt));
        // Another copy still on disk keeps its entry; so does one that could not be read.
        QCOMPARE(fix(true, false, other, std::nullopt), Fix(false, std::nullopt));
        QCOMPARE(fix(true, false, QString(), std::nullopt), Fix(false, std::nullopt));
        // Off: only this program's own entry goes.
        QCOMPARE(fix(false, false, QDir::toNativeSeparators(program), std::nullopt), Fix(false, false));
        for (const auto &run : {std::optional<QString>(), std::optional<QString>(other), std::optional<QString>(gone)})
            QCOMPARE(fix(false, false, run, std::nullopt), Fix(false, std::nullopt));
        QCOMPARE(fix(false, true, QDir::toNativeSeparators(program), std::nullopt), Fix(true, false));
        // No task: admin mode goes off and the Run entry starts it instead.
        QCOMPARE(fix(true, true, std::nullopt, std::nullopt), Fix(false, true));
        QCOMPARE(fix(true, true, other, std::nullopt), Fix(false, std::nullopt));
        for (const auto &task : {program, other, gone, QString()}) QCOMPARE(fix(true, true, std::nullopt, task), Fix(true, std::nullopt));
    }

    // Every start the entries follow the switch, whatever changed either of them since.
    void autostartFollowsTheSwitchEveryStart() {
        QTemporaryDir dir;
        QSettings qs(QDir(dir.path()).filePath("settings.ini"), QSettings::IniFormat);
        ToolSettingsManager config(qs);
        const QString program = QDir::toNativeSeparators(QDir(dir.path()).filePath("jietuba.exe"));
        StartupEntries entries;
        QList<bool> reads, writes;
        const auto read = [&](bool admin) {
            reads.append(admin);
            return entries;
        };
        const auto write = [&](bool enabled) {
            writes.append(enabled);
            entries.run_entry = enabled ? std::optional<QString>(program) : std::nullopt;
        };
        // A switch the wizard has not stored yet leaves the startup entries alone.
        data_folder::align_startup(config, program, read, write);
        QVERIFY(reads.isEmpty());
        QVERIFY(writes.isEmpty());
        config.set_autostart_enabled(true);
        data_folder::align_startup(config, program, read, write);
        QCOMPARE(writes, QList<bool>{true});
        data_folder::align_startup(config, program, read, write);
        QCOMPARE(writes.size(), 1);
        // Switched off: this program's entry goes.
        config.set_autostart_enabled(false);
        data_folder::align_startup(config, program, read, write);
        QCOMPARE(writes, (QList<bool>{true, false}));
        // Admin mode without its task falls back to the Run entry.
        config.set_autostart_enabled(true);
        config.set_run_as_admin(true);
        data_folder::align_startup(config, program, read, write);
        QVERIFY(!config.get_run_as_admin());
        QCOMPARE(writes, (QList<bool>{true, false, true}));
        QCOMPARE(reads, (QList<bool>{false, false, false, true}));
    }
};

QTEST_GUILESS_MAIN(DataFolderTests)
#include "test_data_folder.moc"
