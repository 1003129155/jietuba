// Linked into every test executable. Settings, data and logs are shared with the Python
// version, so a test that forgets its own throwaway settings must still not reach the
// user's: before main() this points the defaults (settings store, %LOCALAPPDATA%,
// %APPDATA%, desktop, autostart) at a temporary folder.
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtGlobal>
#include <memory>

namespace {
const QString SANDBOX_PREFIX = "jietuba-test-";

// Files still open at exit (a database, a process that crashed on purpose) keep a sandbox
// from being removed then; a later run removes it once it is a day old.
void remove_stale_sandboxes() {
    const auto cutoff = QDateTime::currentDateTime().addDays(-1);
    for (const auto &info : QDir(QDir::tempPath()).entryInfoList({SANDBOX_PREFIX + "*"}, QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (info.isSymLink() || info.isJunction() || info.lastModified() >= cutoff) continue;
        QDir(info.absoluteFilePath()).removeRecursively();
    }
}

struct TestSandbox {
    std::unique_ptr<QTemporaryDir> dir;
    TestSandbox() {
        // A child process started by a test keeps its parent's sandbox and environment.
        if (!qEnvironmentVariableIsEmpty("JIETUBA_TEST_SANDBOX")) return;
        remove_stale_sandboxes();
        dir = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/" + SANDBOX_PREFIX + "XXXXXX");
        if (!dir->isValid()) qFatal("test sandbox: no temporary folder");
        const QDir root(dir->path());
        root.mkpath("LocalAppData");
        root.mkpath("AppData");
        qputenv("JIETUBA_TEST_SANDBOX", QDir::toNativeSeparators(dir->path()).toLocal8Bit());
        qputenv("LOCALAPPDATA", QDir::toNativeSeparators(root.filePath("LocalAppData")).toLocal8Bit());
        qputenv("APPDATA", QDir::toNativeSeparators(root.filePath("AppData")).toLocal8Bit());
    }
};

const TestSandbox sandbox;
}
