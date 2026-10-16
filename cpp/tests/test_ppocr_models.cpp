#include "core/native.h"
#include "ocr/ppocr_models.h"
#include "ui/settings_ui/ppocr_model_dialog.h"
#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <stdexcept>

using namespace jietuba;

namespace {
// Stands in for the updater helper: records the start, and the test emits its events.
class FakeRunner : public UpdaterProcess {
public:
    bool fake_busy = false;
    QPair<QString, QStringList> started;
    bool cancelled = false;
    bool busy() const override { return fake_busy; }
    bool start(const QString &command, const QStringList &arguments, int) override {
        started = {command, arguments};
        fake_busy = true;
        return true;
    }
    void cancel() override { cancelled = true; }
    void event(const QString &name, const QJsonObject &data = {}) { emit event_received(name, transaction, data); }
    void finish() {
        fake_busy = false;
        emit finished();
    }
};

void write_file(const QString &path, qint64 size) {
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArray(size, '\0'));
}

// A model folder with fake files: listing only checks that the files exist.
void write_model(const QString &root, const QString &id, const QString &name) {
    write_file(QDir(root).filePath(id + "/det.onnx"), 1);
    write_file(QDir(root).filePath(id + "/rec.onnx"), 1);
    QFile manifest(QDir(root).filePath(id + "/model.json"));
    QVERIFY(manifest.open(QIODevice::WriteOnly));
    manifest.write(QJsonDocument(QJsonObject{{"name", name}, {"det", "det.onnx"}, {"rec", "rec.onnx"}}).toJson());
}

// Clicks Yes on the next modal confirmation.
void confirm_next_dialog() {
    QTimer::singleShot(0, [] {
        auto *dialog = QApplication::activeModalWidget();
        auto *box = dialog ? dialog->findChild<QDialogButtonBox *>() : nullptr;
        if (!box) return;
        for (auto *button : box->buttons())
            if (box->buttonRole(button) == QDialogButtonBox::YesRole) button->click();
    });
}
}

// PP-OCR model storage, downloads through the updater and the model manager, like the
// reference's tests/test_ppocr_models.py. A temporary folder stands in for models/.
// Importing loads real models: JIETUBA_PPOCR_MODELS names a folder with the default model
// and an onnxruntime.dll (1.24 or later); without it those cases are skipped.
class PpocrModelsTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir dir_;
    QString root_;
    QString pack_;
    bool runtime_ = false;
    // The default model's files; listing only checks that they exist.
    void install_default() {
        for (const char *name : {"PP-OCRv6_det_small.onnx", "PP-OCRv6_rec_small.onnx"}) write_file(QDir(root_).filePath(name), 1);
    }
