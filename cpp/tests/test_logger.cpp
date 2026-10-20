#include "core/constants.h"
#include "core/logger.h"
#include "settings/tool_settings.h"
#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <stdexcept>

using namespace jietuba;

namespace {
QString read_all(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

void write_file(const QString &path, const QByteArray &content) {
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) file.write(content);
}
}

// The process-wide logger: set-up, levels, formatting, exceptions, Qt messages and old-log cleanup.
class LoggerTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir dir_;
    QString log_path_;
private slots:
    void initTestCase() { QVERIFY(dir_.isValid()); }

    // A folder that cannot be created leaves logging off instead of failing later.
    void anUnusableFolderTurnsLoggingOff() {
        write_file(dir_.filePath("blocker"), "a file where the folder should be");
        auto &logger = get_logger();
        logger.setup(true, dir_.filePath("blocker/logs"));
        QVERIFY(!logger.ready());
        logger.setup(false);
        QVERIFY(!logger.ready());
    }

    void setUpWritesAHeaderAndLevelledLines() {
        QSettings settings(dir_.filePath("settings.ini"), QSettings::IniFormat);
        auto *config = get_tool_settings_manager(&settings);
        config->set_log_dir(dir_.filePath("logs"));
        config->set_log_level("INFO");
        config->set_log_enabled(true);
        // An old daily log is removed and oversized fixed logs are cut to their tail. The
        // Python version's files in the shared folder are left to it.
        QDir().mkpath(dir_.filePath("logs"));
        const QString old_date = QDate::currentDate().addDays(-30).toString("yyyyMMdd");
        const QString old_log = dir_.filePath("logs/" + RUNTIME_LOG_PREFIX + old_date + ".log");
        const QString python_log = dir_.filePath("logs/runtime_" + old_date + ".log");
        write_file(old_log, "old");
        write_file(python_log, "python");
        write_file(dir_.filePath("logs/runtime_notadate.log"), "kept");
        QByteArray big;
        for (int line = 0; line < 80000; ++line) big += "crash line " + QByteArray::number(line) + "\n";
        write_file(dir_.filePath("logs/" + CRASH_LOG_NAME), big);
        write_file(dir_.filePath("logs/crash.log"), big);
        setup_logger(config);
        auto &logger = get_logger();
        QVERIFY(logger.ready());
        log_path_ = logger.log_file_path();
        QVERIFY(QFileInfo(log_path_).fileName().startsWith(RUNTIME_LOG_PREFIX));
        QVERIFY(!QFile::exists(old_log));
        QVERIFY(QFile::exists(python_log));
        QVERIFY(QFile::exists(dir_.filePath("logs/runtime_notadate.log")));
        const auto crash = read_all(dir_.filePath("logs/" + CRASH_LOG_NAME));
        QVERIFY(crash.size() < big.size());
        QVERIFY(crash.contains("crash line 79999"));
        QVERIFY(!crash.contains("crash line 0\n"));
        QCOMPARE(QFileInfo(dir_.filePath("logs/crash.log")).size(), qint64(big.size()));
        // Setting up again or moving the folder once running is refused.
        logger.setup(true, dir_.filePath("elsewhere"));
        logger.set_log_dir(dir_.filePath("elsewhere"));
        QCOMPARE(logger.log_file_path(), log_path_);

        log_debug(T("hidden {n}", {{"n", 1}}), "Test");
        log_info(T("shown {n:d} {h:X} {l:x} {f:.2f}", {{"n", 42}, {"h", 255}, {"l", -26}, {"f", 1.005}}), "Test");
        log_warning("plain warning");
        // Geometry goes in as its debug text, as the callers write it; None and booleans as Python prints them.
        log_error(T("rect {r} point {p} size {s} none {x} flag {b}", {{"r", geometry_text(QRect(1, 2, 3, 4))}, {"p", geometry_text(QPoint(5, 6))}, {"s", geometry_text(QSize(7, 8))}, {"x", QVariant()}, {"b", true}}));
        // A malformed template is written as it is rather than lost.
        log_error(T("broken {n:d}", {{"n", 1.5}}));
        log_error(T("missing {absent}"));
        set_log_level(LogLevel::Debug);
        set_console_log_level(LogLevel::Error);
        log_debug("now visible");
        const auto text = read_all(log_path_);
        QVERIFY(text.contains("Jietuba"));
        QVERIFY(!text.contains("hidden 1"));
        QVERIFY2(text.contains("[INFO] [Test] shown 42 FF -1a 1.00"), qPrintable(text.right(800)));
        QVERIFY(text.contains("[WARNING] plain warning"));
        QVERIFY(text.contains("rect QRect(1,2 3x4)"));
        QVERIFY(text.contains("point QPoint(5,6)"));
        QVERIFY(text.contains("size QSize(7, 8) none None flag True"));
        QVERIFY(text.contains("broken {n:d}"));
        QVERIFY(text.contains("missing {absent}"));
        QVERIFY(text.contains("now visible"));
        QCOMPARE(geometry_text(QRectF(0.5, 1, 2, 3)), QString("QRectF(0.5,1 2x3)"));
        QCOMPARE(geometry_text(QSizeF(1.5, 2)), QString("QSizeF(1.5, 2)"));
        QCOMPARE(geometry_text(QPointF(868, 123.25)), QString("QPointF(868,123.25)"));
    }

    void exceptionsAndQtMessagesReachTheFile() {
        auto &logger = get_logger();
        log_exception(std::runtime_error("quiet failure"), T("while testing"));
        log_exception("ValueError", "loud failure", {}, false);
        log_exception_full(std::logic_error("full failure"), T("context"));
        qWarning("a Qt warning");
        logger.print("printed line");
        const auto text = read_all(log_path_);
        QVERIFY(text.contains("[SILENT] [静默异常] while testing"));
        QVERIFY(text.contains("quiet failure"));
        QVERIFY(text.contains("[ERROR] [静默异常] ValueError: loud failure"));
        QVERIFY(text.contains("[异常] context"));
        QVERIFY(text.contains("full failure"));
        QVERIFY(text.contains("a Qt warning"));
        QVERIFY(text.contains("printed line"));
        // Turned off, nothing more is written; turned on again, writing resumes.
        logger.set_enabled(false);
        log_error("while off");
        logger.set_enabled(true);
        log_error("back on");
        const auto after = read_all(log_path_);
        QVERIFY(!after.contains("while off"));
        QVERIFY(after.contains("back on"));
    }

    // Messages built from QString are UTF-8 and are not read as the ANSI code page;
    // bytes that are not UTF-8 still decode as the ANSI code page.
    void exceptionTextKeepsNonAsciiMessages() {
        const QString path = QString::fromUtf8("C:\\用户\\截图 ①.png");
        QCOMPARE(exception_text(std::runtime_error(("Cannot move " + path).toStdString())), "Cannot move " + path);
        log_exception(std::runtime_error(("Cannot remove " + path).toStdString()), T("non-ASCII"));
        QVERIFY(read_all(log_path_).contains("Cannot remove " + path));
        const QByteArray ansi("\x82\xa0");
        QCOMPARE(exception_text(std::runtime_error(ansi.toStdString())), QString::fromLocal8Bit(ansi));
    }

    void cleanupTestCase() { get_logger().close(); }
};

QTEST_GUILESS_MAIN(LoggerTests)
#include "test_logger.moc"
