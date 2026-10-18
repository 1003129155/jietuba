#include "ocr/bundled_runtime.h"
#include "ocr/ocr_manager.h"
#include "ocr/ppocr_models.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace jietuba;
using bundled_runtime::SeedResult;

namespace {
QByteArray read_all(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void write_all(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(bytes);
}
}

// The full package's exe puts its embedded onnxruntime.dll into models/ when none is usable.
class BundledRuntimeTests : public QObject {
    Q_OBJECT
private slots:
    void aBuildWithoutTheResourceEmbedsNothingAndWritesNothing() {
        QVERIFY(bundled_runtime::data().isEmpty());
        QTemporaryDir models;
        QVERIFY(models.isValid());
        ppocr_models::set_models_root_for_test(models.path());
        const auto restore = qScopeGuard([] { ppocr_models::set_models_root_for_test({}); });
        QString error;
        QCOMPARE(bundled_runtime::seed_embedded(&error), SeedResult::Unneeded);
        QVERIFY(QDir(models.path()).isEmpty());
        // Nothing to write, so a missing DLL stays missing rather than becoming an empty file.
        QCOMPARE(bundled_runtime::seed({}, QDir(models.path()).filePath("onnxruntime.dll")), SeedResult::Unneeded);
        QVERIFY(QDir(models.path()).isEmpty());
    }

    void aMissingDllIsWrittenWholeIntoAFolderThatDoesNotExistYet() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = QDir(dir.path()).filePath("models/onnxruntime.dll");
        QCOMPARE(bundled_runtime::seed("bytes", path), SeedResult::Written);
        QCOMPARE(read_all(path), QByteArray("bytes"));
        // The temporary file of the atomic write is gone.
        QCOMPARE(QDir(QFileInfo(path).absolutePath()).entryList(QDir::Files).size(), 1);
    }

    void anUnusableDllIsReplaced() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = QDir(dir.path()).filePath("onnxruntime.dll");
        // No version resource: not an ONNX Runtime this build can load.
        write_all(path, "MZ");
        QVERIFY(!ppocr_runtime_supported(path));
        QCOMPARE(bundled_runtime::seed("replacement", path), SeedResult::Written);
        QCOMPARE(read_all(path), QByteArray("replacement"));
    }

    void aUsableDllIsLeftAlone() {
        const QString pack = QDir(QCoreApplication::applicationDirPath()).filePath("models/onnxruntime.dll");
        if (!QFileInfo::exists(pack) || !ppocr_runtime_supported(pack)) QSKIP("No usable onnxruntime.dll next to the test executable; set JIETUBA_PPOCR_MODELS for the OCR tests.");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = QDir(dir.path()).filePath("onnxruntime.dll");
        QVERIFY(QFile::copy(pack, path));
        const QByteArray before = read_all(path);
        QCOMPARE(bundled_runtime::seed("replacement", path), SeedResult::Unneeded);
        QCOMPARE(read_all(path), before);
    }

    void aFailedWriteSaysWhy() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // The "folder" is a file, so neither it nor the DLL inside can be created.
        const QString blocker = QDir(dir.path()).filePath("models");
        write_all(blocker, "x");
        QString error;
        QCOMPARE(bundled_runtime::seed("bytes", QDir(blocker).filePath("onnxruntime.dll"), &error), SeedResult::Failed);
        QVERIFY(!error.isEmpty());
        QCOMPARE(read_all(blocker), QByteArray("x"));
    }
};

QTEST_GUILESS_MAIN(BundledRuntimeTests)
#include "test_bundled_runtime.moc"
