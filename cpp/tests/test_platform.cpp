#include "core/bootstrap.h"
#include "core/crash_handler.h"
#include "core/constants.h"
#include "core/platform_utils.h"
#include "ui/fluent_lite/frameless.h"
#include <QApplication>
#include <QScreen>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QWidget>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <string>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace jietuba;

namespace {
QString read_all(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

QString pid_file() { return QDir(QDir::tempPath()).filePath(QFileInfo(QCoreApplication::applicationFilePath()).fileName() + ".pid"); }

// The raw Run value of this build, to put back after the test.
std::optional<QString> read_run_value() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &key) != ERROR_SUCCESS) return std::nullopt;
    wchar_t buffer[2048] = {};
    DWORD size = sizeof buffer;
    DWORD type = 0;
    const auto status = RegQueryValueExW(key, reinterpret_cast<LPCWSTR>(AUTOSTART_APP_NAME.utf16()), nullptr, &type, reinterpret_cast<BYTE *>(buffer), &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ) return std::nullopt;
    return QString::fromWCharArray(buffer);
}

// A child process of this executable in one of the modes main() handles.
std::unique_ptr<QProcess> start_child(const QString &mode, const QString &app_data) {
    auto process = std::make_unique<QProcess>();
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("LOCALAPPDATA", app_data);
    process->setProcessEnvironment(environment);
    process->start(QCoreApplication::applicationFilePath(), {mode});
    return process;
}
}

