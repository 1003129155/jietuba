#include "clipboard/core/manager.h"
#include "core/bootstrap.h"
#include "core/constants.h"
#include "core/elevation.h"
#include "core/global_hotkey.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "settings/tool_settings.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <stdexcept>
#include <windows.h>

using namespace jietuba;

namespace {
const QString EXE = R"(C:\Apps\Jietuba & Co\jietuba_lite.exe)";
const QString USER = R"(PC\user)";
// A program that is not on disk.
const QString GONE = R"(C:\Gone\jietuba_lite.exe)";

// Records schtasks calls; /Query answers from command (nullopt = no task). Calls through UAC are
// recorded in elevated_calls as well and answer elevated_code when set (-1: UAC cancelled).
struct FakeSchtasks {
    std::optional<QString> command;
    QMap<QString, int> codes;
    QStringList calls;
    QStringList elevated_calls;
    std::optional<int> elevated_code;
    QByteArray created_xml;
    std::function<void()> on_run;

    std::pair<int, QByteArray> elevated(const QStringList &args) {
        elevated_calls << args.value(0);
        if (elevated_code) return {*elevated_code, {}};
        return (*this)(args);
    }

    std::pair<int, QByteArray> operator()(const QStringList &args) {
        const QString verb = args.value(0);
        calls << verb;
        if (codes.contains(verb)) return {codes.value(verb), {}};
        if (verb == "/Query") {
            if (!command) return {1, {}};
            // Like the real output: UTF-8 bytes and CR CR LF line ends under a UTF-16 declaration.
            return {0, elevation::build_task_xml(*command, USER).replace("\n", "\r\r\n").toUtf8()};
        }
        if (verb == "/Create") {
            // Opened like schtasks does: sharing reads only, so a writer still holding the file fails here.
            const std::wstring path = args.value(args.indexOf("/XML") + 1).toStdWString();
            HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (handle == INVALID_HANDLE_VALUE) return {1, {}};
            char buffer[8192];
            DWORD read = 0;
            while (ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) && read > 0) created_xml.append(buffer, static_cast<qsizetype>(read));
            CloseHandle(handle);
            command = EXE;
        }
        if (verb == "/Delete") {
            if (!command) return {1, {}};
            command.reset();
        }
        if (verb == "/Run" && on_run) on_run();
        return {0, {}};
    }
};

// A fake schtasks and environment for one test; the real ones come back afterwards.
struct ElevationFixture {
    std::shared_ptr<FakeSchtasks> fake = std::make_shared<FakeSchtasks>();
    explicit ElevationFixture(const QString &state) {
        elevation::set_schtasks_runner_for_test([fake = fake](const QStringList &args) { return (*fake)(args); });
        elevation::set_elevated_schtasks_runner_for_test([fake = fake](const QStringList &args) { return fake->elevated(args); });
        elevation::set_environment_for_test(EXE, state, USER);
    }
    ~ElevationFixture() {
        elevation::set_schtasks_runner_for_test({});
        elevation::set_elevated_schtasks_runner_for_test({});
        elevation::set_environment_for_test(std::nullopt, std::nullopt, std::nullopt);
        elevation::set_self_launcher_for_test({});
    }
};

