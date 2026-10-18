#include "canvas/items/drawing_items.h"
#include "canvas/items/stroke_item.h"
#include "canvas/items/arrow_item.h"
#include "canvas/items/watermark_item.h"
#include "canvas/scene.h"
#include "canvas/selection_model.h"
#include "canvas/smart_edit_controller.h"
#include "canvas/view.h"
#include "core/clipboard_utils.h"
#include "core/last_capture_region.h"
#include "pin/pin_canvas.h"
#include "pin/pin_window.h"
#include "pin/pin_manager.h"
#include "settings/tool_settings.h"
#include "tools/controller.h"
#include "ui/mask_overlay.h"
#include "ui/magnifier.h"
#include "core/shortcut_manager.h"
#include "ui/screenshot_window.h"
#include "ui/selection_info/panel.h"
#include "ui/selection_info/border_shadow.h"
#include "ui/selection_info/rounded_corners.h"
#include "tools/action.h"
#include "ui/color_picker_button.h"
#include "ui/toolbar.h"
#include "core/i18n.h"
#include "ui/arrow_settings_panel.h"
#include "ui/mosaic_settings_panel.h"
#include "ui/number_settings_panel.h"
#include "ui/paint_settings_panel.h"
#include "ui/shape_settings_panel.h"
#include "ui/text_settings_panel.h"
#include "ui/toolbar_layout_dialog.h"
#include "barcode/result_window.h"
#include "text_recognition/recognizer.h"
#include "text_recognition/result_window.h"
#include "translation/translation_manager.h"
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QContextMenuEvent>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QImageReader>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace jietuba;

namespace {
constexpr int W = 400;
constexpr int H = 300;

QImage capture_image() {
    QImage image(W, H, QImage::Format_ARGB32);
    image.fill(Qt::blue);
    return image;
}

void send_mouse(ScreenshotWindow *window, QEvent::Type type, Qt::MouseButton button, const QPoint &pos, Qt::MouseButtons buttons,
                Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    // An action may have ended the session on the press.
    if (!window->view) return;
    auto *viewport = window->view->viewport();
    QMouseEvent event(type, QPointF(pos), QPointF(viewport->mapToGlobal(pos)), button, buttons, modifiers);
    QApplication::sendEvent(viewport, &event);
}

void drag(ScreenshotWindow *window, const QPoint &start, const QPoint &end) {
    send_mouse(window, QEvent::MouseButtonPress, Qt::LeftButton, start, Qt::LeftButton);
    send_mouse(window, QEvent::MouseMove, Qt::NoButton, (start + end) / 2, Qt::LeftButton);
    send_mouse(window, QEvent::MouseMove, Qt::NoButton, end, Qt::LeftButton);
    send_mouse(window, QEvent::MouseButtonRelease, Qt::LeftButton, end, Qt::NoButton);
    QApplication::processEvents();
}

// The bitmap size on the system clipboard, read back through Win32: the offscreen
// platform's QClipboard is a separate in-process one.
QSize clipboard_bitmap_size() {
    QSize size;
    for (int attempt = 0; attempt < 20 && size.isEmpty(); ++attempt) {
        if (OpenClipboard(nullptr)) {
            if (HANDLE handle = GetClipboardData(CF_DIBV5)) {
                if (const auto *header = static_cast<const BITMAPV5HEADER *>(GlobalLock(handle))) {
                    size = QSize(header->bV5Width, std::abs(header->bV5Height));
                    GlobalUnlock(handle);
                }
            }
            CloseClipboard();
        }
        if (size.isEmpty()) QTest::qWait(25);
    }
    return size;
}

void clear_clipboard() {
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        CloseClipboard();
    }
}

void click(ScreenshotWindow *window, Qt::MouseButton button, const QPoint &pos = QPoint(30, 30)) {
    send_mouse(window, QEvent::MouseButtonPress, button, pos, button);
    send_mouse(window, QEvent::MouseButtonRelease, button, pos, Qt::NoButton);
}

// Answers the next modal file dialog: an empty path cancels it, otherwise the name is typed in.
// An unanswered dialog is cancelled after ten seconds, so a test never waits for a person.
// opened_in receives the folder the dialog started in.
void answer_file_dialog(const QString &path, const QString &filter = {}, QString *opened_in = nullptr) {
    auto *timer = new QTimer;
    auto started = std::make_shared<QElapsedTimer>();
    started->start();
    QObject::connect(timer, &QTimer::timeout, [timer, path, filter, started, opened_in] {
        auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
        auto *name = dialog ? dialog->findChild<QLineEdit *>("fileNameEdit") : nullptr;
        const bool give_up = started->elapsed() > 10000;
        if ((!name || !dialog->isVisible()) && !give_up) return;
        timer->stop();
        timer->deleteLater();
        if (!dialog) return;
        if (opened_in) *opened_in = dialog->directory().absolutePath();
        if (path.isEmpty() || give_up) {
            dialog->reject();
            return;
        }
        if (!filter.isEmpty()) dialog->selectNameFilter(filter);
        dialog->setDirectory(QFileInfo(path).absolutePath());
        name->setText(QFileInfo(path).fileName());
        static_cast<QDialog *>(dialog)->accept();
    });
    timer->start(50);
}

template <typename T>
T *visible_window() {
    for (auto *widget : QApplication::topLevelWidgets())
        if (auto *typed = qobject_cast<T *>(widget); typed && typed->isVisible()) return typed;
    return nullptr;
}
}

class ScreenshotWindowTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir dir_;
    QSettings *settings_ = nullptr;
    ToolSettingsManager *config_ = nullptr;
    QPointer<ScreenshotWindow> window_;

    // A new window per capture, as the application opens them; the ended one is deleted first.
    ScreenshotWindow *open(const QRectF &rect = QRectF(0, 0, W, H)) {
        if (window_) window_->cleanup_and_close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        window_ = new ScreenshotWindow(config_, capture_image(), rect);
        QApplication::processEvents();
        return window_;
    }

    // A confirmed 200x150 selection at (50, 40).
    ScreenshotWindow *open_with_selection() {
        auto *window = open();
        drag(window, QPoint(50, 40), QPoint(250, 190));
        return window;
    }

    QStringList saved_files() const { return QDir(dir_.filePath("captures")).entryList(QDir::Files); }

