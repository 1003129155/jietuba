#include "ocr/ocr_manager.h"
#include "ocr/ppocr_models.h"
#include "settings/tool_settings.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QPainter>
#include <QScopeGuard>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <vector>
#include <windows.h>

using namespace jietuba;

namespace {
// "major.minor.build.private" from a file's version resource; empty without one.
QString file_version(const QString &path) {
    const auto file = QDir::toNativeSeparators(path).toStdWString();
    DWORD ignored = 0;
    std::vector<char> data(GetFileVersionInfoSizeW(file.c_str(), &ignored));
    VS_FIXEDFILEINFO *info = nullptr;
    UINT length = 0;
    if (data.empty() || !GetFileVersionInfoW(file.c_str(), 0, static_cast<DWORD>(data.size()), data.data())
        || !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&info), &length) || !info)
        return {};
    return QString("%1.%2.%3.%4").arg(HIWORD(info->dwFileVersionMS)).arg(LOWORD(info->dwFileVersionMS)).arg(HIWORD(info->dwFileVersionLS)).arg(LOWORD(info->dwFileVersionLS));
}

QImage text_image(const QString &text) {
    QImage image(640, 160, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setPen(Qt::black);
    QFont font("Arial");
    font.setPixelSize(56);
    painter.setFont(font);
    painter.drawText(image.rect(), Qt::AlignCenter, text);
    painter.end();
    return image;
}

QString recognized_text(const OcrResult &result) {
    QStringList texts;
    for (const auto &line : result.data) texts << line.text;
    return texts.join(' ');
}

// Listing a model only checks that its files exist; without a runtime they are never opened.
void write_placeholder_model(const QString &root) {
    const QDir folder(QDir(root).filePath("placeholder"));
    QVERIFY(QDir().mkpath(folder.path()));
    for (const char *name : {"det.onnx", "rec.onnx"}) {
        QFile file(folder.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }
    QFile manifest(folder.filePath("model.json"));
    QVERIFY(manifest.open(QIODevice::WriteOnly));
    manifest.write(R"({"name": "placeholder", "det": "det.onnx", "rec": "rec.onnx"})");
}
}

// Engine choice, loading, recognition and release. PP-OCR needs its pack (models and
// onnxruntime.dll) next to the executable; JIETUBA_PPOCR_MODELS names a folder to copy it from.
class OcrManagerTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir dir_;
    QSettings *settings_ = nullptr;
    bool ppocr_ = false;
private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        settings_ = new QSettings(dir_.filePath("settings.ini"), QSettings::IniFormat, this);
        get_tool_settings_manager(settings_);
        QVERIFY(QFontDatabase::addApplicationFont(QDir(qEnvironmentVariable("WINDIR")).filePath("Fonts/arial.ttf")) >= 0);
        const QString source = qEnvironmentVariable("JIETUBA_PPOCR_MODELS");
        const QDir target(QDir(QCoreApplication::applicationDirPath()).filePath("models"));
        if (!source.isEmpty()) {
            QDir().mkpath(target.path());
            for (const char *name : {"PP-OCRv6_det_small.onnx", "PP-OCRv6_rec_small.onnx", "onnxruntime.dll"})
                if (!target.exists(name) && QDir(source).exists(name)) QVERIFY(QFile::copy(QDir(source).filePath(name), target.filePath(name)));
        }
        ppocr_ = get_ppocr_status() == "available";
    }

    void cleanup() { release_ocr_engine(); }

    void enginesResolveByPreference() {
        auto &ocr = OCRManager::instance();
        const auto engines = get_available_engines();
        QCOMPARE(is_ocr_available(), !engines.isEmpty());
        QCOMPARE(ocr.resolve_engine(OCRManager::ENGINE_AUTO), engines.isEmpty() ? std::nullopt : std::optional<QString>(engines.front()));
        QCOMPARE(ocr.resolve_engine("tesseract"), std::nullopt);
        QVERIFY(!set_ocr_engine("tesseract"));
        // With no engine at all, a missing choice has nothing to fall back to and reports why.
        set_oneocr_available_for_test(false);
        QTemporaryDir models;
        QVERIFY(models.isValid());
        ppocr_models::set_models_root_for_test(models.path());
        const auto restore = qScopeGuard([] {
            ppocr_models::set_models_root_for_test({});
            set_oneocr_available_for_test(std::nullopt);
            set_ocr_engine(OCRManager::ENGINE_AUTO);
        });
        QVERIFY(!set_ocr_engine(OCRManager::ENGINE_PP_RUST));
        QVERIFY(ocr.get_last_error().contains("PP-OCR"));
        QVERIFY(!initialize_ocr(OCRManager::ENGINE_PP_RUST));
        QCOMPARE(recognize_text(text_image("x")).code, -1);
        QVERIFY(!set_ocr_engine(OCRManager::ENGINE_ONEOCR));
        QVERIFY(!ocr.get_last_error().isEmpty());
        QCOMPARE(get_ocr_memory_status(), QString::fromUtf8("未初始化"));
    }

    // The pack's onnxruntime.dll is judged by its version resource, before anything loads it.
    void ppocrRuntimeNeedsOnnxRuntimeOneTwentyFourOrLater() {
        QVERIFY(!ppocr_runtime_supported(dir_.filePath("absent.dll")));
        QFile fake(dir_.filePath("onnxruntime.dll"));
        QVERIFY(fake.open(QIODevice::WriteOnly));
        fake.write("MZ");
        fake.close();
        QVERIFY(!ppocr_runtime_supported(fake.fileName()));
        // A DLL whose version is not ONNX Runtime 1.x.
        QVERIFY(!ppocr_runtime_supported(QDir(qEnvironmentVariable("WINDIR")).filePath("System32/kernel32.dll")));
        // Windows 11 ships an ONNX Runtime 1.17 with Windows ML.
        const QString system = QDir(qEnvironmentVariable("WINDIR")).filePath("System32/onnxruntime.dll");
        const QString version = QFileInfo::exists(system) ? file_version(system) : QString();
        if (version.startsWith("1.17.")) QVERIFY(!ppocr_runtime_supported(system));
        if (ppocr_) QVERIFY(ppocr_runtime_supported(QDir(QCoreApplication::applicationDirPath()).filePath("models/onnxruntime.dll")));
    }

    void ppocrRecognizesRenderedText() {
        if (!ppocr_) QSKIP("PP-OCR pack is not installed; set JIETUBA_PPOCR_MODELS.");
        auto &ocr = OCRManager::instance();
        QVERIFY(initialize_ocr(OCRManager::ENGINE_PP_RUST));
        QCOMPARE(get_current_engine(), std::optional<QString>(OCRManager::ENGINE_PP_RUST));
        QVERIFY(ocr.is_engine_loaded());
        QVERIFY(get_ocr_memory_status().contains("ppocr_rust"));
        // Loading again reuses the engine.
        QVERIFY(initialize_ocr(OCRManager::ENGINE_PP_RUST));
        const auto result = recognize_text(text_image("Hello 2026"));
        QCOMPARE(result.code, 100);
        QVERIFY2(recognized_text(result).contains("Hello"), qPrintable(recognized_text(result)));
        QVERIFY(result.elapse > 0);
        QCOMPARE(result.data.first().box.size(), 4);
        // One position per character, in reading order and inside the line.
        for (const auto &line : result.data) {
            QCOMPARE(line.chars.size(), line.text.toUcs4().size());
            float previous = 0;
            for (const auto &span : line.chars) {
                QVERIFY2(span.start >= previous - 1e-6f && span.start <= span.center && span.center <= span.end && span.end <= 1.0f, qPrintable(line.text));
                previous = span.end;
            }
        }
        QVERIFY(format_ocr_result_text(result).contains("2026"));
        // Formats other than RGB888 are converted; blank and null images find nothing.
        QImage alpha = text_image("Second line").convertToFormat(QImage::Format_ARGB32);
        QVERIFY(recognized_text(recognize_text(alpha)).contains("Second"));
        QImage white(200, 80, QImage::Format_RGB32);
        white.fill(Qt::white);
        const auto blank = recognize_text(white);
        QCOMPARE(blank.code, 100);
        QVERIFY(blank.data.isEmpty());
        QVERIFY(recognize_text(QImage()).data.isEmpty());
        // Releasing frees the engine; the next recognition loads it again.
        release_ocr_engine();
        QVERIFY(!ocr.is_engine_loaded());
        QVERIFY(recognized_text(recognize_text(text_image("Again"))).contains("Again"));
        QVERIFY(ocr.is_engine_loaded());
    }

    // The engine follows the chosen model and reloads when it changes; Auto takes the catalog's
    // choice, not the saved model. A deleted model gives way to the automatic choice while the
    // setting stays as it was.
    void switchingModelsReloadsAndAMissingOneFallsBackToAuto() {
        if (!ppocr_) QSKIP("PP-OCR pack is not installed; set JIETUBA_PPOCR_MODELS.");
        set_oneocr_available_for_test(false);
        const auto restore = qScopeGuard([] {
            set_oneocr_available_for_test(std::nullopt);
            set_ppocr_model({});
            set_ocr_engine(OCRManager::ENGINE_AUTO);
        });
        // A chosen Snipping Tool OCR that is not there gives way to PP-OCR.
        QVERIFY(set_ocr_engine(OCRManager::ENGINE_ONEOCR));
        QCOMPARE(get_current_engine(), std::optional<QString>(OCRManager::ENGINE_PP_RUST));
        const QDir root(ppocr_models::models_root());
        QVERIFY(initialize_ocr(OCRManager::ENGINE_PP_RUST));
        ppocr_models::import_model("copy", root.filePath("PP-OCRv6_det_small.onnx"), root.filePath("PP-OCRv6_rec_small.onnx"));
        set_ppocr_model("copy");
        QCOMPARE(OCRManager::instance().selected_ppocr_model()->value("id").toString(), QString("copy"));
        QVERIFY(recognized_text(recognize_text(text_image("Switched"))).contains("Switched"));
        QVERIFY(set_ocr_engine(OCRManager::ENGINE_AUTO));
        QCOMPARE(OCRManager::instance().selected_ppocr_model()->value("id").toString(), QString());
        QVERIFY(set_ocr_engine(OCRManager::ENGINE_PP_RUST));
        release_ocr_engine();
        ppocr_models::remove("copy");
        QCOMPARE(get_ppocr_status(), QString("available"));
        QCOMPARE(OCRManager::instance().selected_ppocr_model()->value("id").toString(), QString());
        QVERIFY(set_ocr_engine(OCRManager::ENGINE_PP_RUST));
        QVERIFY(recognized_text(recognize_text(text_image("Fallback"))).contains("Fallback"));
    }

    // A model that fails to load is passed over for the automatic one, and tried again once its
    // files are replaced.
    void aBrokenModelFallsBackAndIsRetriedOnceReplaced() {
        if (!ppocr_) QSKIP("PP-OCR pack is not installed; set JIETUBA_PPOCR_MODELS.");
        set_oneocr_available_for_test(false);
        const QDir pack(ppocr_models::models_root());
        const auto model = ppocr_models::import_model("damaged", pack.filePath("PP-OCRv6_det_small.onnx"), pack.filePath("PP-OCRv6_rec_small.onnx"));
        const auto restore = qScopeGuard([] {
            release_ocr_engine();
            ppocr_models::remove("damaged");
            set_oneocr_available_for_test(std::nullopt);
            set_ppocr_model({});
            set_ocr_engine(OCRManager::ENGINE_AUTO);
        });
        const QString det = model.value("det").toString();
        QVERIFY(QFile::remove(det));
        QFile junk(det);
        QVERIFY(junk.open(QIODevice::WriteOnly));
        junk.write("not a model");
        junk.close();
        set_ppocr_model("damaged");
        QVERIFY(set_ocr_engine(OCRManager::ENGINE_PP_RUST));
        QVERIFY(recognized_text(recognize_text(text_image("Fallback"))).contains("Fallback"));
        QVERIFY(OCRManager::instance().get_last_error().contains(QString::fromUtf8("初始化失败")));
        QCOMPARE(OCRManager::instance().selected_ppocr_model()->value("id").toString(), QString());
        release_ocr_engine();
        QVERIFY(QFile::remove(det));
        QVERIFY(QFile::copy(pack.filePath("PP-OCRv6_det_small.onnx"), det));
        QCOMPARE(OCRManager::instance().selected_ppocr_model()->value("id").toString(), QString("damaged"));
        QVERIFY(recognized_text(recognize_text(text_image("Repaired"))).contains("Repaired"));
    }

    // Failures carry the reason the result window explains: no model at all, or a model that
    // does not load with nothing else to use.
    void failuresReportTheirReason() {
        set_oneocr_available_for_test(false);
        QTemporaryDir models;
        QVERIFY(models.isValid());
        ppocr_models::set_models_root_for_test(models.path());
        const auto restore = qScopeGuard([] {
            release_ocr_engine();
            ppocr_models::set_models_root_for_test({});
            set_oneocr_available_for_test(std::nullopt);
            set_ppocr_model({});
            set_ocr_engine(OCRManager::ENGINE_AUTO);
        });
        QVERIFY(!set_ocr_engine(OCRManager::ENGINE_PP_RUST));
        auto result = recognize_text(text_image("x"));
        QCOMPARE(result.code, -1);
        QCOMPARE(result.reason, QString("no_model"));
        // A model without the runtime points at the runtime, not at another model.
        write_placeholder_model(models.path());
        QVERIFY(!set_ocr_engine(OCRManager::ENGINE_PP_RUST));
        result = recognize_text(text_image("x"));
        QCOMPARE(result.code, -1);
        QCOMPARE(result.reason, QString("no_runtime"));
        QVERIFY(QDir(QDir(models.path()).filePath("placeholder")).removeRecursively());
        if (!ppocr_) return;
        const QDir pack(QDir(QCoreApplication::applicationDirPath()).filePath("models"));
        QVERIFY(QFile::copy(pack.filePath("onnxruntime.dll"), QDir(models.path()).filePath("onnxruntime.dll")));
        const auto model = ppocr_models::import_model("only", pack.filePath("PP-OCRv6_det_small.onnx"), pack.filePath("PP-OCRv6_rec_small.onnx"));
        QFile det(model.value("det").toString());
        QVERIFY(det.open(QIODevice::WriteOnly | QIODevice::Truncate));
        det.write("not a model");
        det.close();
        set_ppocr_model("only");
        QVERIFY(set_ocr_engine(OCRManager::ENGINE_PP_RUST));
        result = recognize_text(text_image("x"));
        QCOMPARE(result.code, -1);
        QCOMPARE(result.reason, QString("model_broken"));
    }

    // Switching engines closes the one no longer chosen.
    void switchingEnginesReleasesTheOldOne() {
        if (!ppocr_ || !get_available_engines().contains(OCRManager::ENGINE_ONEOCR)) QSKIP("needs both engines");
        auto &ocr = OCRManager::instance();
        QVERIFY(initialize_ocr(OCRManager::ENGINE_PP_RUST));
        QVERIFY(ocr.is_engine_loaded());
        QVERIFY(set_ocr_engine(OCRManager::ENGINE_ONEOCR));
        QVERIFY(!ocr.is_engine_loaded());
        QVERIFY(initialize_ocr());
        QVERIFY(get_ocr_memory_status().contains(QString::fromUtf8("截图工具")));
        QVERIFY(recognized_text(recognize_text(text_image("Snipping"))).contains("Snipping"));
        QVERIFY(set_ocr_engine(OCRManager::ENGINE_PP_RUST));
        QVERIFY(!ocr.is_engine_loaded());
        QVERIFY(set_ocr_engine(OCRManager::ENGINE_AUTO));
    }
};

QTEST_MAIN(OcrManagerTests)
#include "test_ocr_manager.moc"