void write_record(const QString &path, const QByteArray &content) {
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write(content);
}
}

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

    void elevationTaskXml() {
        const auto cases = fixtures_["elevation_task_xml"].toArray();
        QVERIFY(!cases.isEmpty());
        for (const auto &value : cases) {
            const auto c = value.toObject();
            QCOMPARE(elevation::build_task_xml(c["command"].toString(), c["user"].toString()), c["expected"].toString());
        }
    }
    void elevationInstallWritesUtf16WithBom() {
        ElevationFixture fixture(elevation::ELEVATED);
        QVERIFY(elevation::install_task());
        const QByteArray xml = fixture.fake->created_xml;
        QVERIFY(xml.startsWith(QByteArray("\xff\xfe", 2)));
        QCOMPARE(QString::fromUtf16(reinterpret_cast<const char16_t *>(xml.constData() + 2), (xml.size() - 2) / 2), elevation::build_task_xml(EXE, USER));
        QCOMPARE(elevation::task_name(), QString("Jietuba RunAsAdmin (user)"));
    }
    void elevationQueryAndSelfMatch() {
        ElevationFixture fixture(elevation::ELEVATED);
        QVERIFY(!elevation::query_task_command());
        fixture.fake->command = EXE;
        QCOMPARE(elevation::query_task_command().value_or(QString()), EXE);
        QVERIFY(elevation::points_to_self(QString(EXE).toUpper().replace('\\', '/')));
        QVERIFY(!elevation::points_to_self(QString(R"(C:\Other\jietuba_lite.exe)")));
        QVERIFY(!elevation::points_to_self(std::nullopt));
        QVERIFY(!elevation::points_to_self(QString()));
    }
    void elevationRemovingAMissingTaskSucceedsWithoutAsking() {
        ElevationFixture fixture(elevation::CAN_ELEVATE);
        QVERIFY(elevation::remove_task());
        QVERIFY(fixture.fake->elevated_calls.isEmpty());
        QVERIFY(!fixture.fake->calls.contains("/Delete"));
        fixture.fake->command = EXE;
        fixture.fake->codes["/Delete"] = 1;
        QVERIFY(!elevation::remove_task());
    }
    void elevationOnlyUnelevatedChangesAskForUac() {
        for (const auto &[state, elevated] : {std::pair{elevation::ELEVATED, false}, std::pair{elevation::CAN_ELEVATE, true}}) {
            ElevationFixture fixture(state);
            QVERIFY(elevation::install_task());
            QVERIFY(elevation::remove_task());
            QCOMPARE(fixture.fake->elevated_calls == QStringList({"/Create", "/Delete"}), elevated);
            QVERIFY(elevation::run_task());
            QVERIFY(!fixture.fake->elevated_calls.contains("/Run"));
        }
    }
    // The return value is whether admin mode is in effect.
    void elevationApplyStartup() {
        {
            ElevationFixture fixture(elevation::ELEVATED);
            QVERIFY(elevation::apply_startup(true, true, false));
            QCOMPARE(fixture.fake->command.value_or(QString()), EXE);
            QVERIFY(!elevation::apply_startup(false, false, true));
            QVERIFY(!fixture.fake->command);
        }
        {
            ElevationFixture fixture(elevation::ELEVATED);
            fixture.fake->codes["/Create"] = 1;
            QVERIFY(!elevation::apply_startup(true, true, false));
        }
        {
            ElevationFixture fixture(elevation::ELEVATED);
            QVERIFY(!elevation::apply_startup(true, false, false));
            QVERIFY(fixture.fake->calls.isEmpty());
        }
        {
            // The welcome wizard without elevation leaves the task alone.
            ElevationFixture fixture(elevation::CAN_ELEVATE);
            QVERIFY(elevation::apply_startup(true, true, true));
            QVERIFY(fixture.fake->calls.isEmpty());
        }
        {
            ElevationFixture fixture(elevation::CAN_ELEVATE);
            QVERIFY(elevation::apply_startup(true, true, false, true));
            QCOMPARE(fixture.fake->elevated_calls, QStringList{"/Create"});
        }
        {
            ElevationFixture fixture(elevation::CAN_ELEVATE);
            fixture.fake->elevated_code = -1;
            QVERIFY(!elevation::apply_startup(true, true, false, true));
        }
        {
            // A task that could not be removed still counts as in effect.
            ElevationFixture fixture(elevation::CAN_ELEVATE);
            fixture.fake->command = EXE;
            fixture.fake->elevated_code = -1;
            QVERIFY(elevation::apply_startup(false, false, true, true));
            QVERIFY(fixture.fake->command);
        }
    }
    // Applying again with the task already running this program changes nothing and asks nothing.
    void elevationApplyStartupLeavesAWorkingTask() {
        ElevationFixture fixture(elevation::CAN_ELEVATE);
        fixture.fake->command = EXE;
        QVERIFY(elevation::apply_startup(true, true, true, true));
        QVERIFY(fixture.fake->elevated_calls.isEmpty());
        // A task for a program that is gone is written again, through UAC.
        fixture.fake->command = GONE;
        QVERIFY(elevation::apply_startup(true, true, true, true));
        QCOMPARE(fixture.fake->elevated_calls, QStringList{"/Create"});
        QCOMPARE(fixture.fake->command.value_or(QString()), EXE);
    }
    void elevationStaleTaskNeedsAnotherMissingProgram() {
        ElevationFixture fixture(elevation::CAN_ELEVATE);
        QVERIFY(elevation::task_is_stale(GONE));
        QVERIFY(!elevation::task_is_stale(std::nullopt));
        QVERIFY(!elevation::task_is_stale(QString()));
        // This program is never stale, and neither is another one that exists.
        QVERIFY(!elevation::task_is_stale(EXE));
        QVERIFY(!elevation::task_is_stale(QCoreApplication::applicationFilePath()));
    }
    void elevationRepairPointsAStaleTaskAtThisProgram() {
        {
            ElevationFixture fixture(elevation::ELEVATED);
            fixture.fake->command = GONE;
            QVERIFY(elevation::repair_task());
            QCOMPARE(fixture.fake->command.value_or(QString()), EXE);
        }
        QTemporaryDir dir;
        QFile other(dir.filePath("other.exe"));
        QVERIFY(other.open(QIODevice::WriteOnly));
        other.close();
        // Left alone: no task, a task for this program, a task for another copy that still exists.
        for (const auto &command : {std::optional<QString>(), std::optional<QString>(EXE), std::optional<QString>(other.fileName())}) {
            ElevationFixture fixture(elevation::ELEVATED);
            fixture.fake->command = command;
            QVERIFY(!elevation::repair_task());
            QVERIFY(!fixture.fake->calls.contains("/Create"));
        }
        // An instance that would need UAC to change the task leaves it too.
        ElevationFixture fixture(elevation::CAN_ELEVATE);
        fixture.fake->command = GONE;
        QVERIFY(!elevation::repair_task());
        QVERIFY(fixture.fake->calls.isEmpty());
        QVERIFY(fixture.fake->elevated_calls.isEmpty());
    }
    void elevationHandOff() {
        QTemporaryDir dir;
        const QString record = dir.filePath("jietuba_lite.exe.pid");
        {
            ElevationFixture fixture(elevation::CAN_ELEVATE);
            fixture.fake->command = EXE;
            write_record(record, "111\n5\n");
            fixture.fake->on_run = [&record] { write_record(record, "222\n9\n"); };
            QVERIFY(hand_off_to_admin_task(10, 200, record));
        }
        {
            ElevationFixture fixture(elevation::CAN_ELEVATE);
            fixture.fake->command = EXE;
            write_record(record, "111\n5\n");
            QVERIFY(!hand_off_to_admin_task(10, 200, record));
            QVERIFY(fixture.fake->calls.contains("/Run"));
            fixture.fake->codes["/Run"] = 1;
            QVERIFY(!hand_off_to_admin_task(10, 200, record));
        }
        for (const QString &state : {elevation::ELEVATED, elevation::STANDARD}) {
            ElevationFixture fixture(state);
            fixture.fake->command = EXE;
            QVERIFY(!hand_off_to_admin_task(10, 200, record));
            QVERIFY(fixture.fake->calls.isEmpty());
        }
        {
            // The task's program is gone: this program starts itself elevated through UAC instead.
            ElevationFixture fixture(elevation::CAN_ELEVATE);
            fixture.fake->command = GONE;
            write_record(record, "111\n5\n");
            int launched = 0;
            elevation::set_self_launcher_for_test([&] {
                ++launched;
                write_record(record, "222\n9\n");
                return true;
            });
            QVERIFY(hand_off_to_admin_task(10, 200, record));
            QCOMPARE(launched, 1);
            QVERIFY(!fixture.fake->calls.contains("/Run"));
            // Declined at the UAC prompt: start normally.
            elevation::set_self_launcher_for_test([] { return false; });
            QVERIFY(!hand_off_to_admin_task(10, 200, record));
        }
        // No task, or one for another copy that still exists: nothing to hand off to.
        QFile other(dir.filePath("other.exe"));
        QVERIFY(other.open(QIODevice::WriteOnly));
        other.close();
        for (const auto &command : {std::optional<QString>(), std::optional<QString>(other.fileName())}) {
            ElevationFixture fixture(elevation::CAN_ELEVATE);
            int launched = 0;
            elevation::set_self_launcher_for_test([&launched] {
                ++launched;
                return true;
            });
            fixture.fake->command = command;
            QVERIFY(!hand_off_to_admin_task(10, 200, record));
            QVERIFY(!fixture.fake->calls.contains("/Run"));
            QCOMPARE(launched, 0);
        }
    }

    // Not from the reference: the test executables' defaults must land in the sandbox
    // (tests/test_sandbox.cpp), never in the user's data folder.
    void testDefaultsStayInTheSandbox() {
        const QString sandbox = QDir::toNativeSeparators(test_sandbox_dir());
        QVERIFY(!sandbox.isEmpty());
        QVERIFY(QDir::toNativeSeparators(get_app_data_dir()).startsWith(sandbox));
        QVERIFY(QDir::toNativeSeparators(clipboard::ClipboardManager::default_db_path()).startsWith(sandbox));
        auto &store = get_tool_settings_manager()->qsettings.settings();
        QCOMPARE(store.format(), QSettings::IniFormat);
        QVERIFY(QDir::toNativeSeparators(store.fileName()).startsWith(sandbox));
    }
private:
    static void set_language(const QString &language) {
        // load_language also installs catalogs; the logger only reads the current code.
        I18nManager::load_language(language);
    }
};

QTEST_MAIN(CoreTests)
#include "test_core.moc"
