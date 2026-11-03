#include "barcode/reader.h"
#include "barcode/result_window.h"
#include "core/i18n.h"
#include "core/ui_scale.h"
#include "ocr/ocr_manager.h"
#include "ocr/ppocr_models.h"
#include "settings/tool_settings.h"
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
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTranslator>
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
        recognize_async(image, &context, [&](const QVector<OcrTextLine> &result, const QString &why, const text_table::Table &) {
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
            recognize_async(image, &gone, [&](const QVector<OcrTextLine> &, const QString &, const text_table::Table &) { called = true; });
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
        auto *window = new TextRecognitionWindow(blank_image());
        window->setAttribute(Qt::WA_DeleteOnClose);
        QTRY_VERIFY_WITH_TIMEOUT(!window->status_label->text().startsWith("Recognizing"), 30000);
        const int before = window->width();
        text_table::Table wide{1, 8, {}};
        for (int c = 0; c < 8; ++c) wide.cells.append({0, c, 1, 1, "A rather long cell text"});
        wide.rows = 2;
        for (int c = 0; c < 8; ++c) wide.cells.append({1, c, 1, 1, "x"});
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
        for (int r = 0; r < 12; ++r) table.cells << text_table::Cell{r, 0, 1, 1, "a"} << text_table::Cell{r, 1, 1, 1, "b"};
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
                             {"the copy button", window->copy_button}})
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