private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        pack_ = qEnvironmentVariable("JIETUBA_PPOCR_MODELS");
        if (!pack_.isEmpty() && QDir(pack_).exists("onnxruntime.dll")) {
            char error[512] = {};
            runtime_ = jb_ppocr_init_runtime(QDir::toNativeSeparators(QDir(pack_).filePath("onnxruntime.dll")).toUtf8().constData(), error, sizeof error);
            QVERIFY2(runtime_, error);
        }
    }

    void init() {
        root_ = dir_.filePath(QString("models-%1").arg(QTest::currentTestFunction()));
        QDir(root_).removeRecursively();
        QVERIFY(QDir().mkpath(root_));
        ppocr_models::set_models_root_for_test(root_);
    }

    void cleanup() { ppocr_models::set_models_root_for_test({}); }

    void catalogMarksOneDefault() {
        QStringList defaults;
        for (const auto &value : ppocr_models::catalog().value("models").toArray())
            if (value.toObject().value("default").toBool()) defaults << value.toObject().value("id").toString();
        QCOMPARE(defaults, QStringList{"ppocr-v6-small"});
        QCOMPARE(ppocr_models::catalog_entry("ppocr-v5-korean")->value("languages").toArray(), (QJsonArray{"ko", "en"}));
        QVERIFY(!ppocr_models::catalog_entry("missing"));
    }

    void importListsAndRemovesALocalModel() {
        if (!runtime_) QSKIP("set JIETUBA_PPOCR_MODELS to a folder with the default model and onnxruntime.dll");
        QVERIFY(ppocr_models::installed().isEmpty());
        const auto model = ppocr_models::import_model(QString::fromUtf8("我的模型"), QDir(pack_).filePath("PP-OCRv6_det_small.onnx"), QDir(pack_).filePath("PP-OCRv6_rec_small.onnx"));
        QCOMPARE(model.value("id").toString(), QString::fromUtf8("我的模型"));
        QVERIFY(model.value("catalog_id").isNull());
        QCOMPARE(ppocr_models::installed().size(), 1);
        QCOMPARE(QDir::fromNativeSeparators(ppocr_models::resolve(QString::fromUtf8("我的模型"))->value("rec").toString()),
                 QDir(root_).filePath(QString::fromUtf8("我的模型/PP-OCRv6_rec_small.onnx")));
        ppocr_models::remove(QString::fromUtf8("我的模型"));
        QVERIFY(ppocr_models::installed().isEmpty());
        QVERIFY(!ppocr_models::resolve(QString::fromUtf8("我的模型")));
    }

    void invalidModelIsRejectedWithoutLeftovers() {
        if (!runtime_) QSKIP("set JIETUBA_PPOCR_MODELS to a folder with the default model and onnxruntime.dll");
        const QString broken = dir_.filePath("broken.onnx");
        write_file(broken, 16);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, ppocr_models::import_model("broken", broken, QDir(pack_).filePath("PP-OCRv6_rec_small.onnx")));
        QVERIFY(QDir(root_).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());
    }

    void defaultModelCanBeRemovedAndAutoMovesOn() {
        install_default();
        write_model(root_, "local", "local");
        QCOMPARE(ppocr_models::resolve("")->value("catalog_id").toString(), QString("ppocr-v6-small"));
        // Catalog models come before ones added by hand.
        QCOMPARE(ppocr_models::auto_choice()->value("id").toString(), QString());
        ppocr_models::remove("");
        QVERIFY(!ppocr_models::resolve(""));
        QCOMPARE(ppocr_models::auto_choice()->value("id").toString(), QString("local"));
        ppocr_models::remove("local");
        QVERIFY(!ppocr_models::auto_choice());
    }

    void downloadRequestNamesTheCatalogFiles() {
        ModelDownload download;
        auto *runner = new FakeRunner;
        runner->setParent(&download);
        download.set_runner(runner);
        QVERIFY(download.start("ppocr-v5-korean"));
        QCOMPARE(runner->started.first, QString("download-model"));
        const QStringList arguments = runner->started.second;
        const QString request_path = arguments.at(arguments.indexOf("--model-file") + 1);
        QFile request(request_path);
        QVERIFY(request.open(QIODevice::ReadOnly));
        const auto entry = *ppocr_models::catalog_entry("ppocr-v5-korean");
        QCOMPARE(QJsonDocument::fromJson(request.readAll()).object(), (QJsonObject{{"staging", "ppocr-v5-korean"}, {"files", entry.value("files")}}));
        request.close();
        QCOMPARE(QFileInfo(arguments.at(arguments.indexOf("--install-exe") + 1)).fileName(), QFileInfo(QCoreApplication::applicationFilePath()).fileName());
        runner->finish();
        QVERIFY(!QFileInfo::exists(request_path));
        QVERIFY(!download.start("missing"));
    }

    void downloadedFilesArePlacedAndReported() {
        ModelDownload download;
        auto *runner = new FakeRunner;
        runner->setParent(&download);
        download.set_runner(runner);
        QSignalSpy finished(&download, &ModelDownload::finished);
        QSignalSpy stopped(&download, &ModelDownload::stopped);
        QVERIFY(download.start("ppocr-v6-tiny"));
        for (const auto &file : ppocr_models::catalog_entry("ppocr-v6-tiny")->value("files").toArray())
            write_file(QDir(root_).filePath(".download-ppocr-v6-tiny/" + file.toObject().value("name").toString()), file.toObject().value("size").toVariant().toLongLong());
        runner->event("downloaded");
        runner->finish();
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().first().toJsonObject().value("id").toString(), QString("ppocr-v6-tiny"));
        QCOMPARE(stopped.size(), 1);
        QCOMPARE(ppocr_models::resolve("ppocr-v6-tiny")->value("catalog_id").toString(), QString("ppocr-v6-tiny"));
    }

    // The runtime comes out of the official onnxruntime wheel; the request carries the wheel's
    // size, hash and member, and the finished DLL goes into models/ itself.
    void runtimeDownloadTakesTheDllFromTheWheel() {
        ModelDownload download;
        auto *runner = new FakeRunner;
        runner->setParent(&download);
        download.set_runner(runner);
        QSignalSpy finished(&download, &ModelDownload::finished);
        QVERIFY(download.start(ModelDownload::RUNTIME));
        const QStringList arguments = runner->started.second;
        QFile request(arguments.at(arguments.indexOf("--model-file") + 1));
        QVERIFY(request.open(QIODevice::ReadOnly));
        const auto file = QJsonDocument::fromJson(request.readAll()).object().value("files").toArray().first().toObject();
        request.close();
        QCOMPARE(file.value("name").toString(), QString("onnxruntime.dll"));
        QCOMPARE(file.value("archive").toObject().value("member").toString(), QString("onnxruntime/capi/onnxruntime.dll"));
        QVERIFY(file.value("urls").toArray().first().toString().endsWith(".whl"));
        write_file(QDir(root_).filePath(".download-runtime/onnxruntime.dll"), file.value("size").toVariant().toLongLong());
        runner->event("downloaded");
        runner->finish();
        QCOMPARE(finished.size(), 1);
        QVERIFY(QFileInfo::exists(ppocr_models::runtime_path()));
        QVERIFY(!QDir(root_).exists(".download-runtime"));
    }

    void errorCodesGetModelWording() {
        ModelUpdaterProcess runner;
        QVERIFY(runner.error_text({{"code", "network"}}).contains("network"));
        QVERIFY(runner.error_text({{"code", "hash"}}).contains("damaged"));
        QVERIFY(runner.error_text({{"code", "cancelled"}}).contains("cancelled"));
    }

    void dialogRowsFollowCatalogAndInstalledState() {
        install_default();
        write_model(root_, "local", "local");
        PpocrModelDialog dialog;
        const auto rows = dialog.rows();
        // The runtime row comes first: the temporary folder has no onnxruntime.dll, and the catalog
        // offers one for this architecture.
        QCOMPARE(rows.first().title, QString("ONNX Runtime"));
        QCOMPARE(rows.first().action, QString("Download"));
        // The size is what gets downloaded: the wheel, not the DLL inside it.
        const auto wheel = ppocr_models::runtime_entry()->value("file").toObject().value("archive").toObject();
        QCOMPARE(rows.first().detail, QString("Needed by PP-OCR · %1 MB · Not downloaded").arg(wheel.value("size").toDouble() / 1048576.0, 0, 'f', 1));
        QCOMPARE(rows.size(), 1 + ppocr_models::catalog().value("models").toArray().size() + 1);
        const auto row = [&rows](const QString &title) { return *std::find_if(rows.begin(), rows.end(), [&title](const auto &r) { return r.title == title; }); };
        QCOMPARE(row("PP-OCRv6 small").action, QString("Delete"));
        QVERIFY(row("PP-OCRv6 small").detail.endsWith("Included"));
        QCOMPARE(row("PP-OCRv5 Korean").action, QString("Download"));
        QVERIFY(row("PP-OCRv5 Korean").detail.contains("Korean"));
        QCOMPARE(row("local").detail, QString("Added manually"));
        QCOMPARE(row("local").action, QString("Delete"));
    }

    void dialogDownloadProgressAndCloseWaitsForCancel() {
        install_default();
        PpocrModelDialog dialog;
        auto *runner = new FakeRunner;
        runner->setParent(&dialog);
        dialog.download()->set_runner(runner);
        auto rows = dialog.rows();
        auto korean = std::find_if(rows.begin(), rows.end(), [](const auto &r) { return r.title == "PP-OCRv5 Korean"; });
        korean->button->click();
        QCOMPARE(runner->started.first, QString("download-model"));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        const auto downloading_row = [&dialog] {
            const auto all = dialog.rows();
            return *std::find_if(all.begin(), all.end(), [](const auto &r) { return r.title == "PP-OCRv5 Korean"; });
        };
        QCOMPARE(downloading_row().action, QString("Cancel"));
        QVERIFY(downloading_row().button->isEnabled());
        for (const auto &r : dialog.rows())
            if (r.button && r.title != "PP-OCRv5 Korean") QVERIFY(!r.button->isEnabled());
        std::vector<double> times{0.0, 0.2, 2.0};
        size_t tick = 0;
        dialog.set_clock_for_test([&] { return times[tick++]; });
        emit dialog.download()->progress(0, 20 * 1048576);
        emit dialog.download()->progress(1048576, 20 * 1048576);
        QCOMPARE(downloading_row().progress_text, QString("1.0 MB / 20.0 MB"));  // under half a second: no speed yet
        emit dialog.download()->progress(10 * 1048576, 20 * 1048576);
        QCOMPARE(downloading_row().progress, 500);
        QCOMPARE(downloading_row().progress_text, QString("10.0 MB / 20.0 MB · 5.0 MB/s"));
        QSignalSpy closed(&dialog, &QDialog::finished);
        dialog.reject();
        QVERIFY(runner->cancelled);
        QCOMPARE(closed.size(), 0);
        runner->finish();
        QCOMPARE(closed.size(), 1);
    }

    void dialogDeletesLocalModels() {
        write_model(root_, "local", "local");
        PpocrModelDialog dialog;
        QSignalSpy changed(&dialog, &PpocrModelDialog::models_changed);
        QSignalSpy removed(&dialog, &PpocrModelDialog::model_removed);
        dialog.show_status("before");
        auto rows = dialog.rows();
        auto local = std::find_if(rows.begin(), rows.end(), [](const auto &r) { return r.title == "local"; });
        confirm_next_dialog();
        local->button->click();
        QVERIFY(!QDir(root_).exists("local"));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(removed.size(), 1);
        QCOMPARE(removed.first().first().toString(), QString("local"));
        QVERIFY(dialog.status_text().isEmpty());
        for (const auto &r : dialog.rows()) QVERIFY(r.title != "local");
    }

    // Downloads and deletions rebuild the list; the view stays where it was.
    void dialogKeepsTheScrollPositionWhenRefilled() {
        install_default();
        PpocrModelDialog dialog;
        dialog.resize(560, 300);
        dialog.show();
        QCoreApplication::processEvents();
        auto *bar = dialog.findChild<QScrollArea *>()->verticalScrollBar();
        QVERIFY(bar->maximum() > 0);
        bar->setValue(bar->maximum() / 2);
        const int position = bar->value();
        QVERIFY(position > 0);
        emit dialog.import_done(QJsonObject{{"name", "x"}}, {});
        QTRY_VERIFY(dialog.status_text().contains("x"));
        QCoreApplication::processEvents();
        QCOMPARE(bar->value(), position);
    }

    void dialogReportsImportResults() {
        PpocrModelDialog dialog;
        QSignalSpy changed(&dialog, &PpocrModelDialog::models_changed);
        emit dialog.import_done(QJsonObject{{"name", "x"}}, {});
        QTRY_VERIFY(dialog.status_text().contains("x"));
        QCOMPARE(changed.size(), 1);
    }
};

QTEST_MAIN(PpocrModelsTests)
#include "test_ppocr_models.moc"
