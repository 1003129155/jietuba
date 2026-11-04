#include "barcode/reader.h"
#include "barcode/result_window.h"
#include "core/i18n.h"
#include "core/ui_scale.h"
#include "ocr/ocr_manager.h"
#include "ocr/ppocr_models.h"
#include "settings/tool_settings.h"
#include "text_recognition/original_image_view.h"
#include "text_recognition/recognizer.h"
#include "text_recognition/result_window.h"
#include "text_recognition/table_view.h"
#include "ui/fluent_lite/fluent_lite.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/titlebar.h"
#include "ui/fluent_lite/labels.h"
#include "caption_check.h"
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QFrame>
#include <QHeaderView>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScopeGuard>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QSplitter>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTranslator>
#include <QWheelEvent>
#include <functional>

using namespace jietuba;

namespace {
QString font_family;

QImage text_image(const QString &text, const QSize &size = QSize(420, 90)) {
    QImage image(size, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setPen(Qt::black);
    painter.setFont(QFont(font_family, 28));
    painter.drawText(image.rect(), Qt::AlignCenter, text);
    painter.end();
    return image;
}

QImage blank_image() {
    QImage image(200, 80, QImage::Format_RGB32);
    image.fill(Qt::white);
    return image;
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

// A drawn three-by-three table with rules, large enough for either engine.
QImage table_image() {
    QImage image(600, 200, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setPen(Qt::black);
    for (int y : {10, 70, 130, 190}) painter.drawLine(10, y, 590, y);
    for (int x : {10, 200, 400, 590}) painter.drawLine(x, 10, x, 190);
    painter.setFont(QFont(font_family, 24));
    const QStringList cells = {"Name", "Qty", "Price", "Apple", "12", "3.50", "Pear", "7", "1.20"};
    for (int k = 0; k < cells.size(); ++k) painter.drawText(QRect(10 + 190 * (k % 3) + 20, 10 + 60 * (k / 3), 170, 60), Qt::AlignVCenter | Qt::AlignLeft, cells[k]);
    painter.end();
    return image;
}

text_table::Table small_table() { return {2, 2, {{0, 0, 1, 1, "Name"}, {0, 1, 1, 1, "Qty"}, {1, 0, 1, 1, "Pear"}, {1, 1, 1, 1, "7"}}}; }

// The first line stops short of the right edge, so the layout keeps the two apart.
const QVector<OcrTextLine> SMALL_LINES = {{"Name Qty", 0, 60, 0, 20}, {"Pear 7", 0, 100, 30, 50}};

// Two recognized lines on a 400 × 200 image: "Hello world" across the top, "第二行" below it.
OcrResult two_line_result() {
    OcrResult result;
    result.data = {OcrLine{{{10, 10}, {230, 10}, {230, 40}, {10, 40}}, "Hello world", 1},
                   OcrLine{{{10, 60}, {100, 60}, {100, 90}, {10, 90}}, QString::fromUtf8("第二行"), 1}};
    return result;
}

QImage plain_image(int width = 400, int height = 200) {
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(Qt::white);
    return image;
}

QRectF area_bounds(const QVector<QPolygonF> &areas) {
    QRectF bounds;
    for (const auto &area : areas) bounds = bounds.isNull() ? area.boundingRect() : bounds.united(area.boundingRect());
    return bounds;
}

// Runs check on the next popup menu, then closes it.
void inspect_next_menu(std::function<void(QMenu *)> check) {
    QTimer::singleShot(0, qApp, [check] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) return;
        check(menu);
        menu->close();
    });
}

// Picks the menu entry whose text starts with label from the next popup menu.
void choose_from_next_menu(const QString &label) {
    QTimer::singleShot(0, qApp, [label] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) return;
        for (auto *action : menu->actions())
            if (action->text().startsWith(label)) menu->setActiveAction(action);
        QTest::keyClick(menu, Qt::Key_Return);
    });
}
}

class ResultWindowTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir dir_;
    QSettings *settings_ = nullptr;
    ToolSettingsManager *config_ = nullptr;
    QImage codes_image_;

