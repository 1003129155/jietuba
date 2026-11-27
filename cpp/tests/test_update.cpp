#include "core/auto_update.h"
#include "core/package_identity.h"
#include "core/update_cache.h"
#include "core/update_checker.h"
#include "core/update_controller.h"
#include "core/updater_process.h"
#include "main_app.h"
#include "settings/tool_settings.h"
#include "ui/update_dialog.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <windows.h>

using namespace jietuba;

// Ported from the original's test_update_checker.py and test_update_cache.py. Checks the
// original makes with monkeypatched helpers run here against tests/fake_updater.cpp.
namespace {
const QString FAKE_HELPER = QStringLiteral(JIETUBA_FAKE_UPDATER_PATH);
const QString ID = QString(32, '1');

// Back-dates a file or directory past the cache's 7-day limit.
void make_old(const QString &path) {
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()), FILE_WRITE_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    QVERIFY(handle != INVALID_HANDLE_VALUE);
    ULARGE_INTEGER value;
    GetSystemTimeAsFileTime(reinterpret_cast<FILETIME *>(&value));
    value.QuadPart -= 8ULL * 86400 * 10000000;
    FILETIME old{value.LowPart, value.HighPart};
    SetFileTime(handle, nullptr, &old, &old);
    CloseHandle(handle);
}

QString directory(const QString &root, const QString &name, const QMap<QString, QByteArray> &files, bool old = false) {
    const QString path = QDir(root).filePath(name);
    QDir().mkpath(path);
    for (auto it = files.begin(); it != files.end(); ++it) {
        QFile file(QDir(path).filePath(it.key()));
        if (file.open(QIODevice::WriteOnly)) file.write(it.value());
    }
    if (old) {
        for (const auto &entry : QDir(path).entryInfoList(QDir::Files)) make_old(entry.absoluteFilePath());
        make_old(path);
    }
    return path;
}

ReleaseInfo release() {
    return {"release-99.0.0", "new", "notes", "https://example.com", true, "release.zip", "x64",
            QJsonObject{{"urls", QJsonArray{"https://example.com/release.zip"}}, {"size", 100}}};
}

class FakeRunner : public UpdaterProcess {
public:
    bool fake_busy = false;
    QList<QPair<QString, QStringList>> calls;
    QStringList sent;
    bool busy() const override { return fake_busy; }
    bool start(const QString &command, const QStringList &arguments, int) override {
        if (fake_busy) return false;
        calls.append({command, arguments});
        fake_busy = true;
        return true;
    }
    bool send(const QString &command) override {
        sent << command;
        return true;
    }
    void cancel() override { sent << "cancel"; }
    void finish() {
        fake_busy = false;
        emit finished();
    }
};

class FakeHost : public UpdateHost {
public:
    QString reason;
    std::pair<bool, QString> prepare_result{true, {}};
    int quit_count = 0;
    int cancel_count = 0;
    QString update_busy_reason() override { return reason; }
    std::pair<bool, QString> prepare_for_update() override { return prepare_result; }
    void cancel_update_preparation() override { ++cancel_count; }
    bool quit_app() override {
        ++quit_count;
        return true;
    }
};

QMap<QString, QString> options(const QStringList &arguments) {
    QMap<QString, QString> result;
    for (qsizetype i = 0; i + 1 < arguments.size(); i += 2) result[arguments.at(i)] = arguments.at(i + 1);
    return result;
}

// A finished download as the helper leaves it in the cache.
QString staged(const QString &root, const QString &id, const QString &executable, const QString &version, const QString &tag) {
    const QJsonObject record{{"tag_name", tag}, {"title", tag}, {"notes", ""}, {"url", "https://example.com"}, {"asset_name", "jietuba_lite-" + tag + "-x64.zip"},
                             {"arch", "x64"}, {"artifact", QJsonObject{{"urls", QJsonArray{"https://example.com/package.zip"}}}}};
    const QJsonObject prepared{{"executable", executable}, {"current_version", version}, {"release", record}, {"staged_hash", "hash"}};
    return directory(root, id, {{"app.exe", "MZ"}, {"prepared.json", QJsonDocument(prepared).toJson()}});
}

QByteArray days_ago(int days) { return QDateTime::currentDateTimeUtc().addDays(-days).toString(Qt::ISODate).toUtf8(); }
}

class UpdateTests : public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> local_;
    QString cache_;
    FakeHost host_;
    FakeRunner *runner_ = nullptr;
    UpdateController *controller_ = nullptr;
    std::unique_ptr<QSettings> settings_;
    std::unique_ptr<ToolSettingsManager> config_;

    // A controller with no window yet, as automatic updates use it.
    void use_background_controller() {
        host_ = FakeHost();
        runner_ = new FakeRunner;
        controller_ = new UpdateController(&host_, nullptr, runner_);
    }

    void use_controller() {
        use_background_controller();
        controller_->present(release());
    }

    void use_settings() {
        settings_ = std::make_unique<QSettings>(QDir(local_->path()).filePath("settings.ini"), QSettings::IniFormat);
        config_ = std::make_unique<ToolSettingsManager>(*settings_);
    }

    void downloaded() {
        emit runner_->event_received("downloaded", ID, {});
        runner_->finish();
    }