private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        settings_ = new QSettings(dir_.filePath("settings.ini"), QSettings::IniFormat, this);
        config_ = get_tool_settings_manager(settings_);
    }

    void init() {
        settings_->clear();
        config_->set_screenshot_save_path(dir_.filePath("captures"));
        config_->set_screenshot_save_enabled(false);
        config_->set_app_setting("ocr_enabled", false);
        config_->set_smart_selection(false);
        config_->set_smart_selection_mode("off");
        reset_last_region_for_test();
        QDir(dir_.filePath("captures")).removeRecursively();
    }

    void cleanup() {
        if (window_ && window_->session_active()) window_->cleanup_and_close();
        PinManager::instance()->close_all();
        QApplication::processEvents();
    }

    void cleanupTestCase() {
        if (window_) window_->cleanup_and_close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!window_);
    }

    // ---- Session ----

    void aSessionStartsWithoutToolbarAndShowsItOnceSelected() {
        auto *window = open();
        QVERIFY(window->session_active());
        QVERIFY(window->isVisible());
        QVERIFY(window->view && window->scene);
        QCOMPARE(window->geometry(), QRect(0, 0, W, H));
        QVERIFY(!window->toolbar->isVisible());
        QVERIFY(!window->scene->selection_model->is_confirmed());
        // Topmost pins sink below the capture while it is open.
        QVERIFY(PinManager::instance()->topmost_suppressed());

        drag(window, QPoint(50, 40), QPoint(250, 190));
        QVERIFY(window->scene->selection_model->is_confirmed());
        QCOMPARE(window->scene->selection_model->rect(), QRectF(50, 40, 200, 150));
        QVERIFY(window->toolbar->isVisible());
        QVERIFY(window->info_panel->isVisible());
        // The toolbar sits below the selection, or above it when there is no room.
        const auto bar = window->toolbar->geometry();
        QVERIFY2(bar.top() >= 190 || bar.bottom() <= 40, qPrintable(QString("%1,%2 %3x%4").arg(bar.x()).arg(bar.y()).arg(bar.width()).arg(bar.height())));
    }

    void escEndsTheSessionAndDeletesTheWindow() {
        auto *window = open_with_selection();
        QSignalSpy ended(window, &ScreenshotWindow::session_ended);
        QPointer<ScreenshotWindow> ended_window = window;
        QPointer<CanvasView> old_view = window->view;
        QTest::keyClick(window, Qt::Key_Escape);
        QCOMPARE(ended.count(), 1);
        QVERIFY(!window->session_active());
        QVERIFY(!window->isVisible());
        QVERIFY(!window->scene && !window->view);
        QVERIFY(!PinManager::instance()->topmost_suppressed());
        // A second end before the deletion is harmless.
        window->cleanup_and_close();
        QCOMPARE(ended.count(), 1);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!old_view);
        QVERIFY(!ended_window);

        // The next capture starts fresh on the current desktop.
        auto *again = open(QRectF(-200, 0, W, H));
        QVERIFY(again->session_active());
        QCOMPARE(again->geometry(), QRect(-200, 0, W, H));
        QVERIFY(!again->toolbar->isVisible());
        QVERIFY(!again->scene->selection_model->is_confirmed());
        QVERIFY(!again->toolbar->current_tool);
    }

    void theCancelButtonAndWindowCloseEndTheSession() {
        auto *window = open_with_selection();
        QSignalSpy ended(window, &ScreenshotWindow::session_ended);
        window->toolbar->cancel_btn->click();
        QCOMPARE(ended.count(), 1);
        window = open();
        QSignalSpy closed(window, &ScreenshotWindow::session_ended);
        window->close();
        QCOMPARE(closed.count(), 1);
        QVERIFY(!window->session_active());
    }

    // ---- Mouse actions ----

    void aRightClickClosesByDefault() {
        auto *window = open_with_selection();
        QSignalSpy ended(window, &ScreenshotWindow::session_ended);
        send_mouse(window, QEvent::MouseButtonPress, Qt::RightButton, QPoint(30, 30), Qt::RightButton);
        QCOMPARE(ended.count(), 1);
    }

    void anUnboundRightClickNeverReachesSelectionOrDrawing() {
        config_->set_app_setting("mouse_capture_close", "");
        auto *window = open();
        QSignalSpy ended(window, &ScreenshotWindow::session_ended);
        for (auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease, QEvent::MouseButtonDblClick})
            send_mouse(window, type, Qt::RightButton, QPoint(30, 30), type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::RightButton);
        QVERIFY(!window->view->selection_drag->active);
        QCOMPARE(ended.count(), 0);
        // A right click in the middle of a left drag does not end the drag.
        send_mouse(window, QEvent::MouseButtonPress, Qt::LeftButton, QPoint(30, 30), Qt::LeftButton);
        QVERIFY(window->view->selection_drag->active);
        send_mouse(window, QEvent::MouseButtonPress, Qt::RightButton, QPoint(30, 30), Qt::LeftButton | Qt::RightButton);
        send_mouse(window, QEvent::MouseButtonRelease, Qt::RightButton, QPoint(30, 30), Qt::LeftButton);
        QVERIFY(window->view->selection_drag->active);
        send_mouse(window, QEvent::MouseButtonRelease, Qt::LeftButton, QPoint(30, 30), Qt::NoButton);
        QVERIFY(!window->view->selection_drag->active);
        // The capture view never shows a context menu.
        QContextMenuEvent menu(QContextMenuEvent::Mouse, QPoint(30, 30), QPoint(30, 30));
        menu.ignore();
        QApplication::sendEvent(window->view->viewport(), &menu);
        QVERIFY(menu.isAccepted());
    }

    void aReboundRightClickQuickSavesTheSelection() {
        config_->set_app_setting("mouse_capture_close", "middle");
        config_->set_app_setting("mouse_capture_quick_save", "right");
        auto *window = open();
        QSignalSpy ended(window, &ScreenshotWindow::session_ended);
        // Without a selection the action does nothing.
        click(window, Qt::RightButton);
        QCOMPARE(ended.count(), 0);
        QVERIFY(saved_files().isEmpty());
        drag(window, QPoint(50, 40), QPoint(250, 190));
        click(window, Qt::RightButton, QPoint(100, 100));
        QCOMPARE(ended.count(), 1);
        QCOMPARE(saved_files().size(), 1);
        QCOMPARE(QImage(QDir(dir_.filePath("captures")).filePath(saved_files().first())).size(), QSize(200, 150));
        // The saved region is remembered for the next capture.
        QCOMPARE(get_last_region(), std::optional<QRect>(QRect(50, 40, 200, 150)));
    }

    void doubleClickRunsItsBindingInsideTheSelection() {
        config_->set_app_setting("mouse_capture_copy", "");
        config_->set_app_setting("mouse_capture_quick_save", "doubleleft");
        auto *window = open_with_selection();
        QVERIFY(window->view->confirm_on_double_click);
        QSignalSpy ended(window, &ScreenshotWindow::session_ended);
        // Outside the selection a double click does nothing.
        send_mouse(window, QEvent::MouseButtonPress, Qt::LeftButton, QPoint(10, 10), Qt::LeftButton);
        send_mouse(window, QEvent::MouseButtonRelease, Qt::LeftButton, QPoint(10, 10), Qt::NoButton);
        send_mouse(window, QEvent::MouseButtonDblClick, Qt::LeftButton, QPoint(10, 10), Qt::LeftButton);
        send_mouse(window, QEvent::MouseButtonRelease, Qt::LeftButton, QPoint(10, 10), Qt::NoButton);
        QCOMPARE(ended.count(), 0);
        send_mouse(window, QEvent::MouseButtonPress, Qt::LeftButton, QPoint(120, 100), Qt::LeftButton);
        send_mouse(window, QEvent::MouseButtonRelease, Qt::LeftButton, QPoint(120, 100), Qt::NoButton);
        send_mouse(window, QEvent::MouseButtonDblClick, Qt::LeftButton, QPoint(120, 100), Qt::LeftButton);
        QCOMPARE(ended.count(), 1);
        QCOMPARE(saved_files().size(), 1);
    }

    void withoutADoubleClickBindingTheViewDoesNotWaitForOne() {
        config_->set_app_setting("mouse_capture_copy", "");
        QVERIFY(!open()->view->confirm_on_double_click);
    }

    void theMiddleButtonRunsItsCaptureAction() {
        config_->set_app_setting("mouse_capture_quick_save", "middle");
        auto *window = open_with_selection();
        QSignalSpy ended(window, &ScreenshotWindow::session_ended);
        click(window, Qt::MiddleButton, QPoint(120, 100));
        QCOMPARE(ended.count(), 1);
        QCOMPARE(saved_files().size(), 1);
    }

    void fullscreenCrosshairIsRebuiltPerSession() {
        config_->set_app_setting("capture_fullscreen_crosshair", true);
        auto *window = open();
        auto *viewport = window->view->viewport();
        window->view->setCursor(Qt::CrossCursor);
        const QPoint pos(140, 100);
        QMouseEvent move(QEvent::MouseMove, QPointF(pos), QPointF(viewport->mapToGlobal(pos)), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &move);
        // The native cross is hidden; the mask draws the full-screen one.
        QCOMPARE(viewport->cursor().shape(), Qt::BlankCursor);
        window->cleanup_and_close();
        config_->set_app_setting("capture_fullscreen_crosshair", false);
        window = open();
        window->view->setCursor(Qt::CrossCursor);
        QCOMPARE(window->view->viewport()->cursor().shape(), Qt::CrossCursor);
    }

    // The full-screen crosshair is for selecting; once confirmed, drawing tools keep the system cross cursor.
    void fullscreenCrosshairOnlyWhileSelecting() {
        config_->set_app_setting("capture_fullscreen_crosshair", true);
        auto *window = open();
        auto *viewport = window->view->viewport();
        window->view->setCursor(Qt::CrossCursor);
        const QPoint pos(140, 100);
        const auto move = [viewport, pos] {
            QMouseEvent event(QEvent::MouseMove, QPointF(pos), QPointF(viewport->mapToGlobal(pos)), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(viewport, &event);
        };
        move();
        QCOMPARE(viewport->cursor().shape(), Qt::BlankCursor);
        QRectF rect(20, 10, 100, 80);
        window->scene->selection_model->activate();
        window->scene->selection_model->set_rect(rect);
        window->scene->confirm_selection();
        QCOMPARE(viewport->cursor().shape(), Qt::CrossCursor);
        QVERIFY(!window->mask_overlay->crosshair_position());
        window->view->setCursor(Qt::CrossCursor);
        move();
        QCOMPARE(viewport->cursor().shape(), Qt::CrossCursor);
        QVERIFY(!window->mask_overlay->crosshair_position());
    }

    // ---- Keys and the toolbar ----

    void toolKeysAndButtonsSwitchTools() {
        auto *window = open_with_selection();
        // Tool keys only work once the selection is confirmed.
        QTest::keyClick(window, Qt::Key_P);
        QCOMPARE(window->scene->tool_controller->current_tool_id(), QString("pen"));
        QCOMPARE(window->toolbar->current_tool, std::optional<QString>("pen"));
        QVERIFY(window->toolbar->pen_btn->isChecked());
        window->toolbar->rect_btn->click();
        QCOMPARE(window->scene->tool_controller->current_tool_id(), QString("rect"));
        QVERIFY(!window->toolbar->pen_btn->isChecked());
        // A second click on the same tool leaves it.
        window->toolbar->rect_btn->click();
        QCOMPARE(window->scene->tool_controller->current_tool_id(), QString("cursor"));
    }

    void drawingUndoRedoAndDeleteThroughKeysAndButtons() {
        auto *window = open_with_selection();
        window->toolbar->select_tool("rect");
        drag(window, QPoint(80, 60), QPoint(180, 140));
        const auto rects = [&] {
            int count = 0;
            for (auto *item : window->scene->items())
                if (dynamic_cast<RectItem *>(item)) ++count;
            return count;
        };
        QCOMPARE(rects(), 1);
        QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(rects(), 0);
        QTest::keyClick(window, Qt::Key_Y, Qt::ControlModifier);
        QCOMPARE(rects(), 1);
        window->toolbar->undo_btn->click();
        QCOMPARE(rects(), 0);
        window->toolbar->redo_btn->click();
        QCOMPARE(rects(), 1);
        // Clicking the rect's outline with its tool selects it; Delete removes it.
        click(window, Qt::LeftButton, QPoint(80, 100));
        QVERIFY(window->view->smart_edit_controller->selected_item());
        QTest::keyClick(window, Qt::Key_Delete);
        QCOMPARE(rects(), 0);
        QVERIFY(window->scene->undo_stack->canUndo());
    }

    void toolbarStyleChangesReachTheTools() {
        auto *window = open_with_selection();
        window->toolbar->select_tool("pen");
        auto &ctx = window->scene->tool_controller->ctx;
        emit window->toolbar->color_changed(QColor("#00ff00"));
        QCOMPARE(ctx.color, QColor("#00ff00"));
        emit window->toolbar->stroke_width_changed(9);
        QCOMPARE(ctx.stroke_width, 9.0);
        emit window->toolbar->opacity_changed(128);
        QVERIFY(std::abs(ctx.opacity - 128 / 255.0) < 0.01);
        drag(window, QPoint(80, 60), QPoint(180, 140));
        StrokeItem *stroke = nullptr;
        for (auto *item : window->scene->items())
            if (auto *found = dynamic_cast<StrokeItem *>(item)) stroke = found;
        QVERIFY(stroke);
        QCOMPARE(*stroke->get_stroke_width(), 9.0);
        // A selected item follows the toolbar too.
        window->view->smart_edit_controller->select_item(stroke);
        emit window->toolbar->stroke_width_changed(4);
        QCOMPARE(*stroke->get_stroke_width(), 4.0);
        emit window->toolbar->line_style_changed("dashed");
        QVERIFY(stroke->pen().style() != Qt::SolidLine);
    }

    void theWatermarkButtonAddsOneWatermark() {
        config_->set_screenshot_watermark_text("ACME");
        auto *window = open_with_selection();
        emit window->toolbar->watermark_requested();
        auto *item = WatermarkItem::find(window->scene);
        QVERIFY(item);
        QCOMPARE(item->text(), QString("ACME"));
        QCOMPARE(item->rect(), QRectF(50, 40, 200, 150));
        emit window->toolbar->watermark_requested();
        int count = 0;
        for (auto *i : window->scene->items()) count += dynamic_cast<WatermarkItem *>(i) != nullptr;
        QCOMPARE(count, 1);
        emit window->toolbar->watermark_style_changed("font_size", 40);
        QCOMPARE(item->style().font_size, 40);
        // Ending the capture remembers that it had a watermark.
        config_->set_app_setting("screenshot_watermark_auto", true);
        window->cleanup_and_close();
        QVERIFY(config_->get_screenshot_watermark_last_on());
        // The next capture gets it automatically once a region is selected, outside undo.
        window = open_with_selection();
        QVERIFY(WatermarkItem::find(window->scene));
        QVERIFY(!window->scene->undo_stack->canUndo());
    }

    void restoreLastRegionPresetsTheSelection() {
        set_last_region(QRect(60, 50, 120, 80));
        auto *window = open();
        QTest::keyClick(window, Qt::Key_L);
        QVERIFY(window->scene->selection_model->is_confirmed());
        QCOMPARE(window->scene->selection_model->rect(), QRectF(60, 50, 120, 80));
        QVERIFY(window->toolbar->isVisible());
        // A region off this desktop is not restored.
        window->cleanup_and_close();
        set_last_region(QRect(5000, 50, 120, 80));
        window = open();
        QTest::keyClick(window, Qt::Key_L);
        QVERIFY(!window->scene->selection_model->is_confirmed());
    }

    void draggingTheSelectionHidesTheToolbarUntilRelease() {
        auto *window = open_with_selection();
        QVERIFY(window->toolbar->isVisible());
        send_mouse(window, QEvent::MouseButtonPress, Qt::LeftButton, QPoint(250, 190), Qt::LeftButton);
        send_mouse(window, QEvent::MouseMove, Qt::NoButton, QPoint(300, 230), Qt::LeftButton);
        QVERIFY(!window->toolbar->isVisible());
        send_mouse(window, QEvent::MouseButtonRelease, Qt::LeftButton, QPoint(300, 230), Qt::NoButton);
        QVERIFY(window->toolbar->isVisible());
        QCOMPARE(window->scene->selection_model->rect(), QRectF(50, 40, 250, 190));
    }

    // ---- Copying and pinning, which write the system clipboard ----

    void enterCopiesTheSelectionAndEndsTheSession() {
        auto *window = open_with_selection();
        window->toolbar->select_tool("rect");
        drag(window, QPoint(80, 60), QPoint(180, 140));
        clear_clipboard();
        QSignalSpy ended(window, &ScreenshotWindow::session_ended);
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(ended.count(), 1);
        QVERIFY(wait_for_clipboard_writes(10.0));
        QCOMPARE(clipboard_bitmap_size(), QSize(200, 150));
        // Auto save is off: nothing on disk.
        QVERIFY(saved_files().isEmpty());
    }

    void autoSaveWritesTheCopyToDiskToo() {
        config_->set_screenshot_save_enabled(true);
        auto *window = open_with_selection();
        clear_clipboard();
        window->toolbar->copy_btn->click();
        QVERIFY(!window->session_active());
        QVERIFY(wait_for_clipboard_writes(10.0));
        QCOMPARE(clipboard_bitmap_size(), QSize(200, 150));
        QTRY_COMPARE_WITH_TIMEOUT(saved_files().size(), 1, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(QImage(QDir(dir_.filePath("captures")).filePath(saved_files().first())).size(), QSize(200, 150), 10000);
    }

    void pinningTheSelectionCarriesItsAnnotations() {
        auto *window = open_with_selection();
        window->toolbar->select_tool("rect");
        drag(window, QPoint(80, 60), QPoint(180, 140));
        QSignalSpy ended(window, &ScreenshotWindow::session_ended);
        QSignalSpy created(PinManager::instance(), &PinManager::pin_created);
        clear_clipboard();
        window->toolbar->pin_btn->click();
        QCOMPARE(ended.count(), 1);
        QCOMPARE(created.count(), 1);
        const auto pins = PinManager::instance()->get_all_pins();
        QCOMPARE(pins.size(), 1);
        auto *pin = pins.first();
        QCOMPARE(pin->geometry(), QRect(50, 40, 200, 150));
        // The background is the plain capture; the rectangle comes along as vector data.
        QCOMPARE(pin->orig_size(), QSize(200, 150));
        QCOMPARE(pin->base_pixmap().toImage().pixelColor(30, 20), QColor(Qt::blue));
        int rects = 0;
        for (auto *item : pin->canvas->scene->items()) rects += dynamic_cast<RectItem *>(item) != nullptr;
        QCOMPARE(rects, 1);
        QVERIFY(wait_for_clipboard_writes(10.0));
        QCOMPARE(clipboard_bitmap_size(), QSize(200, 150));
        QCOMPARE(get_last_region(), std::optional<QRect>(QRect(50, 40, 200, 150)));
    }

    void thePinShortcutPinsOnlyAConfirmedSelection() {
        auto *window = open();
        window->pin_from_global_hotkey();
        QCOMPARE(PinManager::instance()->count(), 0);
        QVERIFY(window->session_active());
        drag(window, QPoint(50, 40), QPoint(250, 190));
        window->pin_from_global_hotkey();
        QCOMPARE(PinManager::instance()->count(), 1);
        QVERIFY(!window->session_active());
        QVERIFY(wait_for_clipboard_writes(10.0));
    }

    // ---- The selection info panel ----

    // Rounded corners: the slider popup follows the button, the export loses its corners,
    // and the choice carries into the next capture.
    void roundedCornersShapeTheExportAndAreRemembered() {
        auto *window = open_with_selection();
        auto *panel = window->info_panel;
        auto *popup = window->findChild<RoundedSliderPopup *>();
        QVERIFY(popup);
        QVERIFY(!popup->isVisible());
        panel->btn_rounded->click();
        QVERIFY(popup->isVisible());
        QVERIFY(config_->get_app_setting("screenshot_rounded_enabled").toBool());
        popup->set_value(30);
        QCOMPARE(config_->get_app_setting("screenshot_rounded_radius").toInt(), 30);
        QVERIFY(!window->grab().isNull());
        // Follows the window variable: each capture has its own window.
        const auto exported = [&window] { return window->action_handler->export_service.export_image(window->scene->selection_model->rect()); };
        auto image = exported();
        QCOMPARE(image.size(), QSize(200, 150));
        QCOMPARE(qAlpha(image.pixel(0, 0)), 0);
        QCOMPARE(qAlpha(image.pixel(199, 149)), 0);
        QCOMPARE(qAlpha(image.pixel(100, 75)), 255);
        // Leaving the button hides the popup shortly after; coming back shows it again.
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(panel->btn_rounded, &leave);
        QTRY_VERIFY(!popup->isVisible());
        QEnterEvent enter(QPointF(2, 2), QPointF(2, 2), QPointF(2, 2));
        QApplication::sendEvent(panel->btn_rounded, &enter);
        QVERIFY(popup->isVisible());
        QApplication::sendEvent(popup, &leave);
        QApplication::sendEvent(popup, &enter);
        QTest::qWait(300);
        QVERIFY(popup->isVisible());
        popup->apply_scale();
        QVERIFY(popup->isVisible());
        // Off: square corners again.
        panel->btn_rounded->click();
        QVERIFY(!popup->isVisible());
        QCOMPARE(qAlpha(exported().pixel(0, 0)), 255);
        // On again, then a new capture starts rounded with the same radius.
        panel->btn_rounded->click();
        window->cleanup_and_close();
        window = open_with_selection();
        QVERIFY(window->info_panel->btn_rounded->isChecked());
        QCOMPARE(window->findChild<RoundedSliderPopup *>()->value(), 30);
        QCOMPARE(qAlpha(exported().pixel(0, 0)), 0);
    }

    // Border or shadow around the export, chosen and styled in the popup.
    void borderAndShadowSurroundTheExport() {
        auto *window = open_with_selection();
        auto *panel = window->info_panel;
        auto *popup = window->findChild<BorderShadowPopup *>();
        QVERIFY(popup);
        panel->btn_border->click();
        QVERIFY(popup->isVisible());
        QCOMPARE(popup->mode(), QString("border"));
        popup->set_size_value(10);
        QCOMPARE(config_->get_app_setting("screenshot_border_size").toInt(), 10);
        QVERIFY(!window->grab().isNull());
        // Follows the window variable: each capture has its own window.
        const auto exported = [&window] {
            return window->action_handler->export_service.export_image(window->scene->selection_model->rect()).convertToFormat(QImage::Format_ARGB32);
        };
        auto image = exported();
        QCOMPARE(image.size(), QSize(220, 170));
        QCOMPARE(QColor(image.pixel(3, 85)), QColor("#FF0000"));
        QCOMPARE(QColor(image.pixel(110, 85)), QColor(Qt::blue));
        // A new border colour from the colour dialog opened by a right click on the group.
        auto groups = popup->findChildren<BorderShadowGroup *>();
        QCOMPARE(groups.size(), 2);
        BorderShadowGroup *border_group = nullptr, *shadow_group = nullptr;
        for (auto *group : groups) {
            for (auto *block : group->findChildren<ColorPickerButton *>())
                (block->color() == QColor("#FF0000") ? border_group : shadow_group) = group;
        }
        QVERIFY(border_group && shadow_group);
        QTimer::singleShot(0, qApp, [] {
            if (auto *dialog = qobject_cast<QColorDialog *>(QApplication::activeModalWidget())) {
                dialog->setCurrentColor(QColor("#00AA00"));
                dialog->accept();
            }
        });
        QTest::mousePress(border_group, Qt::RightButton);
        QCOMPARE(config_->get_app_setting("screenshot_border_color").toString().toUpper(), QString("#00AA00"));
        QCOMPARE(QColor(exported().pixel(3, 85)), QColor("#00AA00"));
        // A left click on the shadow group switches to a soft shadow.
        QTest::mousePress(shadow_group, Qt::LeftButton);
        QCOMPARE(popup->mode(), QString("shadow"));
        QCOMPARE(config_->get_app_setting("screenshot_border_mode").toString(), QString("shadow"));
        for (auto *block : shadow_group->findChildren<ColorPickerButton *>()) emit block->color_changed(QColor("#202020"));
        QCOMPARE(config_->get_app_setting("screenshot_shadow_color").toString(), QString("#202020"));
        QVERIFY(!window->grab().isNull());
        image = exported();
        QCOMPARE(image.size(), QSize(220, 170));
        // Fading out away from the image.
        QVERIFY2(qAlpha(image.pixel(0, 85)) < qAlpha(image.pixel(8, 85)), qPrintable(QString("%1 %2").arg(qAlpha(image.pixel(0, 85))).arg(qAlpha(image.pixel(8, 85)))));
        // With rounded corners the effect follows the radius.
        panel->btn_rounded->click();
        QVERIFY(!window->grab().isNull());
        QCOMPARE(exported().size(), QSize(220, 170));
        panel->btn_rounded->click();
        // Hover leaves hide the popup; the preview goes with it.
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(panel->btn_border, &leave);
        QTRY_VERIFY(!popup->isVisible());
        QEnterEvent enter(QPointF(2, 2), QPointF(2, 2), QPointF(2, 2));
        QApplication::sendEvent(panel->btn_border, &enter);
        QVERIFY(popup->isVisible());
        QApplication::sendEvent(popup, &leave);
        QApplication::sendEvent(popup, &enter);
        popup->apply_scale();
        // Without "keep enabled" the next capture starts without the effect.
        window->cleanup_and_close();
        window = open_with_selection();
        QVERIFY(!window->info_panel->btn_border->isChecked());
        QCOMPARE(exported().size(), QSize(200, 150));
        // With it, the effect stays on.
        window->info_panel->btn_border->click();
        auto *persist = window->findChild<BorderShadowPopup *>()->findChild<QCheckBox *>();
        QVERIFY(persist);
        persist->setChecked(true);
        QVERIFY(config_->get_app_setting("screenshot_border_persist").toBool());
        window->cleanup_and_close();
        window = open_with_selection();
        QVERIFY(window->info_panel->btn_border->isChecked());
        QCOMPARE(exported().size(), QSize(220, 170));
        window->info_panel->btn_border->click();
        QVERIFY(!window->findChild<BorderShadowPopup *>()->isVisible());
        QCOMPARE(exported().size(), QSize(200, 150));
    }

    // The lock keeps the selection's ratio while a corner is dragged.
    void lockingTheRatioKeepsItWhileResizing() {
        auto *window = open_with_selection();
        window->info_panel->btn_lock->click();
        send_mouse(window, QEvent::MouseButtonPress, Qt::LeftButton, QPoint(250, 190), Qt::LeftButton);
        send_mouse(window, QEvent::MouseMove, Qt::NoButton, QPoint(270, 195), Qt::LeftButton);
        send_mouse(window, QEvent::MouseMove, Qt::NoButton, QPoint(290, 200), Qt::LeftButton);
        send_mouse(window, QEvent::MouseButtonRelease, Qt::LeftButton, QPoint(290, 200), Qt::NoButton);
        QCOMPARE(window->scene->selection_model->rect(), QRectF(50, 40, 240, 180));
        // Unlocked, the corner moves freely again.
        window->info_panel->btn_lock->click();
        send_mouse(window, QEvent::MouseButtonPress, Qt::LeftButton, QPoint(290, 220), Qt::LeftButton);
        send_mouse(window, QEvent::MouseMove, Qt::NoButton, QPoint(330, 280), Qt::LeftButton);
        send_mouse(window, QEvent::MouseButtonRelease, Qt::LeftButton, QPoint(330, 280), Qt::NoButton);
        QCOMPARE(window->scene->selection_model->rect(), QRectF(50, 40, 280, 240));
    }

    // Holding the refresh button recaptures the screens behind the selection.
    void refreshReplacesTheBackground() {
        auto *window = open_with_selection();
        const QImage before = window->original_image;
        QTest::mousePress(window->info_panel->btn_refresh, Qt::LeftButton);
        QVERIFY(window->original_image.cacheKey() != before.cacheKey());
        // Held past the long press, it keeps refreshing.
        const auto first = window->original_image.cacheKey();
        QTRY_VERIFY_WITH_TIMEOUT(window->original_image.cacheKey() != first, 3000);
        QTest::mouseRelease(window->info_panel->btn_refresh, Qt::LeftButton);
        const auto last = window->original_image.cacheKey();
        QTest::qWait(400);
        QCOMPARE(window->original_image.cacheKey(), last);
        QCOMPARE(window->scene->selection_model->rect(), QRectF(50, 40, 200, 150));
    }

    // ---- Toolbar actions that leave the capture ----

    // Save asks for a file, starting in the save folder; the chosen filter decides the format of a name without one.
    void saveAsksForAFileAndWritesTheSelection() {
        auto *window = open_with_selection();
        QSignalSpy ended(window, &ScreenshotWindow::session_ended);
        QTemporaryDir out;
        QString opened_in;
        // Cancelling keeps the capture open.
        answer_file_dialog({}, {}, &opened_in);
        window->toolbar->save_btn->click();
        QCOMPARE(ended.count(), 0);
        QVERIFY(window->session_active());
        QCOMPARE(QDir(opened_in), QDir(dir_.filePath("captures")));
        QVERIFY(QFileInfo(dir_.filePath("captures")).isDir());
        answer_file_dialog(out.filePath("shot"), "JPG (*.jpg)");
        window->toolbar->save_btn->click();
        QCOMPARE(ended.count(), 1);
        const QImage saved(out.filePath("shot.jpg"));
        QCOMPARE(saved.size(), QSize(200, 150));
        QCOMPARE(QImageReader(out.filePath("shot.jpg")).format(), QByteArray("jpeg"));
        // A typed extension wins over the filter.
        window = open_with_selection();
        answer_file_dialog(out.filePath("typed.bmp"), "JPG (*.jpg)");
        window->toolbar->save_btn->click();
        QCOMPARE(QImageReader(out.filePath("typed.bmp")).format(), QByteArray("bmp"));
    }

    // The quick-save action writes to the save folder, with the unannotated original beside it.
    void quickSaveKeepsTheOriginalBesideAnnotatedCaptures() {
        config_->set_screenshot_save_original_enabled(true);
        auto *window = open_with_selection();
        auto *actions = window->action_handler.get();
        QVERIFY(!actions->handle_capture_action("unknown"));
        window->toolbar->rect_btn->click();
        drag(window, QPoint(80, 70), QPoint(160, 130));
        QVERIFY(actions->handle_capture_action("quick_save"));
        QVERIFY(!window->session_active());
        QTRY_COMPARE(saved_files().size(), 2);
        bool original = false;
        for (const auto &name : saved_files()) original |= name.contains("_original");
        QVERIFY(original);
        // Without a selection only "close" acts.
        window = open();
        QVERIFY(!window->action_handler->handle_capture_action("copy"));
        QVERIFY(window->action_handler->handle_capture_action("close"));
        QVERIFY(!window->session_active());
    }

    // Recognition, code scanning and screenshot translation close the capture and open their windows.
    void recognitionScanningAndTranslationHandOffTheSelection() {
        auto *window = open_with_selection();
        window->toolbar->text_recognize_btn->click();
        QVERIFY(!window->session_active());
        TextRecognitionWindow *text = nullptr;
        QTRY_VERIFY((text = visible_window<TextRecognitionWindow>()));
        text->close();
        window = open_with_selection();
        window->toolbar->scan_code_btn->click();
        QVERIFY(!window->session_active());
        BarcodeResultWindow *codes = nullptr;
        QTRY_VERIFY((codes = visible_window<BarcodeResultWindow>()));
        codes->close();
        // Copying the text directly shows only a short notice.
        config_->set_ocr_copy_directly_enabled(true);
        window = open_with_selection();
        window->toolbar->text_recognize_btn->click();
        QVERIFY(!window->session_active());
        shutdown_recognition();
        // Screenshot translation: an engine without a key answers locally.
        config_->set_translation_provider("deepl");
        window = open_with_selection();
        window->toolbar->screenshot_translate_btn->click();
        QVERIFY(!window->session_active());
        QVERIFY(TranslationManager::instance()->is_dialog_open());
        TranslationManager::cleanup();
        // Without a confirmed selection a warning explains why nothing happens.
        window = open();
        window->action_handler->handle_scan_code();
        QVERIFY(window->session_active());
        QWidget *warning = nullptr;
        for (auto *widget : QApplication::topLevelWidgets())
            if (qobject_cast<QDialog *>(widget) && widget->isVisible()) warning = widget;
        QVERIFY(warning);
        warning->close();
    }

    // ---- The capture toolbar's panels, placement and layout ----

    // Panel style changes become the tools' saved settings; a temporary cross-tool edit leaves them.
    void toolbarPanelStylesAreSaved() {
        auto *window = open_with_selection();
        auto *toolbar = window->toolbar;
        for (const auto &tool_id : Toolbar::PANEL_TOOLS) toolbar->ensure_panel_for_tool(tool_id);
        const auto setting = [this](const char *tool, const char *key) { return config_->get_tool_settings(tool)->get(key); };
        toolbar->rect_btn->click();
        emit toolbar->shape_panel->line_style_changed("dashed");
        QCOMPARE(setting("rect", "line_style").toString(), QString("dashed"));
        QSignalSpy arrow_style(toolbar, &Toolbar::arrow_style_changed);
        emit toolbar->arrow_panel->arrow_style_changed("bar");
        QCOMPARE(setting("arrow", "arrow_style").toString(), QString("bar"));
        QCOMPARE(arrow_style.size(), 1);
        emit toolbar->mosaic_panel->draw_mode_changed("rect");
        QCOMPARE(setting("mosaic", "draw_mode").toString(), QString("rect"));
        QSignalSpy mosaic_style(toolbar, &Toolbar::mosaic_style_changed);
        emit toolbar->mosaic_panel->style_changed("blur");
        QCOMPARE(setting("mosaic", "style").toString(), QString("blur"));
        QCOMPARE(mosaic_style.size(), 1);
        emit toolbar->mosaic_panel->block_size_changed(14);
        QCOMPARE(setting("mosaic", "block_size").toInt(), 14);
        emit toolbar->paint_panel->highlighter_mode_changed("rect");
        QCOMPARE(setting("highlighter", "draw_mode").toString(), QString("rect"));
        QSignalSpy next(toolbar, &Toolbar::number_next_changed);
        emit toolbar->number_panel->next_number_changed(5);
        QCOMPARE(next.size(), 1);
        QSignalSpy text_color(toolbar, &Toolbar::text_color_changed);
        emit toolbar->text_panel->color_changed(QColor("#123456"));
        QCOMPARE(text_color.size(), 1);
        QCOMPARE(setting("text", "color").toString(), QString("#123456"));
        emit toolbar->text_panel->background_changed(true, QColor(255, 255, 0), 120);
        emit toolbar->text_panel->outline_changed(true, QColor(0, 0, 255), 2.0);
        emit toolbar->text_panel->shadow_changed(true, QColor(0, 0, 0, 80));
        emit toolbar->text_panel->background_changed(false, QColor(255, 255, 0), 120);
        emit toolbar->text_panel->outline_changed(false, QColor(0, 0, 255), 2.0);
        emit toolbar->text_panel->shadow_changed(false, QColor(0, 0, 0, 80));
        // A temporary edit of a selected item changes only that item.
        toolbar->temporary_edit_active = true;
        emit toolbar->arrow_panel->arrow_style_changed("double");
        emit toolbar->mosaic_panel->style_changed("pixel");
        emit toolbar->number_panel->next_number_changed(9);
        emit toolbar->text_panel->color_changed(QColor("#654321"));
        QCOMPARE(setting("arrow", "arrow_style").toString(), QString("bar"));
        QCOMPARE(setting("mosaic", "style").toString(), QString("blur"));
        QCOMPARE(setting("text", "color").toString(), QString("#123456"));
        QCOMPARE(next.size(), 1);
        QCOMPARE(arrow_style.size(), 2);
        toolbar->temporary_edit_active = false;
        // Rescaling keeps the toolbar in place and working.
        const QPoint before = toolbar->pos();
        toolbar->apply_scale();
        QVERIFY(toolbar->isVisible());
        QCOMPARE(toolbar->pos(), before);
    }

    // The handle places the toolbar by hand; a double click on it returns it to its place.
    void theToolbarIsDraggedByItsHandleAndReset() {
        auto *window = open_with_selection();
        auto *toolbar = window->toolbar;
        auto *handle = toolbar->drag_handle;
        QVERIFY(handle && handle->isVisible());
        const QPoint home = toolbar->pos();
        const QPoint grip = handle->mapToGlobal(handle->rect().center());
        const auto send = [handle](QEvent::Type type, const QPoint &global, Qt::MouseButtons buttons) {
            QMouseEvent event(type, QPointF(handle->mapFromGlobal(global)), QPointF(global), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, Qt::NoModifier);
            QApplication::sendEvent(handle, &event);
        };
        send(QEvent::MouseButtonPress, grip, Qt::LeftButton);
        send(QEvent::MouseMove, grip + QPoint(-20, -15), Qt::LeftButton);
        send(QEvent::MouseMove, grip + QPoint(-40, -30), Qt::LeftButton);
        send(QEvent::MouseButtonRelease, grip + QPoint(-40, -30), Qt::NoButton);
        QCOMPARE(toolbar->pos(), home + QPoint(-40, -30));
        QTest::mouseDClick(handle, Qt::LeftButton);
        QCOMPARE(toolbar->pos(), home);
    }

    // "Adjust" in the "..." popup edits the layout in a dialog and rearranges at once.
    void theLayoutDialogOpensFromTheMorePopup() {
        auto *window = open_with_selection();
        auto *toolbar = window->toolbar;
        toolbar->more_btn->click();
        QPushButton *adjust = nullptr;
        for (auto *widget : QApplication::topLevelWidgets())
            if (widget->isVisible())
                for (auto *button : widget->findChildren<QPushButton *>())
                    if (button->text() == i18n_tr("Adjust", "Toolbar") && button->isVisible()) adjust = button;
        QVERIFY(adjust);
        bool opened = false;
        QTimer::singleShot(0, this, [&opened] {
            if (auto *dialog = qobject_cast<ToolbarLayoutDialog *>(QApplication::activeModalWidget())) {
                opened = true;
                dialog->accept();
            }
        });
        adjust->click();
        QVERIFY(opened);
        QVERIFY(toolbar->isVisible());
    }

    // With cross-tool selection on, Ctrl+click takes an item of another tool: the toolbar switches
    // to that tool and shows the item's style.
    void ctrlClickEditsItemsOfOtherTools() {
        config_->set_cross_tool_selection_enabled(true);
        auto *window = open_with_selection();
        const auto draw = [window](QPushButton *tool, const QPoint &from, const QPoint &to) {
            tool->click();
            drag(window, from, to);
        };
        draw(window->toolbar->arrow_btn, QPoint(60, 50), QPoint(140, 70));
        draw(window->toolbar->ellipse_btn, QPoint(60, 90), QPoint(120, 130));
        draw(window->toolbar->pen_btn, QPoint(160, 60), QPoint(230, 90));
        window->toolbar->number_btn->click();
        click(window, Qt::LeftButton, QPoint(200, 160));
        window->toolbar->mosaic_btn->click();
        drag(window, QPoint(140, 140), QPoint(170, 170));
        window->toolbar->rect_btn->click();
        const auto ctrl_click = [window](const QPoint &pos) {
            send_mouse(window, QEvent::MouseButtonPress, Qt::LeftButton, pos, Qt::LeftButton, Qt::ControlModifier);
            send_mouse(window, QEvent::MouseButtonRelease, Qt::LeftButton, pos, Qt::NoButton, Qt::ControlModifier);
            QApplication::processEvents();
        };
        auto *controller = window->view->smart_edit_controller.get();
        // The arrow: the toolbar follows and its colour changes only the arrow.
        ctrl_click(QPoint(100, 60));
        QCOMPARE(window->toolbar->current_tool, std::optional<QString>("arrow"));
        // The switch is real: the arrow tool is current, so the item is an ordinary selection of it
        // and the panel shows its style.
        auto *arrow = dynamic_cast<ArrowItem *>(controller->selected_item());
        QVERIFY(arrow);
        QCOMPARE(window->scene->tool_controller->current_tool_id(), QString("arrow"));
        QVERIFY(!controller->is_cross_tool_selection());
        for (const QPoint &spot : {QPoint(60, 110), QPoint(195, 75), QPoint(200, 160)}) {
            ctrl_click(spot);
            QVERIFY2(controller->selected_item(), qPrintable(QString("%1,%2").arg(spot.x()).arg(spot.y())));
        }
        QCOMPARE(window->toolbar->current_tool, std::optional<QString>("number"));
        // The wheel sizes the selection; Ctrl+wheel fades it.
        auto *number = dynamic_cast<NumberItem *>(controller->selected_item());
        QVERIFY(number);
        const double radius = number->radius;
        QTest::wheelEvent(window->windowHandle(), QPointF(200, 160), QPoint(0, 120));
        QVERIFY(number->radius != radius);
        // The mosaic shows its mode and strength in the panel.
        ctrl_click(QPoint(155, 155));
        QCOMPARE(window->toolbar->current_tool, std::optional<QString>("mosaic"));
    }

    // ---- Keyboard actions of the capture window ----

    // The magnifier's keys: zoom, cycle the colour format, copy the colour; arrows nudge the pointer.
    void magnifierAndPointerKeys() {
        const auto bindings = load_inapp_bindings();
        const auto press = [&bindings](ScreenshotWindow *window, const QString &key) {
            const auto binding = bindings.value(key);
            QTest::keyClick(window, Qt::Key(binding.first), binding.second);
        };
        auto *window = open();
        send_mouse(window, QEvent::MouseMove, Qt::NoButton, QPoint(120, 100), Qt::NoButton);
        auto *magnifier = window->magnifier_overlay;
        QVERIFY(magnifier && magnifier->cursor_scene_pos && magnifier->should_render());
        const double zoom = magnifier->get_zoom_factor();
        press(window, "inapp_zoom_in");
        QVERIFY(magnifier->get_zoom_factor() > zoom);
        press(window, "inapp_zoom_out");
        QCOMPARE(magnifier->get_zoom_factor(), zoom);
        QTest::keyClick(window, Qt::Key_Shift);
        QVERIFY(window->session_active());
        QCursor::setPos(300, 300);
        QTest::keyClick(window, Qt::Key_Left);
        QVERIFY(QCursor::pos() != QPoint(300, 300));
        // C copies the colour under the pointer and ends the capture.
        QTest::keyClick(window, Qt::Key_C);
        QVERIFY(!window->session_active());
        QVERIFY(wait_for_clipboard_writes(10.0));
    }

    // Confirm, translate and recognize keys act only on a confirmed selection.
    void confirmTranslateAndRecognizeKeys() {
        const auto bindings = load_inapp_bindings();
        const auto press = [&bindings](ScreenshotWindow *window, const QString &key) {
            const auto binding = bindings.value(key);
            QTest::keyClick(window, Qt::Key(binding.first), binding.second);
        };
        auto *window = open();
        press(window, "inapp_confirm");
        QVERIFY(window->session_active());
        window->cleanup_and_close();
        window = open_with_selection();
        press(window, "inapp_confirm");
        QVERIFY(!window->session_active());
        QVERIFY(wait_for_clipboard_writes(10.0));
        window = open_with_selection();
        QSignalSpy recognize(window->toolbar, &Toolbar::text_recognize_clicked);
        press(window, "inapp_text_recognize");
        QCOMPARE(recognize.size(), 1);
        for (auto *widget : QApplication::topLevelWidgets())
            if (qobject_cast<TextRecognitionWindow *>(widget)) widget->close();
        config_->set_translation_provider("deepl");
        window = open_with_selection();
        QSignalSpy translate(window->toolbar, &Toolbar::screenshot_translate_clicked);
        press(window, "inapp_translate");
        QCOMPARE(translate.size(), 1);
        TranslationManager::cleanup();
    }
};

QTEST_MAIN(ScreenshotWindowTests)
#include "test_screenshot_window.moc"
