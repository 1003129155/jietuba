#include "core/constants.h"
#include "canvas/selection_model.h"
#include "core/last_capture_region.h"
#include "ui/toolbar_layout.h"
#include "settings/tool_settings.h"
#include "core/ui_scale.h"
#include "core/export.h"
#include "settings/color_formats.h"
#include "core/save.h"
#include <QThreadPool>
#include <QSemaphore>
#include <QCryptographicHash>
#include <QGraphicsPixmapItem>
#include <QGraphicsRectItem>
#include <QFile>
#include <QImageReader>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QDir>

using namespace jietuba;
namespace {
QJsonArray rect(const QRectF &r) { return {r.x(), r.y(), r.width(), r.height()}; }
QRectF rectangle(const QJsonValue &v) {
    const auto a = v.toArray();
    return {a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble()};
}
}
class ParityTests : public QObject {
    Q_OBJECT
    QJsonObject fixtures_;
private slots:
    void initTestCase() {
        QFile file(JIETUBA_FIXTURE_PATH);
        QVERIFY(file.open(QIODevice::ReadOnly));
        fixtures_ = QJsonDocument::fromJson(file.readAll()).object();
        QVERIFY(!fixtures_.isEmpty());
    }
    void selectionStateAndSignalTrace() {
        for (const auto &sequence : fixtures_["selection"].toArray()) {
            SelectionModel model;
            QJsonArray events;
            auto record = [&](const QString &signal, QJsonValue value) {
                events.append(QJsonObject{{"signal", signal}, {"value", value},
                    {"active", model.is_active()}, {"confirmed", model.is_confirmed()}, {"dragging", model.is_dragging()}});
            };
            connect(&model, &SelectionModel::rectChanged, this, [&](QRectF r) { record("rect", rect(r)); });
            connect(&model, &SelectionModel::confirmed, this, [&](QRectF r) { record("confirmed", rect(r)); });
            connect(&model, &SelectionModel::draggingChanged, this, [&](bool v) { record("dragging", v); });
            for (const auto &v : sequence.toArray()) {
                const auto expected = v.toObject(), action = expected["action"].toObject();
                const auto op = action["op"].toString();
                events = {};
                QJsonValue caller(QJsonValue::Null);
                if (op == "set") { auto r = rectangle(action["rect"]); model.set_rect(r); caller = rect(r); }
                else if (op == "initialize") model.initialize_confirmed_rect(rectangle(action["rect"]));
                else if (op == "activate") model.activate();
                else if (op == "deactivate") model.deactivate();
                else if (op == "confirm") model.confirm();
                else if (op == "normalize") model.normalize();
                else if (op == "start_dragging") model.start_dragging();
                else if (op == "stop_dragging") model.stop_dragging();
                else if (op == "mutate_copy") { auto r = model.rect(); r.setWidth(999); }
                else QFAIL("Unknown oracle action");
                QCOMPARE(caller, expected["caller"]);
                QCOMPARE(rect(model.rect()), expected["rect"].toArray());
                QCOMPARE(model.is_active(), expected["active"].toBool());
                QCOMPARE(model.is_confirmed(), expected["confirmed"].toBool());
                QCOMPARE(model.is_dragging(), expected["dragging"].toBool());
                QCOMPARE(model.is_empty(), expected["empty"].toBool());
                QCOMPARE(events, expected["events"].toArray());
            }
        }
    }
    void lastRegionCopiesAndLifetime() {
        reset_last_region_for_test();
        QVERIFY(!get_last_region());
        QRect input(-400, -200, 300, 200);
        set_last_region(input);
        input.setWidth(999);
        QCOMPARE(get_last_region()->width(), 300);
        auto output = get_last_region();
        output->setHeight(999);
        QCOMPARE(get_last_region()->height(), 200);
        set_last_region({500, 500, 50, 50});
        QCOMPARE(*get_last_region(), QRect(500, 500, 50, 50));
        // Do not normalize invalid rectangles: the Python module is a value store.
        set_last_region({-100, 20, -5, 0});
        QCOMPARE(*get_last_region(), QRect(-100, 20, -5, 0));
        reset_last_region_for_test();
    }
    void toolbarNormalization() {
        for (const auto &v : fixtures_["layout"].toArray()) {
            const auto row = v.toObject();
            QCOMPARE(toolbar_layout::normalize_layout_json(row["input_json"].toString().toUtf8()), row["expected"].toArray());
        }
    }
    void pinToolbarNormalization() {
        for (const auto &v : fixtures_["pin_layout"].toArray()) {
            const auto row = v.toObject();
            QCOMPARE(toolbar_layout::normalize_layout_json(row["input_json"].toString().toUtf8(), toolbar_layout::PIN), row["expected"].toArray());
        }
    }
    void scaleRoundingAndDpi() {
        for (const auto &v : fixtures_["scale"].toArray()) {
            const auto row = v.toObject();
            QCOMPARE(UIScaleManager::normalize_percent(row["input"].toVariant()), row["expected"].toInt());
        }
        UIScaleManager scale;
        DialogScaleManager dialog;
        for (const auto &v : fixtures_["pixels"].toArray()) {
            const auto row = v.toObject();
            scale.set_percent(row["percent"].toInt());
            dialog.set_percent(row["percent"].toInt());
            QCOMPARE(scale.px(row["base"].toDouble()), row["px"].toInt());
            QCOMPARE(dialog.px(row["base"].toDouble()), row["px"].toInt());
            QCOMPARE(scale.pxf(row["base"].toDouble()), row["pxf"].toDouble());
        }
        for (const auto &v : fixtures_["dpi"].toArray()) {
            const auto row = v.toObject();
            QCOMPARE(recommended_scale_percent(row["input"].toVariant()), row["expected"].toInt());
        }
    }
    void defaultsAndToolCoercion() {
        QTemporaryDir temp;
        QSettings settings(temp.filePath("defaults.ini"), QSettings::IniFormat);
        ToolSettingsManager config(settings);
        const auto app = fixtures_["app_reads"].toObject();
        for (auto it = app.begin(); it != app.end(); ++it) {
            if (it.key() == "screenshot_save_path") {
                QCOMPARE(config.get_app_setting(it.key()).toString(), QDir::toNativeSeparators(QDir::homePath() + "/Pictures/jietuba_photos"));
            } else if (it.key() == "log_dir") {
                // The fixture holds a placeholder: the folder depends on the machine.
                QCOMPARE(config.get_app_setting(it.key()).toString(), QDir::toNativeSeparators(get_log_dir()));
            } else {
                const auto actual = QJsonValue::fromVariant(config.get_app_setting(it.key()));
                QVERIFY2(actual == it.value(), qPrintable(it.key() + ": " + QString::fromUtf8(QJsonDocument(QJsonArray{actual, it.value()}).toJson())));
            }
        }
        int index = 0;
        for (const auto &v : fixtures_["tool_coercion"].toArray()) {
            const auto row = v.toObject();
            QSettings qs(temp.filePath(QString("coerce%1.ini").arg(index++)), QSettings::IniFormat);
            const auto tool = row["tool"].toString(), key = row["key"].toString();
            qs.setValue("tools/" + tool + "/" + key, row["raw"].toVariant());
            ToolSettingsManager model(qs);
            QCOMPARE(QJsonValue::fromVariant(model.get_setting(tool, key)), row["expected"]);
        }
    }
    void configurationKeysPersistenceAndSignals() {
        QTemporaryDir temp;
        QSettings qs(temp.filePath("config.ini"), QSettings::IniFormat);
        ToolSettingsManager model(qs);
        QSignalSpy spy(&model, &ToolSettingsManager::settings_changed);
        model.set_setting("pen", "stroke_width", 21, false);
        QCOMPARE(spy.count(), 1);
        QVERIFY(!qs.contains("tools/pen/stroke_width"));
        model.save_all();
        QCOMPARE(qs.value("tools/pen/stroke_width").toInt(), 21);
        model.set_setting("missing", "stroke_width", 1);
        QCOMPARE(spy.count(), 1);
        model.reset_tool("pen");
        QCOMPARE(model.get_stroke_width("pen"), 12.0);
        QCOMPARE(spy.count(), 2);
        model.set_app_setting("pin_auto_toolbar", false);
        QVERIFY(qs.contains("pin/auto_toolbar"));
        QVERIFY(!qs.contains("app/pin_auto_toolbar"));
        model.set_inapp_shortcut("inapp_confirm", "ctrl+enter");
        QCOMPARE(qs.value("inapp/inapp_confirm").toString(), QString("ctrl+enter"));
        model.reset_app_settings();
        QCOMPARE(model.get_inapp_shortcut("inapp_confirm"), QString("ctrl+c"));
        QCOMPARE(model.get_hotkey(), QString("ctrl+1"));
        // reset_app_settings uses app/clipboard_hotkey; the dedicated accessor
        // uses clipboard/hotkey in the reference. Preserve both namespaces.
        qs.setValue("clipboard/hotkey", "alt+2");
        model.reset_app_settings();
        QCOMPARE(model.get_clipboard_hotkey(), QString("alt+2"));
        model.import_settings({{"pen", QVariantMap{{"stroke_width", 8}, {"unknown", 99}}}});
        QCOMPARE(model.get_stroke_width("pen"), 8.0);
        QCOMPARE(model.get_setting("pen", "unknown").toInt(), 99);
        qs.sync();
        ToolSettingsManager reopened(qs);
        QCOMPARE(reopened.get_stroke_width("pen"), 8.0);
    }
    void exactGeneratedAccessors() {
        QTemporaryDir temp;
        QSettings qs(temp.filePath("accessors.ini"), QSettings::IniFormat);
        ToolSettingsManager config(qs);
        auto actual = QJsonObject::fromVariantMap(config.generated_accessor_values());
        auto expected = fixtures_["accessor_reads"].toObject();
        // The fixture holds a placeholder for the log folder; the real one depends on the machine.
        QCOMPARE(actual.take("get_log_dir").toString(), QDir::toNativeSeparators(get_log_dir()));
        expected.remove("get_log_dir");
        QCOMPARE(actual, expected);
        for (const auto &v : fixtures_["accessor_setters"].toArray()) {
            const auto row = v.toObject();
            QVERIFY(config.apply_generated_setter(row["name"].toString(), row["input"].toVariant()));
            QCOMPARE(QJsonValue::fromVariant(qs.value(row["path"].toString())), row["stored"]);
        }
        QVERIFY(!config.apply_generated_setter("not-a-source-method", true));
    }
    void firstRunAndIndependentScales() {
        QTemporaryDir temp;
        QSettings qs(temp.filePath("scale.ini"), QSettings::IniFormat);
        ToolSettingsManager config(qs);
        QCOMPARE(*apply_first_run_scale_defaults(config, 120), 125);
        config.set_app_setting("ui_scale_percent", 90);
        QCOMPARE(*apply_first_run_scale_defaults(config, 144), 125);
        QCOMPARE(config.get_app_setting("ui_scale_percent").toInt(), 90);
        QCOMPARE(config.get_app_setting("dialog_scale_percent").toInt(), 125);
        UIScaleManager scale;
        DialogScaleManager dialog;
        scale.init(&config);
        dialog.init(&config);
        QSignalSpy spy(&scale, &UIScaleManager::scale_changed);
        QVERIFY(!scale.set_percent(90));
        QVERIFY(scale.set_percent(150));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(dialog.percent(), 125);
        config.mark_as_run();
        QVERIFY(!apply_first_run_scale_defaults(config, 168));
        QWidget plain;
        QCOMPARE(widget_scaled(&plain, 2.5), 2); // ties-to-even, not qRound
        plain.setProperty("dialog_scale_factor", 1.5);
        QCOMPARE(widget_scaled(&plain, 3), 4);
        plain.setProperty("dialog_scale_factor", "invalid");
        QCOMPARE(widget_scaled(&plain, 3), 3);
        plain.setProperty("dialog_scale_factor", "0");
        QCOMPARE(widget_scaled(&plain, 3), 1);
        plain.setProperty("dialog_scale_factor", 0);
        QCOMPARE(widget_scaled(&plain, 3), 3);
    }
    void exportPixelsAndVisibility() {
        for (const auto &v : fixtures_["exports"].toArray()) {
            const auto row = v.toObject();
            QGraphicsScene scene;
            scene.setSceneRect(-50, -30, 32, 24);
            SelectionModel model;
            QImage image(32, 24, QImage::Format_ARGB32_Premultiplied);
            for (int y = 0; y < 24; ++y) for (int x = 0; x < 32; ++x) {
                image.setPixelColor(x, y, QColor((x * 17) % 256, (y * 23) % 256, (x + y) * 4, 255));
            }
            auto *background = scene.addPixmap(QPixmap::fromImage(image));
            background->setOffset(-50, -30);
            background->setZValue(0);
            auto *drawing = scene.addRect(QRectF(-45, -26, 8, 6), QPen(Qt::NoPen), QBrush(QColor(255, 0, 0, 128)));
            drawing->setZValue(20);
            auto *hidden = scene.addRect(QRectF(-40, -20, 6, 4), QPen(Qt::NoPen), QBrush(Qt::blue));
            hidden->setVisible(false);
            ExportService service(scene, model, background);
            const auto region = rectangle(row["region"]);
            for (const auto &kind : {QString("composite"), QString("base")}) {
                const auto result = kind == "base" ? service.export_base_image_only(region) : service.export_image(region);
                const auto expected = row[kind].toObject();
                QCOMPARE(result.width(), expected["width"].toInt());
                QCOMPARE(result.height(), expected["height"].toInt());
                const auto hash = result.isNull() ? QString() : QString::fromLatin1(QCryptographicHash::hash(
                    QByteArrayView(reinterpret_cast<const char *>(result.constBits()), result.sizeInBytes()), QCryptographicHash::Sha256).toHex());
                QCOMPARE(hash, expected["sha256"].toString());
            }
            QCOMPARE(drawing->isVisible(), row["drawing_visible"].toBool());
            QCOMPARE(hidden->isVisible(), row["hidden_visible"].toBool());
            QCOMPARE(service.get_result_pixmap().size(), QSize(32, 24));
        }
    }
    void colorFormatsAndLegacyPersistence() {
        for (const auto &v : fixtures_["color_render"].toArray()) {
            const auto row = v.toObject();
            const auto rgb = row["rgb"].toArray();
            const QColor color(rgb[0].toInt(), rgb[1].toInt(), rgb[2].toInt());
            QJsonArray actual;
            for (const auto &format : color_formats::default_formats()) actual.append(format.render(color));
            QCOMPARE(actual, row["expected"].toArray());
        }
        QTemporaryDir temp;
        int index = 0;
        for (const auto &v : fixtures_["color_loading"].toArray()) {
            const auto row = v.toObject();
            QSettings qs(temp.filePath(QString("color%1.ini").arg(index++)), QSettings::IniFormat);
            ToolSettingsManager config(qs);
            config.set_app_setting(color_formats::SETTING_KEY, row["raw"].toString());
            config.set_app_setting(color_formats::LEGACY_SETTING_KEY, row["legacy"].toString());
            const auto result = color_formats::load(&config);
            QJsonArray actual;
            for (const auto &format : result) actual.append(QJsonObject{{"name", format.name},
                {"template", format.template_string}, {"enabled", format.enabled}});
            QCOMPARE(actual, row["expected"].toArray());
            color_formats::save(&config, result);
            QCOMPARE(config.get_app_setting(color_formats::SETTING_KEY).toString(), row["saved"].toString());
        }
    }
    void saveFormatLimitsAndPdfPixels() {
        for (const auto &v : fixtures_["save_formats"].toArray()) {
            const auto row = v.toObject();
            QCOMPARE(SaveService::format_for_size(row["format"].toString(), row["width"].toInt(),
                row["height"].toInt(), row["dpi"].toInt()), row["expected"].toString());
        }
        for (const auto &v : fixtures_["pdf_flatten"].toArray()) {
            const auto row = v.toObject();
            QImage image(4, 3, QImage::Format_ARGB32);
            image.fill(QColor(255, 40, 80, row["alpha"].toInt()));
            const auto flattened = SaveService::flatten_for_pdf(image);
            const auto hash = QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char *>(flattened.constBits()),
                flattened.sizeInBytes()), QCryptographicHash::Sha256).toHex();
            QCOMPARE(QString::fromLatin1(hash), row["sha256"].toString());
        }
    }
    void saveReservationAsyncAndFailureCleanup() {
        QTemporaryDir temp;
        QSettings qs(temp.filePath("save.ini"), QSettings::IniFormat);
        ToolSettingsManager config(qs);
        config.set_screenshot_save_path(temp.path());
        config.set_screenshot_save_path("relative-path");
        QCOMPARE(config.get_screenshot_save_path(), temp.path());
        SaveService service(config);
        const auto stem = temp.filePath("collision");
        QCOMPARE(SaveService::claim_first_free(stem, ".png"), stem + ".png");
        QCOMPARE(SaveService::claim_first_free(stem, ".png"), stem + "_1.png");
        QCOMPARE(SaveService::claim_first_free(stem, ".png"), stem + "_2.png");
        QImage image(24, 16, QImage::Format_ARGB32);
        image.fill(Qt::red);
        const auto result = service.save_qimage(image, temp.path(), "prefix", "suffix", "PNG");
        QVERIFY(result.first);
        QVERIFY(result.second);
        QCOMPARE(QImage(*result.second).pixelColor(10, 10), QColor(Qt::red));
        const auto original = SaveService::save_beside(image, *result.second, "original");
        QVERIFY(original);
        QVERIFY(original->endsWith("_original.png"));
        const auto placeholder = SaveService::claim_first_free(temp.filePath("bad"), ".bad");
        QVERIFY(!SaveService::save_qimage_to_path(image, placeholder, "INVALID"));
        QVERIFY(!QFile::exists(placeholder));
        QFile keep(temp.filePath("nonempty.bad"));
        QVERIFY(keep.open(QIODevice::WriteOnly));
        keep.write("do-not-delete");
        keep.close();
        SaveService::cleanup_failed_placeholder(keep.fileName());
        QVERIFY(QFile::exists(keep.fileName()));
        QVERIFY(!service.save_qimage(QImage()).second);
        QSemaphore ready;
        bool success = false;
        QString callbackPath;
        const auto asyncPath = service.save_qimage_async(image, temp.path(), "async", {}, "PNG", 300,
            [&](bool value, const QString &path) { success = value; callbackPath = path; ready.release(); });
        QVERIFY(asyncPath);
        QVERIFY(ready.tryAcquire(1, 5000));
        QVERIFY(success);
        QCOMPARE(callbackPath, *asyncPath);
        QCOMPARE(QImage(*asyncPath).size(), image.size());
        const auto pdf = temp.filePath("capture.pdf");
        QVERIFY(SaveService::save_qimage_to_path(image, pdf, "PDF", 300));
        QFile pdfFile(pdf);
        QVERIFY(pdfFile.open(QIODevice::ReadOnly));
        QVERIFY(pdfFile.readAll().startsWith("%PDF-"));
        QThreadPool::globalInstance()->waitForDone();
    }
    void offeredFormatsSaveAndImportedFormatsDecode() {
        // The formats the save dialogs and settings offer, and the suffixes the clipboard image form accepts.
        QTemporaryDir temp;
        QImage image(24, 16, QImage::Format_ARGB32);
        image.fill(QColor(30, 120, 200));
        for (const char *format : {"PNG", "JPG", "BMP", "WEBP"}) {
            const auto path = temp.filePath(QString("shot.%1").arg(QString(format).toLower()));
            QVERIFY2(SaveService::save_qimage_to_path(image, path, format), format);
            QCOMPARE(QImage(path).size(), image.size());
        }
        const auto readable = QImageReader::supportedImageFormats();
        for (const char *suffix : {"png", "jpg", "jpeg", "bmp", "gif", "webp", "tif", "tiff", "ico"})
            QVERIFY2(readable.contains(suffix), suffix);
    }
    void toolbarSerializationAndResetSignalOrder() {
        QTemporaryDir temp;
        QSettings qs(temp.filePath("ordering.ini"), QSettings::IniFormat);
        ToolSettingsManager config(qs);
        const auto expected = toolbar_layout::default_layout();
        QCOMPARE(toolbar_layout::save_layout(config, QJsonArray()), expected);
        auto serialization = QJsonDocument(expected).toJson(QJsonDocument::Compact);
        serialization.replace(",", ", ");
        QCOMPARE(config.get_app_setting(toolbar_layout::SETTING_KEY).toString(), QString::fromUtf8(serialization));
        QCOMPARE(toolbar_layout::load_layout(config), expected);
        config.set_app_setting(toolbar_layout::SETTING_KEY, "{not-json");
        QCOMPARE(toolbar_layout::load_layout(config), expected);
        QStringList order;
        connect(&config, &ToolSettingsManager::settings_changed, this, [&](const QString &tool, const QVariantMap &) { order.append(tool); });
        config.reset_all();
        QCOMPARE(order, QStringList({"pen", "highlighter", "mosaic", "spotlight", "watermark", "rect", "ellipse", "arrow", "text", "number", "eraser"}));
    }
};
QTEST_MAIN(ParityTests)
#include "test_parity.moc"
