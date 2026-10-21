#include "ocr/snipping_tool_ocr.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>
#include <stdexcept>
#include <windows.h>

using namespace jietuba;

namespace {
// The engine binding against stand-in oneocr.dll files (tests/fake_oneocr.cpp), one per variant.
struct Variant {
    const char *name;
    const char *build_dir;
};
constexpr Variant FULL{"full", JIETUBA_FAKE_ONEOCR_FULL_DIR};
constexpr Variant LEGACY{"legacy", JIETUBA_FAKE_ONEOCR_LEGACY_DIR};
constexpr Variant BROKEN{"broken", JIETUBA_FAKE_ONEOCR_BROKEN_DIR};
}

class SnippingToolOcrTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir packages_;

    // The package's full name carries the process id: copies are cached by full name, and a
    // stand-in DLL left by an earlier run must not be taken for this build's.
    QString full_name(const Variant &variant) const {
        return QString("Fake.Oneocr.%1_1.0.0.0_x64__test%2").arg(variant.name).arg(QCoreApplication::applicationPid());
    }

    std::pair<QString, QString> package(const Variant &variant) {
        const QDir source(QDir(packages_.path()).filePath(variant.name));
        const auto ready = QDir().mkpath(source.path()) && (QFile::exists(source.filePath("oneocr.dll")) || QFile::copy(QDir(variant.build_dir).filePath("oneocr.dll"), source.filePath("oneocr.dll")));
        for (const char *name : {"oneocr.onemodel", "onnxruntime.dll"}) {
            QFile placeholder(source.filePath(name));
            if (!placeholder.open(QIODevice::WriteOnly)) return {};
        }
        return ready ? std::make_pair(full_name(variant), source.path()) : std::pair<QString, QString>();
    }

    // What the stand-in loaded for this variant recorded as the line limit; -1 when never set.
    long long fake_max_line_count(const Variant &variant) const {
        const auto path = QDir::toNativeSeparators(QDir(snipping_tool_cache_root()).filePath(full_name(variant) + "/oneocr.dll")).toStdWString();
        auto *module = GetModuleHandleW(path.c_str());
        if (!module) return -2;
        auto *probe = reinterpret_cast<long long (*)()>(GetProcAddress(module, "FakeMaxLineCount"));
        return probe ? probe() : -3;
    }

    // Sets what the stand-in loaded for this variant reports as the line's words.
    void set_fake_words(const Variant &variant, long long mode) const {
        const auto path = QDir::toNativeSeparators(QDir(snipping_tool_cache_root()).filePath(full_name(variant) + "/oneocr.dll")).toStdWString();
        auto *module = GetModuleHandleW(path.c_str());
        QVERIFY(module);
        auto *set = reinterpret_cast<void (*)(long long)>(GetProcAddress(module, "FakeWordMode"));
        QVERIFY(set);
        set(mode);
    }

    static QImage page() {
        QImage image(200, 100, QImage::Format_ARGB32);
        image.fill(Qt::white);
        return image;
    }

private slots:
    void initTestCase() { QVERIFY(packages_.isValid()); }

    void aLineLimitIsSetWhenTheEngineExportsIt() {
        const auto source = package(FULL);
        QVERIFY(!source.first.isEmpty());
        SnippingToolOcr engine(source);
        QCOMPARE(fake_max_line_count(FULL), 1000);
        const auto lines = engine.recognize(page());
        QCOMPARE(lines.size(), 1);
        QCOMPARE(lines[0].text, QString("fake line"));
    }

    // With every word's text and box the line gets a position for each character.
    void wordsGiveEveryCharacterAPosition() {
        const auto source = package(FULL);
        QVERIFY(!source.first.isEmpty());
        SnippingToolOcr engine(source);
        set_fake_words(FULL, 1);
        const auto lines = engine.recognize(page());
        QCOMPARE(lines.size(), 1);
        QVERIFY(!lines[0].vertical);
        QCOMPARE(lines[0].chars.size(), lines[0].text.size());
        for (int i = 1; i < lines[0].chars.size(); ++i) QVERIFY(lines[0].chars[i].center > lines[0].chars[i - 1].center);
        // Positions run along the line, from its start to its far end.
        QVERIFY(lines[0].chars.first().start >= -0.5f);
        QVERIFY(lines[0].chars.last().end <= 100.5f);
        QCOMPARE(lines[0].score, 1.0);
    }

    // One word that cannot be read leaves the line with its text and confidence but no positions.
    void aWordThatCannotBeReadLeavesTheLineWithoutPositions() {
        const auto source = package(FULL);
        QVERIFY(!source.first.isEmpty());
        SnippingToolOcr engine(source);
        for (long long mode : {2, 3, 4}) {
            set_fake_words(FULL, mode);
            const auto lines = engine.recognize(page());
            QCOMPARE(lines.size(), 1);
            QCOMPARE(lines[0].text, QString("fake line"));
            QVERIFY2(lines[0].chars.isEmpty(), qPrintable(QString("mode %1").arg(mode)));
        }
        set_fake_words(FULL, 0);
    }

    // Some Snipping Tool versions lack the export; the engine must still load and recognize.
    void anEngineWithoutTheLineLimitExportStillRecognizes() {
        const auto source = package(LEGACY);
        QVERIFY(!source.first.isEmpty());
        SnippingToolOcr engine(source);
        QCOMPARE(fake_max_line_count(LEGACY), -1);
        const auto lines = engine.recognize(page());
        QCOMPARE(lines.size(), 1);
        QCOMPARE(lines[0].text, QString("fake line"));
    }

    // Only the line limit is optional: any other missing export still fails the load, by name.
    void aMissingRequiredExportFailsTheLoad() {
        const auto source = package(BROKEN);
        QVERIFY(!source.first.isEmpty());
        QString error;
        try {
            SnippingToolOcr engine(source);
        } catch (const std::runtime_error &exception) {
            error = exception.what();
        }
        QVERIFY2(error.contains("Missing DLL function: RunOcrPipeline"), qPrintable(error));
    }
};

QTEST_MAIN(SnippingToolOcrTests)
#include "test_snipping_tool_ocr.moc"