private slots:
    void init() {
        local_ = std::make_unique<QTemporaryDir>();
        qputenv("LOCALAPPDATA", QDir::toNativeSeparators(local_->path()).toLocal8Bit());
        cache_ = update_cache::cache_root();
        set_helper_source_for_test(FAKE_HELPER);
    }

    void cleanup() {
        if (controller_) {
            if (controller_->dialog) controller_->dialog->committed = false;
            delete controller_;
            controller_ = nullptr;
        }
        config_.reset();
        settings_.reset();
        set_helper_source_for_test({});
        qunsetenv("JIETUBA_FAKE_UPDATER");
        qunsetenv("JIETUBA_FAKE_UPDATER_ARGS");
        qunsetenv("JIETUBA_FAKE_PUBLISHED");
        update_cache::reset_sessions_for_test();
        local_.reset();
    }

    void helpersCannotOpenMissingDllDialogs() {
        // The helpers below are copied away from the Qt DLLs; one that cannot load them must fail,
        // not leave a system dialog on the user's screen. They inherit this process's error mode.
        QVERIFY(GetErrorMode() & SEM_FAILCRITICALERRORS);
    }

    void comparableVersionAcceptsCommonReleaseTags_data() {
        QTest::addColumn<QString>("value");
        QTest::addColumn<QList<int>>("expected");
        QTest::newRow("plain") << "2.0.3" << QList<int>{2, 0, 3, 0};
        QTest::newRow("v") << "v2.1.0" << QList<int>{2, 1, 0, 0};
        QTest::newRow("release-") << "release-2.4" << QList<int>{2, 4, 0, 0};
        QTest::newRow("release/") << "release/3" << QList<int>{3, 0, 0, 0};
    }
    void comparableVersionAcceptsCommonReleaseTags() {
        QFETCH(QString, value);
        QFETCH(QList<int>, expected);
        const auto version = comparable_version(value);
        QCOMPARE((QList<int>{version[0], version[1], version[2], version[3]}), expected);
    }

    void versionComparisonIsNumericInsteadOfLexicographic() {
        QVERIFY(is_newer_version("v2.0.10", "2.0.9"));
        QVERIFY(!is_newer_version("release-2.0", "2.0.3"));
        QVERIFY(!is_newer_version("v2.0.3", "2.0.3"));
    }

    void parseReleasePayloadExtractsNotesAndUsesLatestDownloadUrl() {
        const QJsonObject payload{{"tag_name", "v2.1.0"}, {"name", "Version 2.1"}, {"body", "  Added update checking.  "}, {"html_url", "https://example.invalid/untrusted"}};
        const auto info = parse_release_payload(QJsonDocument(payload).toJson());
        QCOMPARE(info.tag_name, QString("v2.1.0"));
        QCOMPARE(info.title, QString("Version 2.1"));
        QCOMPARE(info.notes, QString("Added update checking."));
        QCOMPARE(info.url, QString("https://github.com/1003129155/jietuba/releases/latest"));
    }

    void parseReleasePayloadRejectsUnusableResponses() {
        for (const QByteArray payload : {QByteArray("not-json"), QByteArray("[]"), QByteArray("{}"), QByteArray(R"({"tag_name": "latest"})")})
            QVERIFY_THROWS_EXCEPTION(UpdateCheckError, parse_release_payload(payload));
    }

    void checkOutputGivesTheReleaseOrTheHelperError() {
        const QByteArray available = R"({"protocol":1,"event":"available","data":{"tag_name":"v99.0.0","title":"v99.0.0","notes":"Bug fixes",)"
                                     R"("url":"u","asset_name":"package.zip","arch":"x64","artifact":{"urls":["https://example.com/package.zip"],"size":123}}})";
        const auto info = release_from_check_output(available, 0);
        QVERIFY(info.available == true);
        QCOMPARE(info.asset_name, QString("package.zip"));
        QVERIFY_THROWS_EXCEPTION(UpdateCheckError, release_from_check_output(available, 1));
        try {
            release_from_check_output(R"({"protocol":1,"event":"error","data":{"message":"network unavailable"}})", 1);
            QFAIL("no error");
        } catch (const UpdateCheckError &e) {
            QCOMPARE(QString(e.what()), QString("network unavailable"));
        }
        for (const QByteArray reply : {QByteArray("not-json"), QByteArray("{}"), QByteArray(R"({"protocol":2})"),
                                       QByteArray(R"({"protocol":1,"event":"available","data":{}})")})
            QVERIFY_THROWS_EXCEPTION(UpdateCheckError, release_from_check_output(reply, 0));
    }

    void checkerRunsInBackgroundRejectsOverlapsAndBuildsTheRequest() {
        qputenv("JIETUBA_FAKE_UPDATER", "check-available");
        const QString record = QDir(local_->path()).filePath("args.txt");
        qputenv("JIETUBA_FAKE_UPDATER_ARGS", QDir::toNativeSeparators(record).toLocal8Bit());
        GitHubReleaseChecker checker(nullptr, 4321);
        QSignalSpy found(&checker, &GitHubReleaseChecker::release_found);
        QVERIFY(checker.check());
        QVERIFY(checker.is_checking());
        QVERIFY(!checker.check());
        QTRY_COMPARE_WITH_TIMEOUT(found.count(), 1, 10000);
        QCOMPARE(found.first().first().value<ReleaseInfo>().tag_name, QString("v99.0.0"));
        QVERIFY(!checker.is_checking());
        QFile file(record);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QStringList args = QString::fromUtf8(file.readAll()).split('\n');
        QCOMPARE(args.first(), QString("check"));
        QVERIFY(args.contains("--install-exe") && args.contains("--current-version"));
        QCOMPARE(args.at(args.indexOf("--variant") + 1), app_variant());
        // The helper copy is removed after the check.
        QVERIFY(QDir(QDir(cache_).filePath("helpers")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
    }

    void checkerReportsTheHelperErrorAndAMissingHelper() {
        qputenv("JIETUBA_FAKE_UPDATER", "check-error");
        GitHubReleaseChecker checker;
        QSignalSpy failed(&checker, &GitHubReleaseChecker::failed);
        QVERIFY(checker.check());
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        QCOMPARE(failed.first().first().toString(), QString("network unavailable"));
        set_helper_source_for_test(QDir(local_->path()).filePath("missing.exe"));
        QVERIFY(checker.check());
        QTRY_COMPARE(failed.count(), 2);
    }

    void downloadProgressDuplicateClickAndHandoff() {
        use_controller();
        QCOMPARE(controller_->dialog->windowModality(), Qt::NonModal);
        controller_->start();
        controller_->start();
        QCOMPARE(runner_->calls.size(), 1);
        emit runner_->event_received("progress", ID, {{"received", 50}, {"total", 100}});
        QCOMPARE(controller_->dialog->progress->value(), 50);
        downloaded();
        const auto [command, arguments] = runner_->calls.last();
        const auto values = options(arguments);
        QCOMPARE(command, QString("apply"));
        QCOMPARE(values["--transaction"], ID);
        QCOMPARE(values["--parent-pid"], QString::number(QCoreApplication::applicationPid()));
        QCOMPARE(values["--variant"], app_variant());
        QVERIFY(values["--failure-message"].startsWith("The update was not completed"));
        emit runner_->event_received("ready", ID, {});
        QCOMPARE(runner_->sent, QStringList{"go"});
        QVERIFY(controller_->dialog->committed);
        QCOMPARE(controller_->dialog->windowModality(), Qt::ApplicationModal);
        controller_->cancel();
        QCOMPARE(runner_->sent, QStringList{"go"});
        emit runner_->event_received("handed_off", ID, {});
        runner_->finish();
        QCOMPARE(host_.quit_count, 1);
        QVERIFY(!controller_->dialog || !controller_->dialog->isVisible());
    }

    void busyKeepsDownloadAndRetriesInstallOnly() {
        use_controller();
        controller_->start();
        host_.reason = "finish work";
        downloaded();
        QVERIFY(!controller_->downloaded.isEmpty());
        QCOMPARE(runner_->calls.size(), 1);
        QVERIFY(controller_->dialog->status->text().endsWith("finish work"));
        host_.reason.clear();
        controller_->start();
        QCOMPARE(runner_->calls.size(), 2);
        QCOMPARE(runner_->calls.at(0).first, QString("download"));
        QCOMPARE(runner_->calls.at(1).first, QString("apply"));
    }

    void cancelCloseOrSettingsFailureDoesNotExit() {
        use_controller();
        controller_->start();
        downloaded();
        host_.prepare_result = {false, "not saved"};
        emit runner_->event_received("ready", ID, {});
        QCOMPARE(runner_->sent, QStringList{"cancel"});
        runner_->finish();
        QCOMPARE(host_.quit_count, 0);
        QVERIFY(controller_->dialog->status->text().contains("not saved"));
    }

    void closeDownloadingCancelsWithoutInstall() {
        use_controller();
        controller_->start();
        controller_->dialog->close();
        downloaded();
        QCOMPARE(runner_->sent, QStringList{"cancel"});
        QCOMPARE(runner_->calls.size(), 1);
    }

    void buildWithoutHelperShowsExplanation() {
        if (QFileInfo::exists(":/updater/jietuba_updater.exe")) QSKIP("This build bundles the helper");
        use_controller();
        set_helper_source_for_test({});
        controller_->start();
        QVERIFY(runner_->calls.isEmpty());
        QVERIFY(controller_->dialog->status->text().contains("packaged"));
    }

    void dialogRejectCancelsUncommittedAndBlocksCommitted() {
        use_controller();
        controller_->dialog->show();
        controller_->dialog->reject();
        QVERIFY(controller_->cancelled());
        QCOMPARE(runner_->sent, QStringList{"cancel"});
        controller_->dialog->show();
        controller_->dialog->committed = true;
        controller_->dialog->reject();
        QVERIFY(controller_->dialog->isVisible());
        QCOMPARE(runner_->sent, QStringList{"cancel"});
    }

    void downloadRequestIsRemovedWhenProcessFinishes() {
        use_controller();
        controller_->start();
        const QString requests = QDir(cache_).filePath("requests");
        QCOMPARE(QDir(requests).entryList(QDir::Files).size(), 1);
        emit runner_->failed("download failed");
        runner_->finish();
        QVERIFY(QDir(requests).entryList(QDir::Files).isEmpty());
        QCOMPARE(host_.quit_count, 0);
    }

    void asyncProcessExchangesJsonlAndCompletes_data() {
        QTest::addColumn<bool>("cancel");
        QTest::newRow("go") << false;
        QTest::newRow("cancel") << true;
    }
    void asyncProcessExchangesJsonlAndCompletes() {
        QFETCH(bool, cancel);
        qputenv("JIETUBA_FAKE_UPDATER", "protocol");
        UpdaterProcess runner;
        QSignalSpy received(&runner, &UpdaterProcess::event_received);
        QSignalSpy errors(&runner, &UpdaterProcess::failed);
        QVERIFY(runner.start("check", {}, 10000));
        QVERIFY(!runner.start("check", {}));
        QTRY_VERIFY_WITH_TIMEOUT(!received.isEmpty(), 10000);
        QCOMPARE(received.first().first().toString(), QString("progress"));
        if (cancel) {
            runner.cancel();
        } else {
            QVERIFY(runner.send("go"));
        }
        QTRY_VERIFY_WITH_TIMEOUT(!runner.busy(), 10000);
        QCOMPARE(errors.count(), cancel ? 1 : 0);
        if (cancel) QCOMPARE(errors.first().first().toString(), QString("The update was cancelled."));
        QCOMPARE(received.last().first().toString(), cancel ? QString("error") : QString("downloaded"));
        QVERIFY(!runner.send("go"));
    }

    void processBufferingRejectsMismatchAndLargeLine() {
        qputenv("JIETUBA_FAKE_UPDATER", "chunked");
        UpdaterProcess chunked;
        QSignalSpy received(&chunked, &UpdaterProcess::event_received);
        QSignalSpy errors(&chunked, &UpdaterProcess::failed);
        QVERIFY(chunked.start("check", {}));
        QTRY_VERIFY_WITH_TIMEOUT(!chunked.busy(), 10000);
        QCOMPARE(received.count(), 1);
        QCOMPARE(received.first().first().toString(), QString("progress"));
        QCOMPARE(errors.first().first().toString(), QString("Invalid updater protocol"));
        qputenv("JIETUBA_FAKE_UPDATER", "large");
        UpdaterProcess large;
        QSignalSpy large_errors(&large, &UpdaterProcess::failed);
        QVERIFY(large.start("check", {}));
        QTRY_VERIFY_WITH_TIMEOUT(!large.busy(), 10000);
        QCOMPARE(large_errors.first().first().toString(), QString("Updater message too large"));
    }

    void shutdownWaitsForCancellationAndRemovesTheHelper_data() {
        QTest::addColumn<QByteArray>("scenario");
        QTest::newRow("cooperative") << QByteArray("protocol");
        QTest::newRow("unresponsive") << QByteArray("sleep");
    }

    void shutdownWaitsForCancellationAndRemovesTheHelper() {
        QFETCH(QByteArray, scenario);
        qputenv("JIETUBA_FAKE_UPDATER", scenario);
        UpdaterProcess runner;
        QSignalSpy stopped(&runner, &UpdaterProcess::finished);
        QSignalSpy failed(&runner, &UpdaterProcess::failed);
        QVERIFY(runner.start("download-model", {}));
        QVERIFY(runner.process()->waitForStarted(3000));
        const QString helper = runner.process()->program();
        runner.shutdown();
        QCOMPARE(runner.process()->state(), QProcess::NotRunning);
        QVERIFY(!runner.busy());
        QCOMPARE(stopped.size(), 1);
        QCOMPARE(failed.size(), 1);
        if (scenario == "protocol") QVERIFY(failed.first().first().toString().contains("cancelled"));
        QVERIFY(!QFileInfo::exists(helper));
    }

    void processReportsMissingHelperAndIncompleteExit() {
        UpdaterProcess runner;
        QSignalSpy errors(&runner, &UpdaterProcess::failed);
        set_helper_source_for_test(QDir(local_->path()).filePath("missing.exe"));
        QVERIFY(!runner.start("check", {}));
        QCOMPARE(errors.count(), 1);
        set_helper_source_for_test(FAKE_HELPER);
        qputenv("JIETUBA_FAKE_UPDATER", "stderr");
        QVERIFY(runner.start("check", {}));
        QTRY_VERIFY_WITH_TIMEOUT(!runner.busy(), 10000);
        QCOMPARE(errors.last().first().toString(), QString("diagnostic"));
    }

    void failedProcessStartAndTimeoutReportOnce() {
        const QString not_a_program = QDir(local_->path()).filePath("broken.exe");
        // Empty: a non-image file with content is taken for a 16-bit program, and Windows then
        // shows a message box that waits for a click.
        QFile file(not_a_program);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        set_helper_source_for_test(not_a_program);
        UpdaterProcess runner;
        QSignalSpy errors(&runner, &UpdaterProcess::failed);
        QSignalSpy finished(&runner, &UpdaterProcess::finished);
        QVERIFY(runner.start("check", {}));
        QTRY_COMPARE(finished.count(), 1);
        QVERIFY(!runner.busy());
        QCOMPARE(errors.count(), 1);
        set_helper_source_for_test(FAKE_HELPER);
        qputenv("JIETUBA_FAKE_UPDATER", "sleep");
        QVERIFY(runner.start("check", {}, 500));
        QTRY_VERIFY_WITH_TIMEOUT(!runner.busy(), 10000);
        QCOMPARE(errors.count(), 2);
        QCOMPARE(errors.last().first().toString(), QString("Update process timed out"));
    }

    void updaterErrorCodesAreShownInTheUiLanguage() {
        UpdaterProcess runner;
        QVERIFY(runner.error_text({{"code", "network"}, {"message", "下载失败"}}).startsWith("Could not download the update"));
        QVERIFY(runner.error_text({{"code", "recovery_failed"}, {"message", "备份损坏"}}).startsWith("The previous version"));
        QCOMPARE(runner.error_text({{"code", "mystery"}, {"message", "未知"}}), QString("The updater stopped with an error (mystery)."));
    }

    void processExitCleansHelperAndOnlyFailedDownload_data() {
        QTest::addColumn<bool>("success");
        QTest::newRow("downloaded") << true;
        QTest::newRow("failed") << false;
    }
    void processExitCleansHelperAndOnlyFailedDownload() {
        QFETCH(bool, success);
        const QString transaction(32, 'a');
        const QString stage = directory(cache_, transaction, {{success ? "app.exe" : "package.part", "data"}});
        qputenv("JIETUBA_FAKE_UPDATER", success ? "downloaded" : "fail");
        UpdaterProcess runner;
        QVERIFY(runner.start("download", {"--transaction", transaction}));
        QTRY_VERIFY_WITH_TIMEOUT(!runner.busy(), 10000);
        QVERIFY(QDir(QDir(cache_).filePath("helpers")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
        QCOMPARE(QFileInfo::exists(stage), success);
    }

    void bundledHelperIsCopiedOutForEachRun() {
        const QString embedded = QDir(local_->path()).filePath("bundle/jietuba_updater.exe");
        QDir().mkpath(QFileInfo(embedded).absolutePath());
        QFile file(embedded);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("CURRENT EMBEDDED HELPER");
        file.close();
        set_helper_source_for_test(embedded);
        const QString copied = helper_path();
        QVERIFY(QFile::remove(embedded));
        QFile copy(copied);
        QVERIFY(copy.open(QIODevice::ReadOnly));
        QCOMPARE(copy.readAll(), QByteArray("CURRENT EMBEDDED HELPER"));
        copy.close();
        QVERIFY(QDir::cleanPath(copied).startsWith(QDir::cleanPath(cache_)));
        update_cache::remove_helper(copied);
        QVERIFY(!QFileInfo::exists(copied));
    }

    // With JIETUBA_UPDATES the real helper is in the resources: its copy runs and reports a release build.
    void bundledHelperRunsFromItsCopy() {
        if (!QFileInfo::exists(":/updater/jietuba_updater.exe")) QSKIP("Built without JIETUBA_UPDATES");
        set_helper_source_for_test({});
        QVERIFY(updater_bundled());
        const QString copied = helper_path();
        QProcess process;
        process.start(copied, {"--version"});
        QVERIFY(process.waitForFinished(10000));
        const QJsonObject reply = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
        QCOMPARE(reply.value("event").toString(), QString("version"));
        QCOMPARE(reply.value("data").toObject().value("test_build").toBool(true), false);
        update_cache::remove_helper(copied);
    }

    // The Store updates a packaged app; even a helper that would run must not turn one-click updates on.
    void aPackagedAppOffersNoOneClickUpdate() {
        set_helper_source_for_test(FAKE_HELPER);
        QVERIFY(updater_bundled());
        set_packaged_for_test(true);
        const auto restore = qScopeGuard([] { set_packaged_for_test(std::nullopt); });
        QVERIFY(!updater_bundled());
    }

    void everyBuildUpdatesAsTheLitePackage() {
        QCOMPARE(app_variant(), QString("lite"));
    }

    void checkOutputCarriesThePublishTime() {
        const QByteArray with = R"({"protocol":1,"event":"available","data":{"tag_name":"v99.0.0","title":"t","notes":"n","url":"u","asset_name":"p.zip",)"
                                R"("arch":"x64","artifact":{"urls":["https://example.com/p.zip"]},"published_at":"2026-11-20T08:00:00Z"}})";
        QCOMPARE(release_from_check_output(with, 0).published_at, QString("2026-11-20T08:00:00Z"));
        QByteArray without = with;
        without.replace(R"(,"published_at":"2026-11-20T08:00:00Z")", "");
        QVERIFY(release_from_check_output(without, 0).published_at.isEmpty());
    }

    void versionTextDropsTheTagPrefix() {
        QCOMPARE(version_text("release-2.2.5"), QString("2.2.5"));
        QCOMPARE(version_text("v3.0"), QString("3.0"));
        QCOMPARE(version_text("latest"), QString("latest"));
    }

    void releasesInstallOnTheirOwnTwoDaysAfterPublishing() {
        const QDateTime now = QDateTime::fromString("2026-11-24T12:00:00Z", Qt::ISODate);
        ReleaseInfo info = release();
        info.published_at = "2026-11-22T12:00:00Z";
        QVERIFY(AutoUpdater::old_enough(info, now));
        info.published_at = "2026-11-22T12:00:01Z";
        QVERIFY(!AutoUpdater::old_enough(info, now));
        // Without a usable time it is left to a manual update.
        for (const QString &value : {QString(), QString("yesterday")}) {
            info.published_at = value;
            QVERIFY(!AutoUpdater::old_enough(info, now));
        }
    }

    void backgroundDownloadOpensNoWindowAndKeepsTheResult() {
        use_background_controller();
        const QString stage = directory(cache_, ID, {{"app.exe", "MZ"}});
        QSignalSpy finished(controller_, &UpdateController::background_download_finished);
        QVERIFY(controller_->download_in_background(release()));
        QVERIFY(!controller_->dialog);
        QCOMPARE(runner_->calls.size(), 1);
        QCOMPARE(runner_->calls.first().first, QString("download"));
        QVERIFY(!controller_->download_in_background(release()));
        emit runner_->event_received("progress", ID, {{"received", 50}, {"total", 100}});
        downloaded();
        // Nothing installs until the next start.
        QCOMPARE(runner_->calls.size(), 1);
        QCOMPARE(finished.count(), 1);
        QVERIFY(finished.first().first().toBool());
        QVERIFY(controller_->has_download(release()));
        QVERIFY(!controller_->download_in_background(release()));
        // Quitting keeps it.
        controller_->cancel();
        QVERIFY(QFileInfo::exists(stage));
        QVERIFY(controller_->has_download(release()));
    }

    void presentingADownloadedReleaseInstallsItWithoutDownloadingAgain() {
        use_background_controller();
        const QString stage = directory(cache_, ID, {{"app.exe", "MZ"}});
        QVERIFY(controller_->download_in_background(release()));
        downloaded();
        controller_->present(release());
        QVERIFY(controller_->dialog);
        // Closing the window keeps it for the next start as well.
        controller_->dialog->reject();
        QVERIFY(QFileInfo::exists(stage));
        controller_->present(release());
        controller_->start();
        QCOMPARE(runner_->calls.size(), 2);
        QCOMPARE(runner_->calls.last().first, QString("apply"));
        QCOMPARE(options(runner_->calls.last().second)["--transaction"], ID);
    }

    void presentingDuringABackgroundDownloadTakesItOver() {
        use_background_controller();
        QSignalSpy finished(controller_, &UpdateController::background_download_finished);
        QVERIFY(controller_->download_in_background(release()));
        controller_->present(release());
        QVERIFY(controller_->dialog);
        QVERIFY(controller_->dialog->working);
        emit runner_->event_received("progress", ID, {{"received", 25}, {"total", 100}});
        QCOMPARE(controller_->dialog->progress->value(), 25);
        downloaded();
        QCOMPARE(finished.count(), 0);
        QCOMPARE(runner_->calls.size(), 2);
        QCOMPARE(runner_->calls.last().first, QString("apply"));
    }

    void aNewerReleaseReplacesTheKeptDownload() {
        use_background_controller();
        const QString stage = directory(cache_, ID, {{"app.exe", "MZ"}});
        QVERIFY(controller_->download_in_background(release()));
        downloaded();
        ReleaseInfo newer = release();
        newer.tag_name = "release-99.1.0";
        QVERIFY(controller_->download_in_background(newer));
        QVERIFY(!QFileInfo::exists(stage));
        QCOMPARE(runner_->calls.size(), 2);
    }

    void backgroundInstallHandsOffWithoutAWindow() {
        use_background_controller();
        QVERIFY(controller_->install_in_background(release(), ID));
        QVERIFY(!controller_->dialog);
        QCOMPARE(runner_->calls.last().first, QString("apply"));
        QCOMPARE(options(runner_->calls.last().second)["--transaction"], ID);
        emit runner_->event_received("ready", ID, {});
        QCOMPARE(runner_->sent, QStringList{"go"});
        emit runner_->event_received("handed_off", ID, {});
        runner_->finish();
        QCOMPARE(host_.quit_count, 1);
    }

    void failedBackgroundInstallIsReportedAndDropped() {
        use_background_controller();
        const QString stage = directory(cache_, ID, {{"app.exe", "MZ"}});
        QSignalSpy failed(controller_, &UpdateController::background_install_failed);
        QVERIFY(controller_->install_in_background(release(), ID));
        emit runner_->failed("No permission");
        runner_->finish();
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.first().first().toString(), QString("No permission"));
        QCOMPARE(host_.quit_count, 0);
        QVERIFY(!QFileInfo::exists(stage));
        // A busy application does not start it.
        host_.reason = "busy";
        QVERIFY(!controller_->install_in_background(release(), ID));
        QCOMPARE(runner_->calls.size(), 1);
    }

    void pendingUpdateIsTheNewestStagedForThisProgramAndVersion() {
        const QString exe = QCoreApplication::applicationFilePath();
        // The helper writes the path as Rust canonicalizes it.
        const QString recorded = "\\\\?\\" + QDir::toNativeSeparators(QFileInfo(exe).canonicalFilePath()).toUpper();
        staged(cache_, QString(32, 'a'), recorded, APP_VERSION, "release-99.0.0");
        staged(cache_, QString(32, 'b'), recorded, APP_VERSION, "release-99.1.0");
        staged(cache_, QString(32, 'c'), recorded, "0.0.1", "release-99.2.0");
        staged(cache_, QString(32, 'd'), "C:\\other\\jietuba_lite.exe", APP_VERSION, "release-99.3.0");
        staged(cache_, QString(32, 'e'), recorded, APP_VERSION, "release-0.0.1");
        directory(cache_, QString(32, 'f'), {{"prepared.json", "{}"}});
        const auto pending = find_pending_update(cache_, exe, APP_VERSION);
        QVERIFY(pending);
        QCOMPARE(pending->transaction, QString(32, 'b'));
        QCOMPARE(pending->release.tag_name, QString("release-99.1.0"));
        QVERIFY(pending->release.available == true);
        QVERIFY(!find_pending_update(cache_, exe, "99.9.9"));
    }

    void autoUpdaterDownloadsAReleaseTwoDaysAfterItIsPublished() {
        use_background_controller();
        use_settings();
        qputenv("JIETUBA_FAKE_UPDATER", "check-available");
        qputenv("JIETUBA_FAKE_PUBLISHED", days_ago(3));
        AutoUpdater updater(config_.get(), controller_);
        QSignalSpy ready(&updater, &AutoUpdater::update_ready);
        updater.check_now();
        QTRY_COMPARE_WITH_TIMEOUT(runner_->calls.size(), 1, 10000);
        QCOMPARE(runner_->calls.first().first, QString("download"));
        QVERIFY(!controller_->dialog);
        QCOMPARE(updater.timer.interval(), updater.interval_ms);
        downloaded();
        QCOMPARE(ready.count(), 1);
        QCOMPARE(ready.first().first().value<ReleaseInfo>().tag_name, QString("v99.0.0"));
        // The next check finds it downloaded and says nothing again.
        updater.check_now();
        QTRY_VERIFY_WITH_TIMEOUT(!updater.checking(), 10000);
        QCOMPARE(runner_->calls.size(), 1);
        QCOMPARE(ready.count(), 1);
    }

    void autoUpdaterWaitsForNewReleasesAndSkipsFailedOnes() {
        use_background_controller();
        use_settings();
        qputenv("JIETUBA_FAKE_UPDATER", "check-available");
        AutoUpdater updater(config_.get(), controller_);
        const auto check = [&updater] {
            updater.check_now();
            QTRY_VERIFY_WITH_TIMEOUT(!updater.checking(), 10000);
        };
        qputenv("JIETUBA_FAKE_PUBLISHED", days_ago(1));
        check();
        QVERIFY(runner_->calls.isEmpty());
        qunsetenv("JIETUBA_FAKE_PUBLISHED");
        check();
        QVERIFY(runner_->calls.isEmpty());
        qputenv("JIETUBA_FAKE_PUBLISHED", days_ago(5));
        config_->set_auto_update_failed_version("v99.0.0");
        check();
        QVERIFY(runner_->calls.isEmpty());
        config_->set_auto_update_failed_version("");
        check();
        QCOMPARE(runner_->calls.size(), 1);
    }

    void autoUpdaterOffChecksNothingAndStopsItsDownload() {
        use_background_controller();
        use_settings();
        config_->set_auto_update(false);
        AutoUpdater updater(config_.get(), controller_);
        updater.start();
        QVERIFY(!updater.timer.isActive());
        updater.check_now();
        QVERIFY(!updater.checking());
        config_->set_auto_update(true);
        updater.set_enabled(true);
        QVERIFY(updater.timer.isActive());
        QCOMPARE(updater.timer.interval(), updater.first_check_ms);
        QVERIFY(controller_->download_in_background(release()));
        config_->set_auto_update(false);
        updater.set_enabled(false);
        QVERIFY(!updater.timer.isActive());
        QCOMPARE(runner_->sent, QStringList{"cancel"});
    }

    void startUpInstallsTheDownloadFromTheLastRun() {
        use_background_controller();
        use_settings();
        const QString stage = staged(cache_, ID, "\\\\?\\" + QDir::toNativeSeparators(QCoreApplication::applicationFilePath()), APP_VERSION, "release-99.0.0");
        AutoUpdater updater(config_.get(), controller_);
        int resumed = 0;
        QVERIFY(updater.install_pending([&resumed] { ++resumed; }));
        QCOMPARE(runner_->calls.last().first, QString("apply"));
        QCOMPARE(options(runner_->calls.last().second)["--transaction"], ID);
        QVERIFY(QFileInfo::exists(QDir(stage).filePath("auto_install")));
        // A failure lets start-up go on and leaves that release to a manual update.
        emit runner_->failed("No permission");
        runner_->finish();
        QCOMPARE(resumed, 1);
        QCOMPARE(config_->get_auto_update_failed_version(), QString("release-99.0.0"));
        QVERIFY(!QFileInfo::exists(stage));
        QVERIFY(!updater.install_pending([&resumed] { ++resumed; }));
    }

    void startUpDropsADownloadWhoseInstallDidNotTake() {
        use_background_controller();
        use_settings();
        const QString stage = staged(cache_, ID, QCoreApplication::applicationFilePath(), APP_VERSION, "release-99.0.0");
        // Left by the last start: the worker restored the old version instead of removing the folder.
        directory(cache_, ID, {{"auto_install", {}}});
        AutoUpdater updater(config_.get(), controller_);
        QVERIFY(!updater.install_pending([] {}));
        QVERIFY(runner_->calls.isEmpty());
        QCOMPARE(config_->get_auto_update_failed_version(), QString("release-99.0.0"));
        QVERIFY(!QFileInfo::exists(stage));
    }

    void startUpNotesAnUpdateAndInstallsNothingWhenOff() {
        use_background_controller();
        use_settings();
        config_->set_last_run_version("0.0.1");
        config_->set_auto_update(false);
        staged(cache_, ID, QCoreApplication::applicationFilePath(), APP_VERSION, "release-99.0.0");
        AutoUpdater updater(config_.get(), controller_);
        QVERIFY(!updater.install_pending([] {}));
        QVERIFY(runner_->calls.isEmpty());
        QCOMPARE(updater.updated_from(), QString("0.0.1"));
        QCOMPARE(config_->get_last_run_version(), APP_VERSION);
        AutoUpdater again(config_.get(), controller_);
        again.install_pending([] {});
        QVERIFY(again.updated_from().isEmpty());
    }

    void sweepRemovesOldCacheAndPreservesRecentFilesAndBackups() {
        const QString old_stage = directory(cache_, QString(32, 'a'), {{"app.exe", "old"}}, true);
        const QString old_helper = directory(QDir(cache_).filePath("helpers"), QString(32, 'b'), {{"jietuba_updater.exe", "old"}}, true);
        const QString requests = directory(cache_, "requests", {{QString(32, 'c') + ".json", "{}"}, {"user.json", "keep"}}, true);
        const QString recent_stage = directory(cache_, QString(32, 'd'), {{"app.exe", "recent"}});
        const QString backup = directory(QDir(cache_).filePath("../.jietuba-update"), QString(32, 'e'), {{"backup.exe", "backup"}}, true);
        update_cache::begin_session();
        QVERIFY(!QFileInfo::exists(old_stage) && !QFileInfo::exists(old_helper));
        QVERIFY(!QFileInfo::exists(QDir(requests).filePath(QString(32, 'c') + ".json")));
        QVERIFY(QFileInfo::exists(QDir(requests).filePath("user.json")));
        QVERIFY(QFileInfo::exists(recent_stage) && QFileInfo::exists(backup));
    }

    void liveApplicationSessionPreventsSweep() {
        const QString old_stage = directory(cache_, QString(32, 'a'), {{"app.exe", "still in use"}}, true);
        QLockFile lease(QDir(cache_).filePath(QString(32, 'b') + ".session.lock"));
        lease.setStaleLockTime(0);
        QVERIFY(lease.tryLock(0));
        update_cache::begin_session();
        QVERIFY(QFileInfo::exists(old_stage));
        lease.unlock();
    }

    void crashedSessionLockDoesNotPreventCleanup() {
        const QString old_stage = directory(cache_, QString(32, 'a'), {{"package.part", "orphan"}}, true);
        const QString lock = QDir(cache_).filePath(QString(32, 'b') + ".session.lock");
        QProcess process;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("JIETUBA_FAKE_UPDATER", "lock");
        process.setProcessEnvironment(env);
        process.start(FAKE_HELPER, {lock});
        QVERIFY(process.waitForFinished(10000));
        QCOMPARE(process.exitCode(), 0);
        QVERIFY(QFileInfo::exists(lock));
        update_cache::begin_session();
        QVERIFY(!QFileInfo::exists(lock) && !QFileInfo::exists(old_stage));
    }

    void unfinishedWorkerPreservesItsStage() {
        const QString stage = directory(cache_, QString(32, 'a'), {{"app.exe", "pending"}}, true);
        const QJsonObject request{{"staged", QDir::toNativeSeparators(QDir(stage).filePath("app.exe"))}};
        const QString worker = directory(cache_, QString(32, 'b'), {{"request.json", QJsonDocument(request).toJson()}}, true);
        update_cache::begin_session();
        QVERIFY(QFileInfo::exists(stage) && QFileInfo::exists(worker));
    }

    void finishedWorkerCacheCanBeSwept() {
        const QString worker = directory(cache_, QString(32, 'b'), {{"request.json", "{}"}, {"result.json", "{}"}, {"jietuba_updater.exe", "old"}}, true);
        update_cache::begin_session();
        QVERIFY(!QFileInfo::exists(worker));
    }

    void cleanupRefusesNestedDirectoriesAndOutsidePaths() {
        const QString nested = directory(cache_, QString(32, 'a'), {{"app.exe", "preserve"}});
        QDir().mkdir(QDir(nested).filePath("unknown"));
        QTemporaryDir outside_root;
        const QString outside = directory(outside_root.path(), QString(32, 'b'), {{"app.exe", "preserve"}});
        update_cache::remove_directory(nested);
        update_cache::remove_directory(outside);
        QVERIFY(QFileInfo::exists(QDir(nested).filePath("app.exe")) && QFileInfo::exists(QDir(outside).filePath("app.exe")));
    }

    void cleanupRefusesLinks() {
        QTemporaryDir outside_root;
        const QString outside = directory(outside_root.path(), QString(32, 'b'), {{"app.exe", "preserve"}});
        const QString link = QDir(cache_).filePath(QString(32, 'a'));
        // A junction needs no privileges, unlike a directory symlink.
        QCOMPARE(QProcess::execute("cmd.exe", {"/c", "mklink", "/J", QDir::toNativeSeparators(link), QDir::toNativeSeparators(outside)}), 0);
        update_cache::remove_directory(link);
        QVERIFY(QFileInfo::exists(QDir(outside).filePath("app.exe")));
        // Remove only the junction before the temporary folders are deleted recursively.
        QVERIFY(RemoveDirectoryW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(link).utf16())));
    }
};

QTEST_MAIN(UpdateTests)
#include "test_update.moc"