private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        settings_ = new QSettings(dir_.filePath("settings.ini"), QSettings::IniFormat, this);
        config_ = get_tool_settings_manager(settings_);
        // The offscreen platform has no font directory; recognizable text needs a real font.
        const int font_id = QFontDatabase::addApplicationFont(QDir(qEnvironmentVariable("WINDIR")).filePath("Fonts/arial.ttf"));
        QVERIFY(font_id >= 0);
        font_family = QFontDatabase::applicationFontFamilies(font_id).value(0);
        codes_image_ = QImage(JIETUBA_BARCODE_FIXTURE_DIR "/barcodes.png");
        QVERIFY(!codes_image_.isNull());
    }

    void init() { settings_->clear(); }

    void cleanup() {
        for (auto *widget : QApplication::topLevelWidgets()) widget->close();
        shutdown_recognition(10000);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    // ---- Text recognition ----

    void recognizedTextLandsInAnEditableBoxAndCopies() {
        if (!is_ocr_available()) QSKIP("no OCR engine on this machine");
        auto *window = show_text_recognition(text_image("Hello 2026"), QRect(100, 100, 420, 90));
        QVERIFY(window);
        QPointer<TextRecognitionWindow> alive = window;
        // It waits in a recognizing state first.
        QVERIFY(!window->copy_button->isEnabled());
        QVERIFY(!window->layout_button->isEnabled() || window->text_edit->toPlainText().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(window->copy_button->isEnabled(), 30000);
        QVERIFY(window->text_edit->toPlainText().contains("Hello"));
        QVERIFY(!window->text_edit->isReadOnly());
        QVERIFY(window->status_label->text().contains(QString::number(window->text_edit->toPlainText().size())));
        // The edited text is what gets copied.
        window->text_edit->setPlainText("edited text");
        window->copy_button->click();
        QCOMPARE(QApplication::clipboard()->text(), QString("edited text"));
        QCOMPARE(window->copy_button->text(), QString("Copied"));
        // The window lives until it is closed.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(alive);
        window->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!alive);
    }

    void aBlankImageSaysWhyAndLeavesCopyDisabled() {
        if (!is_ocr_available()) QSKIP("no OCR engine on this machine");
        auto *window = show_text_recognition(blank_image());
        QTRY_VERIFY_WITH_TIMEOUT(window->status_label->text().contains("No text found"), 30000);
        QVERIFY(!window->copy_button->isEnabled());
    }

    void missingModelsSayWhereToGetThem() {
        QTemporaryDir models;
        QVERIFY(models.isValid());
        ppocr_models::set_models_root_for_test(models.path());
        set_oneocr_available_for_test(false);
        const auto restore = qScopeGuard([] {
            ppocr_models::set_models_root_for_test({});
            set_oneocr_available_for_test(std::nullopt);
        });
        auto *window = show_text_recognition(blank_image());
        QTRY_VERIFY_WITH_TIMEOUT(window->status_label->text().contains("No PP-OCR model is installed"), 30000);
        QVERIFY(!window->copy_button->isEnabled());
        window->close();
    }

    // A model is installed but the runtime is not: the window names the runtime, in every language.
    void missingRuntimeSaysWhereToGetIt() {
        QTemporaryDir models;
        QVERIFY(models.isValid());
        write_placeholder_model(models.path());
        ppocr_models::set_models_root_for_test(models.path());
        set_oneocr_available_for_test(false);
        const auto restore = qScopeGuard([] {
            ppocr_models::set_models_root_for_test({});
            set_oneocr_available_for_test(std::nullopt);
        });
        const std::vector<std::pair<QString, QString>> languages = {
            {"en", "ONNX Runtime, which PP-OCR needs, is not installed"},
            {"zh", QString::fromUtf8("没有安装 PP-OCR 所需的 ONNX Runtime")},
            {"zh_TW", QString::fromUtf8("沒有安裝 PP-OCR 所需的 ONNX Runtime")},
            {"ja", QString::fromUtf8("PP-OCR に必要な ONNX Runtime がインストールされていません")},
            {"ko", QString::fromUtf8("PP-OCR에 필요한 ONNX Runtime이 설치되어 있지 않습니다")}};
        for (const auto &[language, expected] : languages) {
            QTranslator translator;
            QVERIFY(translator.load(QString(":/translations/app_%1.qm").arg(language)));
            QCoreApplication::installTranslator(&translator);
            const auto uninstall = qScopeGuard([&translator] { QCoreApplication::removeTranslator(&translator); });
            auto *window = show_text_recognition(blank_image());
            QTRY_VERIFY2_WITH_TIMEOUT(window->status_label->text().contains(expected), qPrintable(language), 30000);
            QVERIFY(!window->copy_button->isEnabled());
            window->close();
            shutdown_recognition(10000);
        }
    }

    void closingWhileRecognizingDoesNotBlockOrReport() {
        if (!is_ocr_available()) QSKIP("no OCR engine on this machine");
        QApplication::clipboard()->setText("before");
        config_->set_ocr_copy_on_close(true);
        auto *window = show_text_recognition(text_image("Closing early"));
        window->close();
        shutdown_recognition(30000);
        QApplication::processEvents();
        // Nothing was recognized into the window, so nothing was copied on close.
        QCOMPARE(QApplication::clipboard()->text(), QString("before"));
    }

    void copyOnCloseAndPinStateFollowTheSettings() {
        if (!is_ocr_available()) QSKIP("no OCR engine on this machine");
        auto *window = show_text_recognition(text_image("Pinned text"));
        QTRY_VERIFY_WITH_TIMEOUT(window->copy_button->isEnabled(), 30000);
        // On top by default; the choice is remembered.
        QVERIFY(window->pin_button->isChecked());
        window->pin_button->click();
        QVERIFY(!config_->get_ocr_result_on_top());
        // The layout menu's switch is a setting.
        choose_from_next_menu("Copy text when closing");
        window->layout_button->click();
        QVERIFY(config_->get_ocr_copy_on_close());
        window->text_edit->setPlainText("copied on close");
        QApplication::clipboard()->setText("before");
        window->close();
        QCOMPARE(QApplication::clipboard()->text(), QString("copied on close"));
        // The next window opens with the remembered state.
        auto *next = show_text_recognition(text_image("Again"));
        QApplication::processEvents();
        QVERIFY(!next->pin_button->isChecked());
    }

    void aRelayoutCanBeUndoneBackToTheEdit() {
        if (!is_ocr_available()) QSKIP("no OCR engine on this machine");
        auto *window = show_text_recognition(text_image("Hello World"));
        QTRY_VERIFY_WITH_TIMEOUT(window->copy_button->isEnabled(), 30000);
        const auto recognized = window->text_edit->toPlainText();
        QVERIFY(recognized.contains(' '));
        window->text_edit->selectAll();
        QTest::keyClicks(window->text_edit, "my edit");
        QCOMPARE(window->text_edit->toPlainText(), QString("my edit"));
        choose_from_next_menu("Remove spaces");
        window->layout_button->click();
        QVERIFY(config_->get_ocr_remove_spaces());
        // Spaces are only removed between CJK characters; the layout re-renders from the recognized lines.
        QCOMPARE(window->text_edit->toPlainText(), recognized);
        window->text_edit->undo();
        QCOMPARE(window->text_edit->toPlainText(), QString("my edit"));
        // The first result itself cannot be undone away.
        window->text_edit->undo();
        window->text_edit->undo();
        QVERIFY(!window->text_edit->toPlainText().isEmpty());
    }

    void theCopyDirectlySwitchWritesTheSetting() {
        auto *window = new TextRecognitionWindow(blank_image());
        window->setAttribute(Qt::WA_DeleteOnClose);
        window->show();
        QApplication::processEvents();
        QTest::mouseClick(window->copy_directly_switch, Qt::LeftButton, Qt::NoModifier, QPoint(8, window->copy_directly_switch->height() / 2));
        QVERIFY(config_->get_ocr_copy_directly_enabled());
        QTest::keyClick(window->copy_directly_switch, Qt::Key_Space);
        QVERIFY(!config_->get_ocr_copy_directly_enabled());
        window->copy_directly_switch->setEnabled(false);
        QTest::mouseClick(window->copy_directly_switch, Qt::LeftButton);
        QVERIFY(!config_->get_ocr_copy_directly_enabled());
    }

    void theToastCopiesWithoutAWindow() {
        if (!is_ocr_available()) QSKIP("no OCR engine on this machine");
        QApplication::clipboard()->setText("before");
        QVERIFY(copy_text_recognition(text_image("Toast 42")));
        QTRY_VERIFY_WITH_TIMEOUT(QApplication::clipboard()->text().contains("Toast"), 30000);
        // A failure leaves the clipboard alone.
        QApplication::clipboard()->setText("kept");
        copy_text_recognition(blank_image());
        shutdown_recognition(30000);
        QApplication::processEvents();
        QCOMPARE(QApplication::clipboard()->text(), QString("kept"));
    }

    void recognizeAsyncReportsLinesInReadingOrder() {
        if (!is_ocr_available()) QSKIP("no OCR engine on this machine");
        QVector<OcrTextLine> lines;
        QString reason = "pending";
        QObject context;
        QImage image(420, 160, QImage::Format_RGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        painter.setPen(Qt::black);
        painter.setFont(QFont(font_family, 26));
        painter.drawText(QRect(0, 10, 420, 60), Qt::AlignCenter, "First line");
        painter.drawText(QRect(0, 90, 420, 60), Qt::AlignCenter, "Second line");
        painter.end();
        recognize_async(image, &context, [&](const QVector<OcrTextLine> &result, const QString &why, const text_table::Table &, const OcrResult &) {
            lines = result;
            reason = why;
        });
        QTRY_VERIFY_WITH_TIMEOUT(reason != "pending", 30000);
        QCOMPARE(reason, QString());
        QCOMPARE(lines.size(), 2);
        QVERIFY(lines[0].text.contains("First"));
        QVERIFY(lines[1].text.contains("Second"));
        QVERIFY(lines[0].top < lines[1].top);
        // A thread whose context is gone never calls back.
        bool called = false;
        {
            QObject gone;
            recognize_async(image, &gone, [&](const QVector<OcrTextLine> &, const QString &, const text_table::Table &, const OcrResult &) { called = true; });
        }
        shutdown_recognition(30000);
        QApplication::processEvents();
        QVERIFY(!called);
        // Shutting down with nothing running returns at once.
        shutdown_recognition(10);
    }

    // ---- Table view ----
    void theTableViewShowsMergedCellsAndUndoesEdits() {
        RecognizedTableView view;
        const text_table::Table table{3, 3, {{0, 0, 2, 1, "A"}, {0, 1, 1, 2, "B"}, {1, 1, 1, 1, "c"}, {1, 2, 1, 1, "d"}, {2, 0, 1, 1, "e"}, {2, 1, 1, 1, "f"}, {2, 2, 1, 1, "g"}}};
        view.set_table(table, false);
        QCOMPARE(view.rowCount(), 3);
        QCOMPARE(view.columnCount(), 3);
        QCOMPARE(view.rowSpan(0, 0), 2);
        QCOMPARE(view.columnSpan(0, 1), 2);
        QCOMPARE(view.item(1, 2)->text(), QString("d"));
        QVERIFY(!view.undo_stack()->canUndo());
        QVERIFY(view.content_width() >= 3 * 48);
        // An edit reaches the table once the editor is done, and undoes.
        view.item(1, 2)->setText("D");
        QTRY_COMPARE(text_table::cell_at(view.table(), 1, 2)->text, QString("D"));
        view.undo_stack()->undo();
        QCOMPARE(view.item(1, 2)->text(), QString("d"));
        view.undo_stack()->redo();
        QCOMPARE(view.item(1, 2)->text(), QString("D"));
        // Deleting a row and a column, then undoing both.
        view.setCurrentCell(2, 2);
        view.delete_selected_rows();
        QCOMPARE(view.rowCount(), 2);
        view.setCurrentCell(1, 2);
        view.delete_selected_columns();
        QCOMPARE(view.columnCount(), 2);
        QCOMPARE(view.columnSpan(0, 1), 1);
        view.undo_stack()->undo();
        view.undo_stack()->undo();
        QCOMPARE(view.rowCount(), 3);
        QCOMPARE(view.columnSpan(0, 1), 2);
        // A fresh table cannot be undone away; the last row and column stay.
        view.set_table({1, 1, {{0, 0, 1, 1, "x"}}}, false);
        QVERIFY(!view.undo_stack()->canUndo());
        view.setCurrentCell(0, 0);
        view.delete_selected_rows();
        view.delete_selected_columns();
        QCOMPARE(view.rowCount(), 1);
        QCOMPARE(view.columnCount(), 1);
    }

    void theTableViewCopiesTheSelectionAndClearsWithDelete() {
        RecognizedTableView view;
        view.set_table({2, 3, {{0, 0, 1, 1, "a"}, {0, 1, 1, 1, "b"}, {0, 2, 1, 1, "c"}, {1, 0, 1, 1, "d"}, {1, 1, 1, 1, "e"}, {1, 2, 1, 1, "f"}}}, false);
        view.resize(400, 200);
        view.show();
        view.setRangeSelected(QTableWidgetSelectionRange(0, 1, 1, 2), true);
        QTest::keyClick(&view, Qt::Key_C, Qt::ControlModifier);
        const auto *mime = QApplication::clipboard()->mimeData();
        QCOMPARE(mime->text(), QString("b\tc\ne\tf"));
        QVERIFY(mime->html().contains("<td>e</td>"));
        // Delete clears the selected cells; Ctrl+Z brings them back, Ctrl+Y clears again.
        QTest::keyClick(&view, Qt::Key_Delete);
        QCOMPARE(view.item(1, 2)->text(), QString());
        QCOMPARE(view.item(0, 0)->text(), QString("a"));
        QTest::keyClick(&view, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(view.item(1, 2)->text(), QString("f"));
        QTest::keyClick(&view, Qt::Key_Y, Qt::ControlModifier);
        QCOMPARE(view.item(1, 2)->text(), QString());
        // Without a selection the current cell is copied.
        view.clearSelection();
        view.setCurrentCell(0, 0, QItemSelectionModel::NoUpdate);
        view.copy_selection();
        QCOMPARE(QApplication::clipboard()->text(), QString("a"));
        // The context menu acts on the clicked cell's row.
        const QPoint at = view.visualItemRect(view.item(1, 0)).center();
        choose_from_next_menu("Delete row");
        QContextMenuEvent event(QContextMenuEvent::Mouse, at, view.viewport()->mapToGlobal(at));
        QApplication::sendEvent(view.viewport(), &event);
        QCOMPARE(view.rowCount(), 1);
        QCOMPARE(view.item(0, 0)->text(), QString("a"));
        // Only one row left: deleting rows is off; undo is offered.
        inspect_next_menu([](QMenu *menu) {
            for (auto *action : menu->actions()) {
                if (action->text().startsWith("Delete row")) QVERIFY(!action->isEnabled());
                if (action->text().startsWith("Undo")) QVERIFY(action->isEnabled());
            }
        });
        QContextMenuEvent again(QContextMenuEvent::Mouse, view.visualItemRect(view.item(0, 0)).center(), view.viewport()->mapToGlobal(at));
        QApplication::sendEvent(view.viewport(), &again);
    }

    void theTableMenuInsertsMergesSplitsAndPastes() {
        RecognizedTableView view;
        // A | B B
        // A | c d
        view.set_table({2, 3, {{0, 0, 2, 1, "A"}, {0, 1, 1, 2, "B"}, {1, 1, 1, 1, "c"}, {1, 2, 1, 1, "d"}}}, false);
        view.resize(400, 200);
        view.show();
        const auto menu_at = [&](int row, int column, const QString &label) {
            const QPoint at = view.visualRect(view.model()->index(row, column)).center();
            choose_from_next_menu(label);
            QContextMenuEvent event(QContextMenuEvent::Mouse, at, view.viewport()->mapToGlobal(at));
            QApplication::sendEvent(view.viewport(), &event);
        };
        // Inserting below c's row adds a row and moves to it; inside A's rows A grows.
        menu_at(1, 1, "Insert row below");
        QCOMPARE(view.rowCount(), 3);
        QCOMPARE(view.currentRow(), 2);
        menu_at(1, 1, "Insert row above");
        QCOMPARE(view.rowSpan(0, 0), 3);
        QCOMPARE(view.item(1, 1)->text(), QString());
        view.undo_stack()->undo();
        view.undo_stack()->undo();
        QCOMPARE(view.rowCount(), 2);
        // Right of B's columns, a column after both of them.
        menu_at(0, 1, "Insert column right");
        QCOMPARE(view.columnCount(), 4);
        QCOMPARE(view.columnSpan(0, 1), 2);
        QCOMPARE(view.currentColumn(), 3);
        menu_at(1, 1, "Insert column left");
        QCOMPARE(view.columnSpan(0, 2), 2);
        QCOMPARE(view.item(1, 2)->text(), QString("c"));
        view.undo_stack()->undo();
        view.undo_stack()->undo();

        // c and d merge into one cell with both texts; split is offered only on merged cells.
        inspect_next_menu([](QMenu *menu) {
            for (auto *action : menu->actions()) {
                if (action->text().startsWith("Merge cells") || action->text().startsWith("Split cells")) QVERIFY(!action->isEnabled());
                if (action->text().startsWith("Redo")) QVERIFY(action->isEnabled());
            }
        });
        const QPoint c_at = view.visualItemRect(view.item(1, 1)).center();
        QContextMenuEvent probe(QContextMenuEvent::Mouse, c_at, view.viewport()->mapToGlobal(c_at));
        QApplication::sendEvent(view.viewport(), &probe);
        view.setRangeSelected(QTableWidgetSelectionRange(1, 1, 1, 2), true);
        menu_at(1, 1, "Merge cells");
        QCOMPARE(view.columnSpan(1, 1), 2);
        QCOMPARE(view.item(1, 1)->text(), QString("c d"));
        // Split acts on the merged cell the current cell is in.
        view.clearSelection();
        view.setCurrentCell(1, 1);
        menu_at(1, 1, "Split cells");
        QCOMPARE(view.columnSpan(1, 1), 1);
        QCOMPARE(view.item(1, 1)->text(), QString("c d"));
        QCOMPARE(view.item(1, 2)->text(), QString());

        // Rows across A delete both of A's rows; the last row stays, so that is off.
        view.setCurrentCell(0, 0);
        inspect_next_menu([](QMenu *menu) {
            for (auto *action : menu->actions())
                if (action->text().startsWith("Delete row")) QVERIFY(!action->isEnabled());
        });
        const QPoint a_at = view.visualItemRect(view.item(0, 0)).center();
        QContextMenuEvent rows(QContextMenuEvent::Mouse, a_at, view.viewport()->mapToGlobal(a_at));
        QApplication::sendEvent(view.viewport(), &rows);

        // Pasting a spreadsheet's copy from c grows the table; Ctrl+X empties what it copied.
        QApplication::clipboard()->setText("1\t2\t3\n4\t5\t6\n");
        view.setCurrentCell(1, 1);
        QTest::keyClick(&view, Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(view.rowCount(), 3);
        QCOMPARE(view.columnCount(), 4);
        QCOMPARE(view.item(1, 3)->text(), QString("3"));
        QCOMPARE(view.item(2, 1)->text(), QString("4"));
        QCOMPARE(view.rowSpan(0, 0), 2);
        QTest::keyClick(&view, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(view.rowCount(), 2);
        // One value fills every selected cell.
        QApplication::clipboard()->setText("x");
        view.setRangeSelected(QTableWidgetSelectionRange(1, 1, 1, 2), true);
        QTest::keyClick(&view, Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(view.item(1, 1)->text(), QString("x"));
        QCOMPARE(view.item(1, 2)->text(), QString("x"));
        QTest::keyClick(&view, Qt::Key_X, Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(), QString("x\tx"));
        QCOMPARE(view.item(1, 2)->text(), QString());
    }

    void theTableHasSpreadsheetHeaders() {
        RecognizedTableView view;
        view.set_table({3, 3, {{0, 0, 1, 1, "Item"}, {0, 1, 1, 1, "Qty"}, {0, 2, 1, 1, "Note"}, {1, 0, 1, 1, "a"}, {1, 1, 1, 1, "1,000"}, {1, 2, 1, 1, "x"},
                               {2, 0, 1, 1, "b"}, {2, 1, 1, 1, "2.5"}, {2, 2, 1, 1, "y"}}},
                       false);
        view.resize(500, 300);
        view.show();
        QApplication::processEvents();
        auto *columns = view.horizontalHeader();
        auto *rows = view.verticalHeader();
        QVERIFY(view.headers_visible());
        QCOMPARE(view.model()->headerData(2, Qt::Horizontal).toString(), QString("C"));
        QCOMPARE(view.model()->headerData(0, Qt::Vertical).toString(), QString("1"));
        // A column's header selects the column; its numbers are summed in the status.
        QTest::mouseClick(columns->viewport(), Qt::LeftButton, {}, QPoint(columns->sectionViewportPosition(1) + 5, columns->height() / 2));
        QVERIFY(view.selectionModel()->isColumnSelected(1, QModelIndex()));
        QCOMPARE(view.selection_summary(), QString("Average: 501.25   Count: 3   Sum: 1,002.5"));
        // Text alone is counted; one cell has no summary.
        view.clearSelection();
        view.setRangeSelected(QTableWidgetSelectionRange(1, 2, 2, 2), true);
        QCOMPARE(view.selection_summary(), QString("Count: 2"));
        view.clearSelection();
        view.setCurrentCell(1, 1);
        QCOMPARE(view.selection_summary(), QString());
        // A row's header selects the row; its menu acts on the row right-clicked.
        QTest::mouseClick(rows->viewport(), Qt::LeftButton, {}, QPoint(rows->width() / 2, rows->sectionViewportPosition(2) + 5));
        QVERIFY(view.selectionModel()->isRowSelected(2, QModelIndex()));
        choose_from_next_menu("Delete row");
        emit rows->customContextMenuRequested(QPoint(rows->width() / 2, rows->sectionViewportPosition(1) + 5));
        QCOMPARE(view.rowCount(), 2);
        QCOMPARE(view.item(1, 0)->text(), QString("b"));
        choose_from_next_menu("Delete column");
        emit columns->customContextMenuRequested(QPoint(columns->sectionViewportPosition(2) + 5, columns->height() / 2));
        QCOMPARE(view.columnCount(), 2);
        // The menu turns the headers off and says so, for the window to remember.
        QSignalSpy chosen(&view, &RecognizedTableView::headers_visibility_chosen);
        choose_from_next_menu("Show row and column headers");
        const QPoint at = view.visualItemRect(view.item(0, 0)).center();
        QContextMenuEvent event(QContextMenuEvent::Mouse, at, view.viewport()->mapToGlobal(at));
        QApplication::sendEvent(view.viewport(), &event);
        QVERIFY(!view.headers_visible());
        QVERIFY(columns->isHidden() && rows->isHidden());
        QCOMPARE(chosen.size(), 1);
        QCOMPARE(chosen.front().front().toBool(), false);
    }

    void ctrlWheelZoomsTheTable() {
        RecognizedTableView view;
        view.set_table(small_table(), false);
        view.resize(400, 200);
        view.show();
        QApplication::processEvents();
        const int font = view.font().pixelSize();
        const int row = view.rowHeight(0);
        QSignalSpy zoomed(&view, &RecognizedTableView::zoom_changed);
        const auto wheel = [&](int delta, Qt::KeyboardModifiers modifiers = Qt::ControlModifier) {
            const QPointF at(50, 20);
            QWheelEvent event(at, view.viewport()->mapToGlobal(at), {}, {0, delta}, Qt::NoButton, modifiers, Qt::NoScrollPhase, false);
            QApplication::sendEvent(view.viewport(), &event);
        };
        wheel(120);
        QCOMPARE(view.zoom_percent(), 110);
        QCOMPARE(zoomed.size(), 1);
        QVERIFY(view.font().pixelSize() > font);
        QVERIFY(view.rowHeight(0) > row);
        // Half notches from a touchpad add up.
        wheel(60);
        QCOMPARE(view.zoom_percent(), 110);
        wheel(60);
        QCOMPARE(view.zoom_percent(), 120);
        // Without Ctrl the wheel scrolls.
        wheel(-120, Qt::NoModifier);
        QCOMPARE(view.zoom_percent(), 120);
        wheel(-120 * 50);
        QCOMPARE(view.zoom_percent(), RecognizedTableView::MIN_ZOOM);
        wheel(120 * 50);
        QCOMPARE(view.zoom_percent(), RecognizedTableView::MAX_ZOOM);
        QTest::keyClick(&view, Qt::Key_Minus, Qt::ControlModifier);
        QCOMPARE(view.zoom_percent(), RecognizedTableView::MAX_ZOOM - RecognizedTableView::ZOOM_STEP);
        QTest::keyClick(&view, Qt::Key_0, Qt::ControlModifier);
        QCOMPARE(view.zoom_percent(), 100);
        QCOMPARE(view.font().pixelSize(), font);
        QCOMPARE(view.rowHeight(0), row);
        QTest::keyClick(&view, Qt::Key_Equal, Qt::ControlModifier);
        QCOMPARE(view.zoom_percent(), 110);
    }

    void ctrlWheelZoomsTheText_data() {
        QTest::addColumn<int>("scale");
        QTest::newRow("normal") << 100;
        QTest::newRow("large-window") << 150;
    }

    void ctrlWheelZoomsTheText() {
        QFETCH(int, scale);
        get_dialog_scale().set_percent(scale);
        const auto theme = get_ui_theme()->mode();
        const auto restore = qScopeGuard([theme] {
            get_dialog_scale().set_percent(100);
            get_ui_theme()->set_mode(ui_theme_mode_value(theme), false);
        });
        config_->set_ocr_show_original(false);
        config_->set_ocr_result_view("text");
        auto *window = show_text_recognition(blank_image());
        QTRY_VERIFY_WITH_TIMEOUT(!window->status_label->text().startsWith("Recognizing"), 30000);
        window->on_recognized(SMALL_LINES, {}, small_table());
        auto *edit = window->text_edit;
        auto *zoom = edit->findChild<ResultZoom *>();
        QVERIFY(zoom);
        const int font = edit->font().pixelSize();
        QCOMPARE(font, dialog_scaled(15));
        const int cursor_height = edit->cursorRect().height();
        edit->moveCursor(QTextCursor::End);
        edit->insertPlainText("!");
        const QString text = edit->toPlainText();
        const int undo_steps = edit->document()->availableUndoSteps();
        auto cursor = edit->textCursor();
        cursor.setPosition(0);
        cursor.setPosition(4, QTextCursor::KeepAnchor);
        edit->setTextCursor(cursor);
        const auto wheel = [&](int delta, Qt::KeyboardModifiers modifiers = Qt::ControlModifier) {
            const QPointF at(50, 20);
            QWheelEvent event(at, edit->viewport()->mapToGlobal(at), {}, {0, delta}, Qt::NoButton, modifiers, Qt::NoScrollPhase, false);
            QApplication::sendEvent(edit->viewport(), &event);
        };
        wheel(120);
        QCOMPARE(zoom->percent(), 110);
        QVERIFY(edit->font().pixelSize() > font);
        QVERIFY(edit->cursorRect().height() > cursor_height);
        QCOMPARE(window->status_label->text(), QString("Selected 4 characters · 110%"));
        wheel(60);
        QCOMPARE(zoom->percent(), 110);
        wheel(60);
        QCOMPARE(zoom->percent(), 120);
        wheel(-120, Qt::NoModifier);
        QCOMPARE(zoom->percent(), 120);
        wheel(-120 * 50);
        QCOMPARE(zoom->percent(), 50);
        wheel(120 * 50);
        QCOMPARE(zoom->percent(), 300);
        QTest::keyClick(edit, Qt::Key_Minus, Qt::ControlModifier);
        QCOMPARE(zoom->percent(), 290);
        QTest::keyClick(edit, Qt::Key_0, Qt::ControlModifier);
        QCOMPARE(zoom->percent(), 100);
        QCOMPARE(edit->font().pixelSize(), font);
        QCOMPARE(window->status_label->text(), QString("Selected 4 characters"));
        QTest::keyClick(edit, Qt::Key_Equal, Qt::ControlModifier);
        QCOMPARE(zoom->percent(), 110);
        QTest::keyClick(edit, Qt::Key_Plus, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(zoom->percent(), 120);
        QCOMPARE(edit->toPlainText(), text);
        QCOMPARE(edit->textCursor().selectedText(), QString("Name"));
        QCOMPARE(edit->document()->availableUndoSteps(), undo_steps);
        window->copy_button->click();
        QCOMPARE(QApplication::clipboard()->text(), QString("Name"));
        get_ui_theme()->set_mode("dark", false);
        get_ui_theme()->set_mode("light", false);
        QCOMPARE(zoom->percent(), 120);
        QVERIFY(edit->font().pixelSize() > font);
        window->view_switch->setCurrentItem("table");
        QVERIFY(window->showing_table());
        window->table_view->set_zoom_percent(160);
        window->view_switch->setCurrentItem("text");
        QVERIFY(!window->showing_table());
        QCOMPARE(zoom->percent(), 120);
        QCOMPARE(window->table_view->zoom_percent(), 160);
        edit->undo();
        QVERIFY(!edit->toPlainText().endsWith('!'));
        edit->redo();
        QCOMPARE(edit->toPlainText(), text);
        choose_from_next_menu("Remove line breaks");
        window->layout_button->click();
        QCOMPARE(zoom->percent(), 120);
        QVERIFY(window->status_label->text().endsWith(" · 120%"));
        QVERIFY(edit->font().pixelSize() > font);
        window->close();
    }

    void columnEdgesDragAndFitTheWidth() {
        RecognizedTableView view;
        const QString long_text = QString("A cell text long enough to wrap in a column of the usual fitted width. ").repeated(3);
        view.set_table({2, 3, {{0, 0, 1, 1, long_text}, {0, 1, 1, 1, "b"}, {0, 2, 1, 1, "c"}, {1, 0, 1, 1, "d"}, {1, 1, 1, 1, "e"}, {1, 2, 1, 1, "f"}}}, false);
        view.resize(600, 300);
        view.show();
        QApplication::processEvents();
        auto *columns = view.horizontalHeader();
        const auto total = [&] {
            int sum = 0;
            for (int c = 0; c < view.columnCount(); ++c) sum += view.columnWidth(c);
            return sum;
        };
        // Fitted, the long text wraps.
        QVERIFY(view.columnWidth(0) < view.sizeHintForColumn(0));
        const int wrapped_row = view.rowHeight(0);
        // A double-click on the edge fits the column to its text on one line.
        emit columns->sectionHandleDoubleClicked(0);
        QCOMPARE(view.columnWidth(0), view.sizeHintForColumn(0));
        QVERIFY(view.columnWidth(0) > view.fontMetrics().horizontalAdvance(long_text));
        QVERIFY(view.rowHeight(0) <= wrapped_row);
        // A dragged width stays while the view resizes; the other columns fill the rest.
        columns->resizeSection(0, 100);
        columns->resizeSection(1, 150);
        QCOMPARE(view.columnWidth(1), 150);
        view.resize(1400, 300);
        QApplication::processEvents();
        QCOMPARE(view.columnWidth(1), 150);
        QCOMPARE(total(), view.viewport()->width());
        // Zooming keeps its share.
        view.set_zoom_percent(200);
        QCOMPARE(view.columnWidth(1), 300);
        // A fresh table starts over.
        view.set_zoom_percent(100);
        view.set_table(small_table(), false);
        QApplication::processEvents();
        QCOMPARE(total(), view.viewport()->width());
        QVERIFY(view.columnWidth(1) != 150);
    }

    void theWindowRemembersTheHeadersAndShowsZoomAndSums() {
        config_->set_ocr_result_view("table");
        config_->set_ocr_show_original(false);
        auto *window = new TextRecognitionWindow(blank_image());
        window->setAttribute(Qt::WA_DeleteOnClose);
        QTRY_VERIFY_WITH_TIMEOUT(!window->status_label->text().startsWith("Recognizing"), 30000);
        window->on_recognized(SMALL_LINES, {}, {2, 2, {{0, 0, 1, 1, "7"}, {0, 1, 1, 1, "8"}, {1, 0, 1, 1, "9"}, {1, 1, 1, 1, "x"}}});
        auto *view = window->table_view;
        QVERIFY(view->headers_visible());
        QCOMPARE(window->status_label->text(), QString("2 rows × 2 columns"));
        view->setRangeSelected(QTableWidgetSelectionRange(0, 0, 1, 1), true);
        QCOMPARE(window->status_label->text(), QString("Average: 8   Count: 4   Sum: 24"));
        view->set_zoom_percent(130);
        QCOMPARE(window->status_label->text(), QString("Average: 8   Count: 4   Sum: 24 · 130%"));
        view->clearSelection();
        QCOMPARE(window->status_label->text(), QString("2 rows × 2 columns · 130%"));
        emit view->headers_visibility_chosen(false);
        QVERIFY(!config_->get_ocr_table_headers());
        window->close();
        TextRecognitionWindow next(blank_image());
        QVERIFY(!next.table_view->headers_visible());
    }

    // ---- Original image ----
    void theOriginalImageFitsZoomsAndCopies() {
        OriginalImageView pane(plain_image(800, 400));
        pane.resize(400, 300);
        pane.show();
        QApplication::processEvents();
        QCOMPARE(pane.size_label->text(), QString("800 × 400 px"));
        // Fitted, and a large image is shrunk.
        QCOMPARE(pane.fit(), OriginalImageView::Fit::Window);
        QVERIFY(pane.scale() < 0.5);
        QVERIFY(pane.scale_label->text().endsWith('%'));
        pane.fit_button->click();
        QCOMPARE(pane.fit(), OriginalImageView::Fit::Actual);
        QCOMPARE(pane.scale(), 1.0);
        QCOMPARE(pane.scale_label->text(), QString("100%"));
        // The wheel zooms by steps; zooming stops at the ends of the range.
        const QPointF middle = pane.view()->viewport()->rect().center();
        QWheelEvent wheel(middle, pane.view()->viewport()->mapToGlobal(middle), {}, QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(pane.view()->viewport(), &wheel);
        QVERIFY(std::abs(pane.scale() - 1.15) < 1e-9);
        QCOMPARE(pane.fit(), OriginalImageView::Fit::Custom);
        pane.zoom_by(1000, middle);
        QCOMPARE(pane.scale(), OriginalImageView::MAX_SCALE);
        QVERIFY(!pane.zoom_in_button->isEnabled());
        pane.zoom_out_button->click();
        QVERIFY(pane.zoom_in_button->isEnabled());
        pane.zoom_by(1e-6, middle);
        QCOMPARE(pane.scale(), OriginalImageView::MIN_SCALE);
        QVERIFY(!pane.zoom_out_button->isEnabled());
        // A double click off the text switches between fitting and 1:1.
        QTest::mouseDClick(pane.view()->viewport(), Qt::LeftButton, Qt::NoModifier, middle.toPoint());
        QCOMPARE(pane.fit(), OriginalImageView::Fit::Window);
        QTest::mouseDClick(pane.view()->viewport(), Qt::LeftButton, Qt::NoModifier, middle.toPoint());
        QCOMPARE(pane.fit(), OriginalImageView::Fit::Actual);
        // Dragging off the text moves the image.
        const int before = pane.view()->horizontalScrollBar()->value();
        QTest::mousePress(pane.view()->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(200, 100));
        QMouseEvent drag(QEvent::MouseMove, QPointF(120, 100), pane.view()->viewport()->mapToGlobal(QPointF(120, 100)), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(pane.view()->viewport(), &drag);
        QTest::mouseRelease(pane.view()->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(120, 100));
        QCOMPARE(pane.view()->horizontalScrollBar()->value(), before + 80);
        pane.copy_button->click();
        QCOMPARE(QApplication::clipboard()->image().size(), QSize(800, 400));
    }

    void theOriginalImageSelectsItsTextAndShowsTheResultSelection() {
        OriginalImageView pane(plain_image());
        pane.resize(500, 300);
        pane.show();
        pane.set_text(OcrTextLayout(two_line_result()));
        pane.view()->setFocus();
        QTest::keyClick(pane.view()->viewport(), Qt::Key_A, Qt::ControlModifier);
        QVERIFY(pane.text_selection()->has_selection());
        QTest::keyClick(pane.view(), Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(), QString::fromUtf8("Hello world\n第二行"));
        // A press on the text starts a selection rather than a drag.
        const QPoint on_text = pane.view()->mapFromScene(QPointF(40, 25));
        const int scrolled = pane.view()->horizontalScrollBar()->value();
        QTest::mousePress(pane.view()->viewport(), Qt::LeftButton, Qt::NoModifier, on_text);
        QTest::mouseRelease(pane.view()->viewport(), Qt::LeftButton, Qt::NoModifier, on_text);
        QCOMPARE(pane.view()->horizontalScrollBar()->value(), scrolled);
        QVERIFY(!pane.text_selection()->has_selection());
        const QPolygonF area(QRectF(10, 60, 30, 30));
        pane.set_highlight({area});
        QCOMPARE(pane.highlight(), QVector<QPolygonF>({area}));
        // The pointer turns to an I-beam over text with no button held, as on a pin.
        auto *viewport = pane.view()->viewport();
        QVERIFY(viewport->hasMouseTracking());
        const auto hover = [&](const QPoint &at) {
            QMouseEvent move(QEvent::MouseMove, at, viewport->mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(viewport, &move);
            return viewport->cursor().shape();
        };
        QCOMPARE(hover(on_text), Qt::IBeamCursor);
        QCOMPARE(hover(pane.view()->mapFromScene(QPointF(300, 150))), Qt::ArrowCursor);
    }

    void theWindowHasOneSelectionAtATime() {
        config_->set_ocr_layout_mode("multiline");
        config_->set_ocr_result_view("text");
        auto *window = new TextRecognitionWindow(plain_image());
        window->setAttribute(Qt::WA_DeleteOnClose);
        window->show();
        QTRY_VERIFY_WITH_TIMEOUT(!window->status_label->text().startsWith("Recognizing"), 30000);
        const auto result = two_line_result();
        window->on_recognized(group_ocr_lines(result), {}, small_table(), result);
        QVERIFY(!window->showing_table());
        auto *image = window->original_view;
        const auto select_text = [&](int from, int to) {
            auto cursor = window->text_edit->textCursor();
            cursor.setPosition(from);
            cursor.setPosition(to, QTextCursor::KeepAnchor);
            window->text_edit->setTextCursor(cursor);
        };
        const auto select_image = [&] {
            image->view()->setFocus();
            QTest::keyClick(image->view()->viewport(), Qt::Key_A, Qt::ControlModifier);
        };
        select_text(0, 5);
        QVERIFY(!image->highlight().isEmpty());
        QCOMPARE(window->status_label->text(), QString("Selected 5 characters"));
        window->copy_button->click();
        QCOMPARE(QApplication::clipboard()->text(), QString("Hello"));
        // Selecting on the image clears the text's selection and its shading; Copy takes the image's.
        select_image();
        QVERIFY(image->text_selection()->has_selection());
        QVERIFY(!window->text_edit->textCursor().hasSelection());
        QVERIFY(image->highlight().isEmpty());
        QCOMPARE(window->status_label->text(), QString("Selected 15 characters"));
        window->copy_button->click();
        QCOMPARE(QApplication::clipboard()->text(), QString::fromUtf8("Hello world\n第二行"));
        // And the other way round.
        select_text(12, 14);
        QVERIFY(!image->text_selection()->has_selection());
        QVERIFY(!image->highlight().isEmpty());
        window->copy_button->click();
        QCOMPARE(QApplication::clipboard()->text(), QString::fromUtf8("第二"));
        // Nothing selected: everything.
        select_text(3, 3);
        QCOMPARE(window->status_label->text(), QString("Recognized 15 characters"));
        window->copy_button->click();
        QCOMPARE(QApplication::clipboard()->text(), QString::fromUtf8("Hello world\n第二行"));

        // The table likewise; one cell alone copies the whole table.
        window->view_switch->setCurrentItem("table");
        QVERIFY(window->showing_table());
        auto *table = window->table_view;
        table->setCurrentCell(1, 0);
        window->copy_button->click();
        QCOMPARE(QApplication::clipboard()->text(), QString("Name\tQty\nPear\t7"));
        table->setRangeSelected(QTableWidgetSelectionRange(1, 0, 1, 1), true);
        window->copy_button->click();
        QCOMPARE(QApplication::clipboard()->text(), QString("Pear\t7"));
        select_image();
        QVERIFY(table->selectedRanges().isEmpty());
        QCOMPARE(window->status_label->text(), QString("Selected 15 characters"));
        window->copy_button->click();
        QCOMPARE(QApplication::clipboard()->text(), QString::fromUtf8("Hello world\n第二行"));
        table->setCurrentCell(0, 0);
        QVERIFY(!image->text_selection()->has_selection());
        QCOMPARE(window->status_label->text(), QString("2 rows × 2 columns"));
        // Closing the image drops its selection.
        select_image();
        window->original_switch->setChecked(false);
        QCOMPARE(window->status_label->text(), QString("2 rows × 2 columns"));
    }

    void selectedTextIsShadedOnTheOriginalImage() {
        config_->set_ocr_layout_mode("multiline");
        auto *window = new TextRecognitionWindow(plain_image());
        window->setAttribute(Qt::WA_DeleteOnClose);
        window->show();
        QTRY_VERIFY_WITH_TIMEOUT(!window->status_label->text().startsWith("Recognizing"), 30000);
        const auto result = two_line_result();
        window->on_recognized(group_ocr_lines(result), {}, {}, result);
        QCOMPARE(window->text_edit->toPlainText(), QString::fromUtf8("Hello world\n第二行"));
        auto select = [&](int from, int to) {
            auto cursor = window->text_edit->textCursor();
            cursor.setPosition(from);
            cursor.setPosition(to, QTextCursor::KeepAnchor);
            window->text_edit->setTextCursor(cursor);
            return window->original_view->highlight();
        };
        // "Hello" lies at the start of the first line, "第二" at the start of the second.
        const auto hello = select(0, 5);
        QCOMPARE(hello.size(), 1);
        QVERIFY(QRectF(10, 10, 220, 30).contains(area_bounds(hello)));
        QVERIFY(area_bounds(hello).width() < 120);
        const auto second = select(12, 14);
        QCOMPARE(second.size(), 1);
        QVERIFY(QRectF(10, 60, 90, 30).contains(area_bounds(second)));
        QCOMPARE(select(0, 14).size(), 2);
        QVERIFY(select(3, 3).isEmpty());
        // Typed text comes from nowhere; the recognized text after it still points home.
        auto cursor = window->text_edit->textCursor();
        cursor.setPosition(0);
        cursor.insertText("XY");
        QCOMPARE(select(0, 2).size(), 0);
        QCOMPARE(area_bounds(select(2, 7)), area_bounds(hello));
        cursor.setPosition(8);
        cursor.setPosition(13, QTextCursor::KeepAnchor);
        cursor.insertText("there");
        QVERIFY(select(8, 13).isEmpty());
        // A new layout lays the text out afresh and points it all home again.
        config_->set_ocr_punctuation("halfwidth");
        window->on_recognized(group_ocr_lines(result), {}, {}, result);
        QCOMPARE(area_bounds(select(0, 5)), area_bounds(hello));
        // Turned off, the image keeps nothing selected.
        window->original_switch->setChecked(false);
        QVERIFY(window->original_view->isHidden());
        QVERIFY(!window->original_view->text_selection()->has_selection());
    }

    void undoAndRedoKeepTheShadingOnTheOriginalImage() {
        config_->set_ocr_layout_mode("multiline");
        auto *window = new TextRecognitionWindow(plain_image());
        window->setAttribute(Qt::WA_DeleteOnClose);
        window->show();
        QTRY_VERIFY_WITH_TIMEOUT(!window->status_label->text().startsWith("Recognizing"), 30000);
        const auto result = two_line_result();
        window->on_recognized(group_ocr_lines(result), {}, {}, result);
        auto select = [&](int from, int to) {
            auto cursor = window->text_edit->textCursor();
            cursor.setPosition(from);
            cursor.setPosition(to, QTextCursor::KeepAnchor);
            window->text_edit->setTextCursor(cursor);
            return area_bounds(window->original_view->highlight());
        };
        const QRectF hello = select(0, 5), second = select(12, 14);
        QVERIFY(!hello.isNull() && !second.isNull());
        // A new layout, then undoing and redoing it.
        choose_from_next_menu("Remove line breaks");
        window->layout_button->click();
        QCOMPARE(window->text_edit->toPlainText(), QString::fromUtf8("Hello world第二行"));
        QCOMPARE(select(11, 13), second);
        window->text_edit->undo();
        QCOMPARE(window->text_edit->toPlainText(), QString::fromUtf8("Hello world\n第二行"));
        QCOMPARE(select(0, 5), hello);
        QCOMPARE(select(12, 14), second);
        window->text_edit->redo();
        QCOMPARE(select(11, 13), second);
        // Deleted recognized text points home again when the deletion is undone.
        window->text_edit->undo();
        auto cursor = window->text_edit->textCursor();
        cursor.setPosition(0);
        cursor.setPosition(5, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
        QVERIFY(select(0, 3).width() < hello.width());
        window->text_edit->undo();
        QCOMPARE(select(0, 5), hello);
        // After an edit on a new branch, nothing is taken from the old one.
        cursor.setPosition(0);
        cursor.insertText("Z");
        QCOMPARE(select(1, 6), hello);
        QVERIFY(select(0, 1).isNull());
    }

    void selectedCellsAreShadedOnTheOriginalImage() {
        config_->set_ocr_result_view("table");
        auto *window = new TextRecognitionWindow(plain_image());
        window->setAttribute(Qt::WA_DeleteOnClose);
        window->show();
        QTRY_VERIFY_WITH_TIMEOUT(!window->status_label->text().startsWith("Recognizing"), 30000);
        const QRectF a(10, 10, 40, 20), b(100, 10, 40, 20);
        const text_table::Table table{2, 2, {{0, 0, 1, 1, "a", {a}}, {0, 1, 1, 1, "b", {b}}, {1, 0, 1, 1, "c", {}}, {1, 1, 1, 1, "d", {}}}};
        window->on_recognized(SMALL_LINES, {}, table);
        QVERIFY(window->showing_table());
        window->table_view->setCurrentCell(0, 1);
        QCOMPARE(window->original_view->highlight(), QVector<QPolygonF>({QPolygonF(b)}));
        window->table_view->setRangeSelected(QTableWidgetSelectionRange(0, 0, 0, 1), true);
        QCOMPARE(window->original_view->highlight().size(), 2);
        // An edited cell no longer points into the image.
        window->table_view->item(0, 1)->setText("B");
        QTRY_COMPARE(text_table::cell_at(window->table_view->table(), 0, 1)->text, QString("B"));
        window->table_view->setCurrentCell(0, 1);
        QVERIFY(window->original_view->highlight().isEmpty());
        // Back in the text view the table's shading goes.
        window->view_switch->setCurrentItem("text");
        QVERIFY(window->original_view->highlight().isEmpty());
    }

    void theOriginalImageSplitsTheWindowWithoutResizingIt() {
        for (const int percent : {100, 150}) {
            get_dialog_scale().set_percent(percent);
            const auto restore = qScopeGuard([] { get_dialog_scale().set_percent(100); });
            config_->set_ocr_show_original(true);
            auto *window = new TextRecognitionWindow(plain_image());
            const int available = window->screen()->availableGeometry().width();
            // Opens wide enough for both sides.
            const int wide = dialog_scaled(OriginalImageView::WIDTH) + window->splitter->handleWidth() + dialog_scaled(TextRecognitionWindow::RESULT_WIDTH_BESIDE_IMAGE);
            QCOMPARE(window->width(), std::min(wide, available));
            window->resize(std::min(available, dialog_scaled(1100)), window->height());
            window->show();
            QApplication::processEvents();
            const int width = window->width();
            window->original_switch->setChecked(false);
            QVERIFY(!config_->get_ocr_show_original());
            QVERIFY(window->original_view->isHidden());
            QCOMPARE(window->width(), width);
            window->original_switch->setChecked(true);
            QVERIFY(window->original_view->isVisible());
            QCOMPARE(window->width(), width);
            // As wide as it opens, or what the result side's minimum leaves on a narrow screen.
            const int room = window->splitter->width() - window->splitter->handleWidth() - window->splitter->widget(1)->minimumSizeHint().width();
            QCOMPARE(window->splitter->sizes().value(0), std::min(dialog_scaled(OriginalImageView::WIDTH), room));
            delete window;
            // Off, the next window opens at the usual width.
            config_->set_ocr_show_original(false);
            TextRecognitionWindow narrow(plain_image());
            QCOMPARE(narrow.width(), std::min(dialog_scaled(TextRecognitionWindow::WIDTH), available));
            QVERIFY(narrow.original_view->isHidden());
        }
        // No image, no switch.
        TextRecognitionWindow bare{QImage()};
        QVERIFY(!bare.original_switch);
        QVERIFY(!bare.original_view);
    }

    void textOnlyResultsShowNoTableSwitch() {
        auto *window = new TextRecognitionWindow(blank_image());
        window->setAttribute(Qt::WA_DeleteOnClose);
        // The blank image's own result arrives first.
        QTRY_VERIFY_WITH_TIMEOUT(!window->status_label->text().startsWith("Recognizing"), 30000);
        window->on_recognized(SMALL_LINES, {}, {});
        QVERIFY(window->view_switch->isHidden());
        QVERIFY(!window->showing_table());
        QCOMPARE(window->text_edit->toPlainText(), QString("Name Qty\nPear 7"));
    }

    void aTableAddsTheSwitchAndTheChosenViewOpensNextTime() {
        auto *window = new TextRecognitionWindow(blank_image());
        window->setAttribute(Qt::WA_DeleteOnClose);
        QTRY_VERIFY_WITH_TIMEOUT(!window->status_label->text().startsWith("Recognizing"), 30000);
        window->on_recognized(SMALL_LINES, {}, small_table());
        QVERIFY(!window->view_switch->isHidden());
        // Text first until the table view is chosen.
        QVERIFY(!window->showing_table());
        QVERIFY(window->status_label->text().startsWith("Recognized"));
        window->view_switch->setCurrentItem("table");
        QVERIFY(window->showing_table());
        QCOMPARE(config_->get_ocr_result_view(), QString("table"));
        QCOMPARE(window->status_label->text(), QString("2 rows × 2 columns"));
        window->copy_button->click();
        const auto *mime = QApplication::clipboard()->mimeData();
        QCOMPARE(mime->text(), QString("Name\tQty\nPear\t7"));
        QVERIFY(mime->html().contains("<table"));
        // Deleting a row updates the count.
        window->table_view->setCurrentCell(1, 0);
        window->table_view->delete_selected_rows();
        QCOMPARE(window->status_label->text(), QString("1 rows × 2 columns"));
        window->view_switch->setCurrentItem("text");
        QCOMPARE(config_->get_ocr_result_view(), QString("text"));
        window->copy_button->click();
        QCOMPARE(QApplication::clipboard()->text(), QString("Name Qty\nPear 7"));
        window->view_switch->setCurrentItem("table");
        window->close();

        auto *next = new TextRecognitionWindow(blank_image());
        next->setAttribute(Qt::WA_DeleteOnClose);
        QTRY_VERIFY_WITH_TIMEOUT(!next->status_label->text().startsWith("Recognizing"), 30000);
        next->on_recognized(SMALL_LINES, {}, small_table());
        QVERIFY(next->showing_table());
        // Choosing a view only by opening does not change the setting.
        QCOMPARE(config_->get_ocr_result_view(), QString("table"));
    }

    void copyOnCloseTakesTheTableShowing() {
        config_->set_ocr_copy_on_close(true);
        config_->set_ocr_result_view("table");
        auto *window = new TextRecognitionWindow(blank_image());
        QTRY_VERIFY_WITH_TIMEOUT(!window->status_label->text().startsWith("Recognizing"), 30000);
        window->on_recognized(SMALL_LINES, {}, small_table());
        QApplication::clipboard()->setText("before");
        window->close();
        QCOMPARE(QApplication::clipboard()->text(), QString("Name\tQty\nPear\t7"));
        delete window;
    }

    void layoutOptionsReachTheCellsAndUndo() {
        config_->set_ocr_result_view("table");
        auto *window = new TextRecognitionWindow(blank_image());
        window->setAttribute(Qt::WA_DeleteOnClose);
        QTRY_VERIFY_WITH_TIMEOUT(!window->status_label->text().startsWith("Recognizing"), 30000);
        const auto cjk = QString::fromUtf8("汉 字");
        window->on_recognized({{cjk + " 7", 0, 100, 0, 20}, {"a b", 0, 100, 30, 50}}, {}, {2, 2, {{0, 0, 1, 1, cjk}, {0, 1, 1, 1, "7"}, {1, 0, 1, 1, "a"}, {1, 1, 1, 1, "b"}}});
        QVERIFY(window->showing_table());
        // Line breaks do not apply to cells.
        inspect_next_menu([](QMenu *menu) {
            for (auto *action : menu->actions())
                if (action->text() == "Auto" || action->text() == "Keep every line") QVERIFY(!action->isEnabled());
        });
        window->layout_button->click();
        choose_from_next_menu("Remove spaces");
        window->layout_button->click();
        QCOMPARE(text_table::cell_at(window->table_view->table(), 0, 0)->text, QString::fromUtf8("汉字"));
        window->table_view->undo_stack()->undo();
        QCOMPARE(text_table::cell_at(window->table_view->table(), 0, 0)->text, cjk);
    }

    void aWideTableWidensTheWindowOnce() {
        config_->set_ocr_result_view("table");
        config_->set_ocr_show_original(false);
        auto *window = new TextRecognitionWindow(blank_image());
        window->setAttribute(Qt::WA_DeleteOnClose);
        QTRY_VERIFY_WITH_TIMEOUT(!window->status_label->text().startsWith("Recognizing"), 30000);
        // Narrower than the default, which already takes most of the offscreen screen.
        window->resize(dialog_scaled(500), window->height());
        const int before = window->width();
        text_table::Table wide{1, 12, {}};
        for (int c = 0; c < 12; ++c) wide.cells.append({0, c, 1, 1, "A rather long cell text"});
        wide.rows = 2;
        for (int c = 0; c < 12; ++c) wide.cells.append({1, c, 1, 1, "x"});
        window->on_recognized(SMALL_LINES, {}, wide);
        QVERIFY(window->showing_table());
        QVERIFY(window->width() > before);
        QVERIFY(window->width() <= window->screen()->availableGeometry().width() * 0.8 + 1);
        QVERIFY(window->screen()->availableGeometry().contains(window->geometry().topLeft()));
    }

    void theGridLeavesTheOuterEdgeToTheCard() {
        config_->set_ocr_result_view("table");
        auto *window = new TextRecognitionWindow(blank_image());
        window->setAttribute(Qt::WA_DeleteOnClose);
        window->show();
        QTRY_VERIFY_WITH_TIMEOUT(!window->status_label->text().startsWith("Recognizing"), 30000);
        window->on_recognized(SMALL_LINES, {}, small_table());
        QApplication::processEvents();
        auto *view = window->table_view;
        // Narrower than the card, the columns widen to fill it.
        int columns = 0;
        for (int c = 0; c < view->columnCount(); ++c) columns += view->columnWidth(c);
        QCOMPARE(columns, view->viewport()->width());
        QImage shot = view->viewport()->grab().toImage();
        int beside = view->columnViewportPosition(1) - 3;
        const int middle = view->rowHeight(0) / 2;
        QVERIFY(shot.pixelColor(beside + 2, middle).alpha() > 0);
        // The rows end above the card's bottom, so the last one closes the table.
        QVERIFY(shot.pixelColor(beside, view->rowViewportPosition(1) + view->rowHeight(1) - 1).alpha() > 0);
        // The card's border is the edge after the last column.
        QCOMPARE(shot.pixelColor(shot.width() - 1, middle).alpha(), 0);
        // Scrolled to the end, the last row sits on the border and draws no line of its own.
        text_table::Table tall{30, 2, {}};
        for (int r = 0; r < 30; ++r) tall.cells << text_table::Cell{r, 0, 1, 1, "a"} << text_table::Cell{r, 1, 1, 1, "b"};
        view->set_table(tall, false);
        QApplication::processEvents();
        view->scrollToBottom();
        QApplication::processEvents();
        shot = view->viewport()->grab().toImage();
        beside = view->columnViewportPosition(1) - 3;
        QCOMPARE(shot.pixelColor(beside, shot.height() - 1).alpha(), 0);
    }

    void closeAfterCopyClosesTheWindowOnCopy() {
        auto *window = new TextRecognitionWindow(blank_image());
        QPointer<TextRecognitionWindow> alive = window;
        window->show();
        QTRY_VERIFY_WITH_TIMEOUT(!window->status_label->text().startsWith("Recognizing"), 30000);
        window->on_recognized(SMALL_LINES, {}, {});
        QVERIFY(!window->close_after_copy_switch->isChecked());
        window->close_after_copy_switch->setChecked(true);
        QVERIFY(config_->get_ocr_close_after_copy());
        // With copy-on-close as well, the clipboard is still written once.
        config_->set_ocr_copy_on_close(true);
        QApplication::clipboard()->setText("before");
        QSignalSpy changes(QApplication::clipboard(), &QClipboard::dataChanged);
        window->copy_button->click();
        QCOMPARE(QApplication::clipboard()->text(), QString("Name Qty\nPear 7"));
        QCOMPARE(changes.count(), 1);
        QVERIFY(!window->isVisible());
        QTRY_VERIFY(alive.isNull());
        // The next window keeps the enabled preference.
        TextRecognitionWindow next(blank_image());
        QVERIFY(next.close_after_copy_switch->isChecked());
    }

    void copyingKeepsTheIconVisibleAndRestartsTheFeedback() {
        const auto theme = get_ui_theme()->mode();
        const auto restore = qScopeGuard([theme] { get_ui_theme()->set_mode(ui_theme_mode_value(theme), false); });
        auto *window = new TextRecognitionWindow(blank_image());
        window->show();
        QTRY_VERIFY_WITH_TIMEOUT(!window->status_label->text().startsWith("Recognizing"), 30000);
        window->on_recognized(SMALL_LINES, {}, {});
        auto *button = window->copy_button;
        const auto button_size = button->size();
        const auto has_ink = [](const QImage &image, const QColor &ink) {
            int count = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x) {
                    const auto pixel = image.pixelColor(x, y);
                    if (pixel.alpha() > 180 && std::abs(pixel.red() - ink.red()) < 48 && std::abs(pixel.green() - ink.green()) < 48 &&
                        std::abs(pixel.blue() - ink.blue()) < 48)
                        ++count;
                }
            return count >= 4;
        };
        const auto verify_icon = [&] {
            const int side = widget_scaled(button, 14);
            const auto icon_rect = QRect(widget_scaled(button, 8) + 1, (button->height() - side) / 2, side, side);
            return has_ink(button->grab().toImage().copy(icon_rect), css_color(ui_tokens(button).text));
        };
        for (const char *mode : {"light", "dark"}) {
            get_ui_theme()->set_mode(mode, false);
            QVERIFY(verify_icon());
            QTest::mousePress(button, Qt::LeftButton);
            QVERIFY(verify_icon());
            QTest::mouseRelease(button, Qt::LeftButton);
            QCOMPARE(button->text(), QString("Copied"));
            QCOMPARE(button->size(), button_size);
            QVERIFY(verify_icon());
            const auto shots = qEnvironmentVariable("JIETUBA_OCR_BUTTON_SHOTS");
            if (!shots.isEmpty()) {
                QVERIFY(QDir().mkpath(shots));
                QVERIFY(button->grab().save(QDir(shots).filePath(QString("copy-%1.png").arg(mode))));
            }
        }
        QTest::qWait(800);
        button->click();
        QTest::qWait(900);
        QCOMPARE(button->text(), QString("Copied"));
        QTRY_COMPARE_WITH_TIMEOUT(button->text(), QString("Copy"), 2000);
        QVERIFY(verify_icon());
    }

    void aPinnedWindowKeepsAClearlyVisibleFill() {
        const auto theme = get_ui_theme()->mode();
        const auto restore = qScopeGuard([theme] { get_ui_theme()->set_mode(ui_theme_mode_value(theme), false); });
        TextRecognitionWindow window(blank_image());
        auto *button = window.pin_button;
        for (const char *mode : {"light", "dark"}) {
            get_ui_theme()->set_mode(mode, false);
            button->setChecked(false);
            const QPoint sample(widget_scaled(button, 10), button->height() / 2);
            const auto off = button->grab().toImage().pixelColor(sample);
            button->setChecked(true);
            QEvent leave(QEvent::Leave);
            QApplication::sendEvent(button, &leave);
            const auto on = button->grab().toImage().pixelColor(sample);
            QCOMPARE(on, css_color(ui_tokens(button).accent_strong));
            QVERIFY(on != off);
            const auto shots = qEnvironmentVariable("JIETUBA_OCR_BUTTON_SHOTS");
            if (!shots.isEmpty()) {
                QVERIFY(QDir().mkpath(shots));
                QVERIFY(button->grab().save(QDir(shots).filePath(QString("pin-on-%1.png").arg(mode))));
                button->setChecked(false);
                QVERIFY(button->grab().save(QDir(shots).filePath(QString("pin-off-%1.png").arg(mode))));
            }
        }
    }

    void theViewSwitchSitsInsideTheTitleBar() {
        TextRecognitionWindow window(blank_image());
        QVERIFY(window.view_switch->sizeHint().height() <= window.titleBar()->height() - 2 * dialog_scaled(3));
    }

    void aDrawnTableIsFoundAndCopiedDirectly() {
        if (!is_ocr_available()) QSKIP("no OCR engine on this machine");
        auto *window = show_text_recognition(table_image());
        QTRY_VERIFY_WITH_TIMEOUT(window->copy_button->isEnabled(), 30000);
        QVERIFY(!window->view_switch->isHidden());
        QCOMPARE(window->table_view->table().rows, 3);
        QCOMPARE(window->table_view->table().columns, 3);
        window->close();
        // Copying directly takes the table when the table view was chosen last.
        config_->set_ocr_result_view("table");
        QApplication::clipboard()->setText("before");
        QVERIFY(copy_text_recognition(table_image()));
        QTRY_VERIFY_WITH_TIMEOUT(QApplication::clipboard()->mimeData()->html().contains("<table"), 30000);
        QVERIFY(QApplication::clipboard()->text().contains('\t'));
    }

    void theResultWindowFitsEveryLanguage() {
        // Text widths need real fonts, which the offscreen platform does not have.
        if (QGuiApplication::platformName() != "windows") QSKIP("needs -platform windows");
        const QString language = I18nManager::get_current_language();
        const auto restore = qScopeGuard([language] {
            get_dialog_scale().set_percent(100);
            I18nManager::load_language(language);
        });
        config_->set_ocr_result_view("table");
        text_table::Table table{12, 2, {}};
        // Long numbers, for the longest selection summary.
        for (int r = 0; r < 12; ++r) table.cells << text_table::Cell{r, 0, 1, 1, "a"} << text_table::Cell{r, 1, 1, 1, "1,234,567.89"};
        // A four-digit count, as a page of text gives.
        const QVector<OcrTextLine> long_text = {{QString(1234, 'a'), 0, 100, 0, 20}};
        for (const int percent : {100, 150, 200}) {
            get_dialog_scale().set_percent(percent);
            for (const char *code : {"zh", "zh_TW", "en", "ja", "ko"}) {
                QVERIFY(I18nManager::load_language(code));
                auto *window = new TextRecognitionWindow(blank_image());
                QTRY_VERIFY_WITH_TIMEOUT(window->status_label->text() != i18n_tr("Recognizing...", "TextRecognitionWindow"), 30000);
                window->on_recognized(long_text, {}, table);
                window->resize(dialog_scaled(TextRecognitionWindow::WIDTH), dialog_scaled(TextRecognitionWindow::HEIGHT));
                // The title bar follows the window in its resize event, which a hidden window only gets when shown.
                QResizeEvent resized(window->size(), QSize());
                QApplication::sendEvent(window, &resized);
                window->layout()->activate();
                auto *bar = qobject_cast<FluentTitleBar *>(window->titleBar());
                QCOMPARE(bar->width(), window->width());
                bar->hBoxLayout->activate();
                const QRect switch_rect(window->view_switch->mapTo(window, QPoint()), window->view_switch->size());
                QVERIFY2(std::abs(switch_rect.center().x() - window->rect().center().x()) <= 1,
                         qPrintable(QString("%1 at %2%: switch %3, window %4, title %5/%6, pin %7")
                                        .arg(code).arg(percent).arg(switch_rect.center().x()).arg(window->rect().center().x())
                                        .arg(bar->titleLabel->width()).arg(bar->titleLabel->sizeHint().width()).arg(window->pin_button->width())));
                QVERIFY(switch_rect.left() > bar->titleLabel->geometry().right());
                QVERIFY(switch_rect.right() < window->pin_button->geometry().left());
                // A wrapping label's size hint allows for wrapping; its text must still fit on one line.
                const auto needs = [](QWidget *part) {
                    const auto *label = qobject_cast<QLabel *>(part);
                    return label && label->wordWrap() ? label->fontMetrics().horizontalAdvance(label->text()) : part->sizeHint().width();
                };
                const auto context = [&](const QString &what, QWidget *part) {
                    return qPrintable(QString("%1 at %2%: %3 is %4 wide, needs %5").arg(code).arg(percent).arg(what).arg(part->width()).arg(needs(part)));
                };
                for (const char *view : {"table", "text"}) {
                    // The text view's status is the character count.
                    window->view_switch->setCurrentItem(view);
                    if (window->showing_table()) {
                        window->table_view->selectAll();
                        window->table_view->set_zoom_percent(RecognizedTableView::MAX_ZOOM);
                        QVERIFY(window->status_label->text().contains("14,814,814.68") && window->status_label->text().endsWith("300%"));
                    }
                    window->layout()->activate();
                    window->findChild<QFrame *>("recognitionToolbar")->layout()->activate();
                    window->findChild<QFrame *>("recognitionFooter")->layout()->activate();
                    // The footer does not hold the window wider than its default.
                    QVERIFY2(window->layout()->minimumSize().width() <= dialog_scaled(TextRecognitionWindow::WIDTH),
                             qPrintable(QString("%1 at %2% in the %3 view: the footer needs %4")
                                            .arg(code).arg(percent).arg(view).arg(window->layout()->minimumSize().width())));
                    for (const auto &[what, part] : std::initializer_list<std::pair<QString, QWidget *>>{
                             {"the view switch", window->view_switch}, {"the title", bar->titleLabel}, {"the status", window->status_label},
                             {"the copy-directly switch", window->copy_directly_switch}, {"the close-after-copy switch", window->close_after_copy_switch},
                             {"the copy button", window->copy_button}, {"the original image switch", window->original_switch}})
                        QVERIFY2(part->width() >= needs(part), context(what, part));
                }
                delete window;
            }
        }
    }

    // ---- Barcode results ----

    void everyCodeGetsACardAndOnlyWebLinksOpen() {
        auto *window = show_barcode_result(codes_image_);
        QVERIFY(window);
        QCOMPARE(window->cards.size(), size_t(4));
        QVERIFY(window->summary_label->text().contains("4"));
        for (auto *card : window->cards) QVERIFY(!card->open_button);
        window->cards[1]->copy_button->click();
        QCOMPARE(QApplication::clipboard()->text(), QString("CODE128-0042"));
        QCOMPARE(window->cards[1]->copy_button->text(), QString("Copied"));
        window->copy_single_check->setChecked(true);
        QVERIFY(config_->get_barcode_copy_single_enabled());
    }

    void pointingAtACodeHighlightsBothSides() {
        auto *window = show_barcode_result(codes_image_);
        window->resize(1000, 700);
        QApplication::processEvents();
        const auto codes = read_codes(codes_image_);
        auto *view = window->image_view;
        // Image pixels to the view: fitted, centred, never enlarged.
        const double scale = std::min({double(view->width()) / codes_image_.width(), double(view->height()) / codes_image_.height(), 1.0});
        const QPointF offset((view->width() - codes_image_.width() * scale) / 2, (view->height() - codes_image_.height() * scale) / 2);
        const auto centre = codes[2].outline.boundingRect().center() * scale + offset;
        QMouseEvent move(QEvent::MouseMove, centre, QPointF(view->mapToGlobal(centre.toPoint())), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(view, &move);
        for (int i = 0; i < 4; ++i) QCOMPARE(window->cards[i]->property("active").toBool(), i == 2);
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(view, &leave);
        for (auto *card : window->cards) QVERIFY(!card->property("active").toBool());
        // Entering a card highlights it too.
        QEnterEvent enter(QPointF(5, 5), QPointF(5, 5), QPointF(5, 5));
        QApplication::sendEvent(window->cards[0], &enter);
        QVERIFY(window->cards[0]->property("active").toBool());
        // Painting with a highlighted code works at any size.
        QVERIFY(!view->grab().isNull());
    }

    void oneCodeIsCopiedWithoutAWindowWhenAsked() {
        const auto codes = read_codes(codes_image_);
        const auto single = codes_image_.copy(codes[1].outline.boundingRect().toAlignedRect().adjusted(-20, -20, 20, 20));
        QCOMPARE(read_codes(single).size(), size_t(1));
        QApplication::clipboard()->setText("before");
        QCOMPARE(show_barcode_result(single, true), nullptr);
        QCOMPARE(QApplication::clipboard()->text(), QString("CODE128-0042"));
        // Several codes, or none, still open the window.
        QVERIFY(show_barcode_result(codes_image_, true));
        auto *empty = show_barcode_result(blank_image(), true);
        QVERIFY(empty);
        QVERIFY(empty->cards.empty());
    }

    // 预解码入口只消费已有结果，空图上的合成结果也不应被重新识别覆盖。
    void predecodedCodesShareTheWindowAndCopyPolicy() {
        const std::vector<DecodedCode> codes{{QString::fromUtf8("中文扫码结果"), "QR Code", {}}};
        QApplication::clipboard()->setText("before");
        auto *window = show_barcode_result(blank_image(), codes, false);
        QVERIFY(window);
        QCOMPARE(window->cards.size(), size_t(1));
        QCOMPARE(QApplication::clipboard()->text(), QString("before"));
        QCOMPARE(window->cards[0]->findChild<CodeText *>()->toPlainText(), codes[0].text);
        QCOMPARE(show_barcode_result(blank_image(), codes, true), nullptr);
        QCOMPARE(QApplication::clipboard()->text(), codes[0].text);
        auto *multiple = show_barcode_result(blank_image(), std::vector<DecodedCode>{codes[0], codes[0]}, true);
        QVERIFY(multiple);
        QCOMPARE(multiple->cards.size(), size_t(2));
        auto *empty = show_barcode_result(blank_image(), std::vector<DecodedCode>{}, true);
        QVERIFY(empty);
        QVERIFY(empty->cards.empty());
        QCOMPARE(QApplication::clipboard()->text(), codes[0].text);
    }

    void webLinksGetAnOpenButton() {
        // Only http(s) links with a host can be opened; the card decides from the text.
        DecodedCode link;
        link.text = "https://example.com/path";
        link.format_name = "QR Code";
        link.outline = QPolygonF(QRectF(10, 10, 50, 50));
        DecodedCode mail = link;
        mail.text = "mailto:someone@example.com";
        DecodedCode bare = link;
        bare.text = "example.com";
        BarcodeResultWindow window(codes_image_, {link, mail, bare});
        QVERIFY(window.cards[0]->open_button);
        QVERIFY(!window.cards[1]->open_button);
        QVERIFY(!window.cards[2]->open_button);
    }

    void theResultWindowsShareTheStandardCaption() {
        use_test_app_icon();
        DecodedCode code;
        code.text = "example.com";
        code.outline = QPolygonF(QRectF(10, 10, 50, 50));
        BarcodeResultWindow barcode(codes_image_, {code});
        VERIFY_STANDARD_CAPTION(barcode);
        TextRecognitionWindow text(blank_image());
        VERIFY_STANDARD_CAPTION(text);
    }
};

QTEST_MAIN(ResultWindowTests)
#include "test_result_windows.moc"