// Process, window and registry helpers, crash logging and the single-instance rule, on the
// real system. Crashes and instances run as child processes of this executable.
class PlatformTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir dir_;
private slots:
    void initTestCase() {
        if (QGuiApplication::platformName() != "windows") QSKIP("needs -platform windows");
        QVERIFY(dir_.isValid());
    }

    void processesAreIdentifiedAndTerminated() {
        const auto self = get_process_identity(QCoreApplication::applicationPid());
        QVERIFY(self);
        QVERIFY(self->first > 0);
        QCOMPARE(self->second, QFileInfo(QCoreApplication::applicationFilePath()).fileName());
        QVERIFY(!get_process_identity(0));
        QVERIFY(!get_process_identity(-5));
        QVERIFY(!terminate_process_by_pid(0x7FFFFFF0));
        QProcess sleeper;
        sleeper.start("C:/Windows/System32/ping.exe", {"-n", "30", "127.0.0.1"});
        QVERIFY(sleeper.waitForStarted());
        const auto identity = get_process_identity(sleeper.processId());
        QVERIFY(identity);
        QCOMPARE(identity->second.toLower(), QString("ping.exe"));
        QVERIFY(terminate_process_by_pid(sleeper.processId()));
        QVERIFY(sleeper.waitForFinished(5000));
        QVERIFY(get_last_error() >= 0);
    }

    void windowsCanLeaveScreenCaptures() {
        QWidget window;
        window.resize(120, 80);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto hwnd = static_cast<quintptr>(window.winId());
        QVERIFY(set_window_exclude_from_capture(hwnd, true));
        DWORD affinity = 0;
        GetWindowDisplayAffinity(reinterpret_cast<HWND>(hwnd), &affinity);
        QCOMPARE(affinity, DWORD(WDA_EXCLUDEFROMCAPTURE));
        QVERIFY(set_window_exclude_from_capture(hwnd, false));
        GetWindowDisplayAffinity(reinterpret_cast<HWND>(hwnd), &affinity);
        QCOMPARE(affinity, DWORD(WDA_NONE));
        QVERIFY(!set_window_exclude_from_capture(0, true));
    }

    void dpiAndIdentityHelpersAnswer() {
        const double dpi = get_system_dpi();
        QVERIFY(dpi >= 96);
        QCOMPARE(get_system_dpi(-1), dpi);
        // Awareness is already set by Qt; a second request only logs.
        set_dpi_awareness();
        set_app_user_model_id("jietuba.platform.test");
    }

    // Windows' recognized games are full exe paths; the game check answers on any desktop, quickly
    // enough for the UI thread.
    void recognizedGamesAreExecutables() {
        QElapsedTimer clock;
        clock.start();
        recognized_game_executables();
        const qint64 cold = clock.nsecsElapsed() / 1000;
        clock.restart();
        const auto games = recognized_game_executables();
        qInfo("%lld recognized games read in %lld us (first read %lld us)", static_cast<long long>(games.size()),
              static_cast<long long>(clock.nsecsElapsed() / 1000), static_cast<long long>(cold));
        for (const auto &path : games)
            QVERIFY2(QFileInfo(path).isAbsolute() && path.endsWith(".exe", Qt::CaseInsensitive), qPrintable(path));
        clock.restart();
        const bool game = is_game_in_foreground();
        qInfo("game check: %s in %lld us", game ? "game" : "no game", static_cast<long long>(clock.nsecsElapsed() / 1000));
    }

    // A due trim waits while the program is busy, then runs.
    void trimmingWaitsWhileBusy() {
        int checks = 0;
        bool busy = true;
        set_trim_busy_check([&] {
            ++checks;
            return busy;
        });
        request_trim_working_set(10);
        QTRY_VERIFY(checks >= 2);
        busy = false;
        const int postponed = checks;
        QTRY_VERIFY_WITH_TIMEOUT(checks > postponed, 5000);
        // Once idle it trims and stops asking.
        QTest::qWait(100);
        QCOMPARE(checks, postponed + 1);
        set_trim_busy_check(nullptr);
        trim_working_set();
    }

    // The trim runs on a worker thread: a slow one does not hold up the UI thread.
    void trimRunsOffTheUiThread() {
        std::atomic_bool release = false, finished = false;
        std::atomic<DWORD> trim_thread = 0;
        set_trim_for_test([&] {
            trim_thread = GetCurrentThreadId();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!release && std::chrono::steady_clock::now() < deadline) Sleep(1);
            finished = true;
        });
        set_trim_busy_check(nullptr);
        request_trim_working_set(10);
        QTRY_VERIFY(trim_thread != 0);
        QVERIFY(!finished);
        QVERIFY(trim_thread != GetCurrentThreadId());
        release = true;
        QTRY_VERIFY(finished);
        set_trim_for_test({});
    }

    // Writing the Run value is persistent configuration, so only reading is checked here.
    void autostartReadsThisBuildsRunValue() { QCOMPARE(get_autostart(), read_run_value().has_value()); }

    // Every user profile has the Run key; without this the check above passes with a wrong path,
    // both sides then reading "no value".
    void autostartOpensTheRunKey() {
        HKEY key = nullptr;
        QCOMPARE(RegOpenKeyExW(HKEY_CURRENT_USER, AUTOSTART_REG_KEY, 0, KEY_READ, &key), LSTATUS(ERROR_SUCCESS));
        RegCloseKey(key);
    }

    // A native fault writes the code and the stack to the fault log.
    void aNativeCrashIsLogged() {
        const QString app_data = dir_.filePath("native");
        auto child = start_child("--crash-native", app_data);
        QVERIFY(child->waitForFinished(30000));
        QVERIFY(child->exitStatus() == QProcess::CrashExit || child->exitCode() != 0);
        const auto log = read_all(QDir(app_data).filePath("Jietuba/Logs/" + FAULT_LOG_NAME));
        QVERIFY2(log.contains("--- session"), qPrintable(log));
        QVERIFY2(log.contains("Windows fatal exception: code 0xc0000005"), qPrintable(log));
        QVERIFY(log.contains("Native stack"));
        QVERIFY(QString::fromLocal8Bit(child->readAllStandardError()).contains("Windows fatal exception"));
    }

    // An uncaught C++ exception goes to the crash log with its type and message.
    void anUncaughtExceptionIsLogged() {
        const QString app_data = dir_.filePath("throw");
        auto child = start_child("--crash-throw", app_data);
        QVERIFY(child->waitForFinished(30000));
        QVERIFY(child->exitStatus() == QProcess::CrashExit || child->exitCode() != 0);
        const auto log = read_all(QDir(app_data).filePath("Jietuba/Logs/" + CRASH_LOG_NAME));
        QVERIFY2(log.contains("boom from the test"), qPrintable(log));
        QVERIFY(log.contains("runtime_error"));
    }

    void writeCrashAppendsToTheLog() {
        const QByteArray saved = qgetenv("LOCALAPPDATA");
        qputenv("LOCALAPPDATA", dir_.filePath("direct").toLocal8Bit());
        write_crash("test tag", "line one\n");
        qputenv("LOCALAPPDATA", saved);
        const auto log = read_all(dir_.filePath("direct/Jietuba/Logs/" + CRASH_LOG_NAME));
        QVERIFY(log.contains("test tag"));
        QVERIFY(log.contains("line one"));
    }


    // The frameless window keeps native resize edges and maximises from its title bar.
    void framelessWindowsResizeFromTheirEdgesAndMaximise() {
        FramelessWindow window;
        window.setGeometry(QRect(QGuiApplication::primaryScreen()->availableGeometry().center() - QPoint(200, 150), QSize(400, 300)));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto hwnd = reinterpret_cast<HWND>(window.winId());
        RECT frame;
        GetWindowRect(hwnd, &frame);
        const auto hit = [hwnd](int x, int y) { return SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(x, y)); };
        const int mid_x = (frame.left + frame.right) / 2, mid_y = (frame.top + frame.bottom) / 2;
        QCOMPARE(hit(frame.left + 1, frame.top + 1), LRESULT(HTTOPLEFT));
        QCOMPARE(hit(frame.right - 2, frame.top + 1), LRESULT(HTTOPRIGHT));
        QCOMPARE(hit(frame.left + 1, frame.bottom - 2), LRESULT(HTBOTTOMLEFT));
        QCOMPARE(hit(frame.right - 2, frame.bottom - 2), LRESULT(HTBOTTOMRIGHT));
        QCOMPARE(hit(frame.left + 1, mid_y), LRESULT(HTLEFT));
        QCOMPARE(hit(frame.right - 2, mid_y), LRESULT(HTRIGHT));
        QCOMPARE(hit(mid_x, frame.top + 1), LRESULT(HTTOP));
        QCOMPARE(hit(mid_x, frame.bottom - 2), LRESULT(HTBOTTOM));
        QCOMPARE(hit(mid_x, mid_y), LRESULT(HTCLIENT));
        // Without resizing the edges are ordinary client area.
        window.setResizeEnabled(false);
        QCOMPARE(hit(frame.left + 1, mid_y), LRESULT(HTCLIENT));
        window.setResizeEnabled(true);
        // A double click on the title bar maximises, again restores; maximised, the frame stays on the screen.
        auto *title = window.titleBar();
        QVERIFY(title);
        QTest::mouseDClick(title, Qt::LeftButton, Qt::NoModifier, QPoint(title->width() / 3, title->height() / 2));
        QTRY_VERIFY(window.isMaximized());
        RECT client;
        GetClientRect(hwnd, &client);
        QVERIFY(client.right - client.left <= GetSystemMetrics(SM_CXSCREEN));
        QCOMPARE(hit(frame.left + 1, mid_y), LRESULT(HTCLIENT));
        QTest::mouseDClick(title, Qt::LeftButton, Qt::NoModifier, QPoint(title->width() / 3, title->height() / 2));
        QTRY_VERIFY(!window.isMaximized());
        // Caption buttons repaint on hover.
        for (auto *button : title->findChildren<TitleBarButton *>()) {
            QEnterEvent enter(QPointF(1, 1), QPointF(1, 1), QPointF(1, 1));
            QApplication::sendEvent(button, &enter);
            QEvent leave(QEvent::Leave);
            QApplication::sendEvent(button, &leave);
        }
    }
    // A new instance ends a confirmed older one and records itself; anything else is left alone.
    void aSecondInstanceEndsTheFirst() {
        QFile::remove(pid_file());
        const QString app_data = dir_.filePath("instance");
        auto first = start_child("--instance", app_data);
        QVERIFY(first->waitForReadyRead(10000));
        QCOMPARE(read_all(pid_file()).split('\n').first().toLongLong(), first->processId());
        auto second = start_child("--instance", app_data);
        QVERIFY(second->waitForReadyRead(10000));
        QVERIFY(first->waitForFinished(10000));
        QCOMPARE(read_all(pid_file()).split('\n').first().toLongLong(), second->processId());
        // A record of someone else's process (here: this test) is not acted on.
        QFile record(pid_file());
        QVERIFY(record.open(QIODevice::WriteOnly | QIODevice::Truncate));
        record.write(QByteArray::number(QCoreApplication::applicationPid()) + "\n12345\n");
        record.close();
        auto third = start_child("--instance", app_data);
        QVERIFY(third->waitForReadyRead(10000));
        QCOMPARE(second->state(), QProcess::Running);
        // A damaged record is rewritten.
        QVERIFY(record.open(QIODevice::WriteOnly | QIODevice::Truncate));
        record.write("not a pid\n");
        record.close();
        auto fourth = start_child("--instance", app_data);
        QVERIFY(fourth->waitForReadyRead(10000));
        QCOMPARE(read_all(pid_file()).split('\n').first().toLongLong(), fourth->processId());
        for (auto *child : {second.get(), third.get(), fourth.get()}) {
            child->kill();
            child->waitForFinished(5000);
        }
        QFile::remove(pid_file());
    }
};

int main(int argc, char **argv) {
    // Child modes, before Qt exists, as the application's own start-up runs.
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "--crash-native" || mode == "--crash-throw") {
        // No error dialogs from the system or the runtime in a test.
        SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        install_crash_hooks();
        if (mode == "--crash-throw") throw std::runtime_error("boom from the test");
        volatile int *null_pointer = nullptr;
        *null_pointer = 1;
        return 0;
    }
    if (mode == "--instance") {
        ensure_single_instance();
        std::fputs("ready\n", stdout);
        std::fflush(stdout);
        Sleep(30000);
        return 0;
    }
    QApplication app(argc, argv);
    PlatformTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "test_platform.moc"
