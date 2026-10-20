#include "canvas/items/watermark_item.h"
#include "core/qt_utils.h"
#include "core/i18n.h"
#include "core/shortcut_manager.h"
#include "core/ui_scale.h"
#include "pin/pin_toolbar.h"
#include "settings/color_formats.h"
#include "settings/tool_settings.h"
#include "ui/base_settings_panel.h"
#include "ui/dialogs.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/labels.h"
#include "ui/hotkey_edit.h"
#include "ui/mosaic_settings_panel.h"
#include "ui/arrow_settings_panel.h"
#include "ui/paint_settings_panel.h"
#include "ui/color_picker_button.h"
#include "ui/color_picker_dialog.h"
#include "ui/magnifier.h"
#include "ui/screen_color_picker.h"
#include "ui/number_settings_panel.h"
#include "ui/text_settings_panel.h"
#include "canvas/items/text_item.h"
#include "ui/inapp_key_edit.h"
#include "ui/key_chip.h"
#include "ui/reorderable_rows.h"
#include "ui/settings_ui/color_format_dialog.h"
#include "ui/toolbar.h"
#include "ui/toolbar_layout.h"
#include "ui/toolbar_layout_dialog.h"
#include "ui/watermark_input_dialog.h"
#include "ui/watermark_settings_panel.h"
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QFrame>
#include <QDialogButtonBox>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QToolButton>
#include <QAbstractItemView>
#include <algorithm>
#include <stdexcept>
#include <QMouseEvent>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QSlider>
#include <QTemporaryDir>
#include <QFile>
#include <QTest>
#include <QTimer>
#include <QToolButton>

using namespace jietuba;
namespace tl = jietuba::toolbar_layout;

namespace {
const QDateTime NOW(QDate(2026, 10, 11), QTime(9, 5, 7));

// Runs action on the next modal dialog once it is open; the caller then opens it.
void on_next_modal(std::function<void(QWidget *)> action) {
    QTimer::singleShot(0, qApp, [action] {
        QTimer *poll = new QTimer(qApp);
        poll->setInterval(10);
        QObject::connect(poll, &QTimer::timeout, qApp, [poll, action] {
            if (auto *modal = QApplication::activeModalWidget()) {
                poll->stop();
                poll->deleteLater();
                action(modal);
            }
        });
        poll->start();
    });
}

QPushButton *button_with_text(QWidget *parent, const QString &text) {
    for (auto *button : parent->findChildren<QPushButton *>())
        if (button->text() == text) return button;
    return nullptr;
}

QJsonArray layout_with(const QString &first = {}, const QMap<QString, QString> &modes = {}, const tl::LayoutProfile &profile = tl::SCREENSHOT) {
    QStringList order = profile.order;
    if (!first.isEmpty()) {
        order.removeAll(first);
        order.prepend(first);
    }
    QJsonArray layout;
    for (const auto &key : order) layout.append(QJsonArray{key, modes.value(key, tl::SHOW)});
    return layout;
}

// The buttons actually laid out on the toolbar, left to right.
QStringList toolbar_row(Toolbar &toolbar) {
    const QList<std::pair<QString, QWidget *>> buttons = {
        {"long_screenshot", toolbar.long_screenshot_btn}, {"save", toolbar.save_btn}, {"copy", toolbar.copy_btn},
        {"screenshot_translate", toolbar.screenshot_translate_btn}, {"text_recognize", toolbar.text_recognize_btn},
        {"scan_code", toolbar.scan_code_btn}, {"gif", toolbar.gif_btn}, {"pen", toolbar.pen_btn},
        {"highlighter", toolbar.highlighter_btn}, {"mosaic", toolbar.mosaic_btn}, {"spotlight", toolbar.spotlight_btn},
        {"watermark", toolbar.watermark_btn}, {"arrow", toolbar.arrow_btn}, {"number", toolbar.number_btn},
        {"rect", toolbar.rect_btn}, {"ellipse", toolbar.ellipse_btn}, {"text", toolbar.text_btn},
        {"eraser", toolbar.eraser_btn}, {"undo", toolbar.undo_btn}, {"redo", toolbar.redo_btn},
        {"cancel", toolbar.cancel_btn}, {"pin", toolbar.pin_btn}, {"confirm", toolbar.confirm_btn}, {"more", toolbar.more_btn}};
    QList<std::pair<int, QString>> shown;
    for (const auto &[key, button] : buttons)
        if (button->parentWidget() == &toolbar && !button->isHidden()) shown.append({button->x(), key});
    std::sort(shown.begin(), shown.end());
    QStringList row;
    for (const auto &entry : shown) row << entry.second;
    return row;
}

QMap<QString, ToolbarLayoutRow *> dialog_rows(ToolbarLayoutDialog &dialog) {
    QMap<QString, ToolbarLayoutRow *> rows;
    for (auto *row : dialog.findChild<ReorderableRowList *>()->rows()) rows[static_cast<ToolbarLayoutRow *>(row)->key] = static_cast<ToolbarLayoutRow *>(row);
    return rows;
}

QString mode_in(const QJsonArray &layout, const QString &key) {
    for (const auto &entry : layout)
        if (entry.toArray()[0].toString() == key) return entry.toArray()[1].toString();
    return {};
}

// Drags a row's grip so the row lands where global_y is.
void drag_grip(DraggableRow *row, int global_y) {
    auto *grip = row->grip;
    const QPoint local(grip->width() / 2, grip->height() / 2);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(local), QPointF(grip->mapToGlobal(local)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(grip, &press);
    const QPoint target(grip->mapToGlobal(local).x(), global_y);
    QMouseEvent move(QEvent::MouseMove, QPointF(grip->mapFromGlobal(target)), QPointF(target), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(grip, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(grip->mapFromGlobal(target)), QPointF(target), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(grip, &release);
}

// A mouse event at local, sent straight to widget as a grabbing drag delivers it, inside or not.
void send_mouse(QWidget *widget, QEvent::Type type, const QPointF &local, Qt::MouseButton button = Qt::LeftButton) {
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(Qt::LeftButton);
    QMouseEvent event(type, local, widget->mapToGlobal(local), type == QEvent::MouseMove ? Qt::NoButton : button, held, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}

bool close_to(double a, double b) { return std::abs(a - b) < 0.005; }

}

class UiWidgetTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir dir_;
    QSettings *settings_ = nullptr;
    ToolSettingsManager *config_ = nullptr;

private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        settings_ = new QSettings(dir_.filePath("settings.ini"), QSettings::IniFormat, this);
        config_ = get_tool_settings_manager(settings_);
    }

    void init() {
        settings_->clear();
        config_->set_screenshot_watermark_text("WATERMARK");
    }

    void cleanup() {
        for (auto *widget : QApplication::topLevelWidgets()) widget->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    // ---- Key chips ----

    void shortcutValuesShowAsReadableKeys_data() {
        QTest::addColumn<QString>("value");
        QTest::addColumn<QString>("expected");
        QTest::newRow("empty") << "" << "";
        QTest::newRow("combo") << "ctrl+shift+a" << "Ctrl + Shift + A";
        QTest::newRow("function") << "f3" << "F3";
        QTest::newRow("named") << "pageup" << "PageUp";
        QTest::newRow("escape") << "escape" << "Esc";
        QTest::newRow("unfinished") << "ctrl+" << "Ctrl +";
        QTest::newRow("plus") << "ctrl++" << "Ctrl + +";
        QTest::newRow("mouse") << "mouseforward" << "Mouse Forward";
    }

    void shortcutValuesShowAsReadableKeys() {
        QFETCH(QString, value);
        QFETCH(QString, expected);
        QCOMPARE(format_shortcut_text(value), expected);
    }

    void theCrossClearsAndTheTextKeepsTheBinding() {
        InAppKeyEdit chip;
        chip.resize(dialog_scaled(CHIP_WIDTH), dialog_scaled(CHIP_HEIGHT));
        chip.setText("ctrl+c");
        chip.show();
        QTest::mouseClick(&chip, Qt::LeftButton, {}, QPoint(10, chip.height() / 2));
        QCOMPARE(chip.text(), QString("ctrl+c"));
        // Hovering the cross paints it highlighted; the error state paints red.
        QTest::mouseMove(&chip, QPoint(chip.width() - chip.height() / 2, chip.height() / 2));
        chip.setErrorState(true);
        QVERIFY(chip.errorState());
        QVERIFY(!chip.grab().isNull());
        chip.setErrorState(false);
        QTest::mouseClick(&chip, Qt::LeftButton, {}, QPoint(chip.width() - chip.height() / 2, chip.height() / 2));
        QCOMPARE(chip.text(), QString());
        QVERIFY(chip.sizeHint().width() > 0 && chip.minimumSizeHint().height() > 0);
    }

    void hotkeyEditMarksIdleOkAndConflicts() {
        HotkeyEdit widget;
        widget.show();
        QCOMPARE(widget.status_state(), QString("idle"));
        QVERIFY(!widget.edit->errorState());
        widget.set_validation_error("duplicated");
        QCOMPARE(widget.status_state(), QString("error"));
        QVERIFY(widget.edit->errorState());
        QCOMPARE(widget.validation_error(), QString("duplicated"));
        widget.set_validation_error();
        // A key typed into the editor becomes the binding, modifiers first.
        QSignalSpy changed(&widget, &HotkeyEdit::textChanged);
        QKeyEvent press(QEvent::KeyPress, Qt::Key_K, Qt::ControlModifier | Qt::AltModifier);
        widget.edit->capture_key(&press);
        QCOMPARE(widget.text(), QString("ctrl+alt+k"));
        QVERIFY(changed.count() >= 1);
        // An unlikely combination is free on this desktop.
        widget.setText("ctrl+alt+shift+f12");
        QTRY_COMPARE(widget.status_state(), QString("ok"));
        QVERIFY(widget.validate_now());
        StatusIcon icon;
        icon.setState("error", "taken");
        QCOMPARE(icon.state(), QString("error"));
        QCOMPARE(icon.toolTip(), QString("taken"));
        QVERIFY(!icon.grab().isNull());
    }

    // ---- Standard dialogs ----

    void confirmDialogsAnswerByButtonAndTitleBarClose() {
        QWidget parent;
        on_next_modal([](QWidget *modal) { static_cast<QDialog *>(modal)->reject(); });
        QVERIFY(!show_confirm_dialog(&parent, "Delete", "Delete this item?"));
        on_next_modal([](QWidget *modal) { button_with_text(modal, "Yes")->click(); });
        QVERIFY(show_confirm_dialog(&parent, "Delete", "Delete this item?"));

        const std::vector<DialogButton> buttons = {{"save", "Save", QDialogButtonBox::AcceptRole, true}, {"cancel", "Cancel", QDialogButtonBox::RejectRole}};
        on_next_modal([](QWidget *modal) { static_cast<QDialog *>(modal)->reject(); });
        QCOMPARE(show_custom_confirm_dialog(&parent, "Save", "Save changes?", buttons), QVariant("cancel"));
        on_next_modal([](QWidget *modal) { button_with_text(modal, "Save")->click(); });
        QCOMPARE(show_custom_confirm_dialog(&parent, "Save", "Save changes?", buttons), QVariant("save"));

        on_next_modal([](QWidget *modal) {
            modal->findChild<QCheckBox *>()->setChecked(true);
            button_with_text(modal, "Yes")->click();
        });
        const auto [confirmed, checked] = show_confirm_checkbox_dialog(&parent, "Quit", "Quit now?", "Don't ask again");
        QVERIFY(confirmed && checked);
    }

    void messageDialogsShowTheirTextAndClose() {
        QWidget parent;
        QStringList seen;
        const auto read_and_close = [&seen](QWidget *modal) {
            for (auto *label : modal->findChildren<QLabel *>()) seen << label->text();
            for (auto *edit : modal->findChildren<QTextEdit *>()) seen << edit->toPlainText();
            static_cast<QDialog *>(modal)->accept();
        };
        on_next_modal(read_and_close);
        show_info_dialog(&parent, "Info", "info message");
        on_next_modal(read_and_close);
        show_warning_dialog(&parent, "Warning", "warning message");
        on_next_modal(read_and_close);
        show_error_dialog(&parent, "Error", "error message");
        on_next_modal(read_and_close);
        show_text_dialog(&parent, "Text", "long text content");
        for (const char *text : {"info message", "warning message", "error message", "long text content"}) QVERIFY2(seen.contains(text), text);
        // A modeless warning stays open without blocking and is kept alive until closed.
        QPointer<QDialog> modeless = show_modeless_warning_dialog(&parent, "Warning", "not blocking", true);
        QVERIFY(modeless && modeless->isVisible());
        QVERIFY(!modeless->isModal());
        modeless->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!modeless);
    }

    void execDialogLocksOnlyItsOwner() {
        QWidget owner, other;
        owner.show();
        other.show();
        QDialog dialog;
        bool owner_blocked = false, other_blocked = true;
        on_next_modal([&](QWidget *modal) {
            owner_blocked = blocking_modal(&owner) == modal;
            other_blocked = blocking_modal(&other) != nullptr;
            static_cast<QDialog *>(modal)->reject();
        });
        QCOMPARE(exec_dialog(&dialog, &owner), int(QDialog::Rejected));
        QVERIFY(owner_blocked);
        QVERIFY(!other_blocked);
    }

    // ---- Toolbar layout ----

    void layoutsNormalizeAndRoundTrip() {
        for (const QJsonValue &stored : {QJsonValue(), QJsonValue(QJsonArray()), QJsonValue("garbage"), QJsonValue(3)})
            QCOMPARE(tl::normalize_layout(stored), tl::default_layout());
        const auto layout = tl::normalize_layout(QJsonArray{QJsonArray{"mosaic", tl::HIDE}, QJsonArray{"nope", tl::SHOW}, QJsonArray{"mosaic", tl::SHOW}, "xy", 3,
                                                            QJsonArray{"save", "weird"}, QJsonArray{"confirm", tl::HIDE}});
        QMap<QString, QString> modes;
        QStringList keys;
        for (const auto &entry : layout) {
            keys << entry.toArray()[0].toString();
            modes[entry.toArray()[0].toString()] = entry.toArray()[1].toString();
        }
        auto sorted_keys = keys;
        auto sorted_default = tl::default_order();
        sorted_keys.sort();
        sorted_default.sort();
        QCOMPARE(sorted_keys, sorted_default);
        QCOMPARE(modes["mosaic"], tl::HIDE);
        QCOMPARE(modes["save"], tl::SCREENSHOT.default_mode("save"));
        // Confirm is always on the toolbar.
        QCOMPARE(modes["confirm"], tl::SHOW);
        // A button missing from an old layout comes back after its default neighbour, in its default mode.
        QJsonArray old;
        for (const auto &key : tl::default_order())
            if (key != "scan_code") old.append(QJsonArray{key, tl::SHOW});
        const auto upgraded = tl::normalize_layout(old);
        QStringList upgraded_keys;
        for (const auto &entry : upgraded) upgraded_keys << entry.toArray()[0].toString();
        QCOMPARE(upgraded_keys.indexOf("scan_code"), upgraded_keys.indexOf("text_recognize") + 1);
        QCOMPARE(upgraded[upgraded_keys.indexOf("scan_code")].toArray()[1].toString(), tl::MORE);
        const auto custom = layout_with("pin", {{"text", tl::HIDE}, {"mosaic", tl::MORE}});
        QCOMPARE(tl::save_layout(*config_, custom), custom);
        QCOMPARE(tl::load_layout(*config_), custom);
        config_->set_app_setting(tl::SCREENSHOT.setting_key, "{not json");
        QCOMPARE(tl::load_layout(*config_), tl::default_layout());
    }

    void theToolbarFoldsHidesAndRereadsItsLayout() {
        Toolbar toolbar;
        toolbar.show();
        // Low-frequency buttons start folded into "...".
        QVERIFY(toolbar.scan_code_btn->parentWidget() != &toolbar || toolbar.scan_code_btn->isHidden());
        QVERIFY(toolbar.copy_btn->isHidden());
        QCOMPARE(toolbar.more_btn->width(), toolbar.drag_handle->width());
        QCOMPARE(toolbar.more_btn->geometry().right(), toolbar.rect().right());

        const QMap<QString, QString> folded = {{"text_recognize", tl::MORE}, {"scan_code", tl::MORE}, {"spotlight", tl::MORE}, {"watermark", tl::MORE}};
        tl::save_layout(*config_, layout_with("pin", folded));
        toolbar.reload_layout();
        const int baseline_width = toolbar.width();
        auto fewer = folded;
        fewer["mosaic"] = tl::MORE;
        fewer["text"] = tl::HIDE;
        tl::save_layout(*config_, layout_with("pin", fewer));
        toolbar.reload_layout();
        QVERIFY(toolbar.width() < baseline_width);
        QVERIFY(toolbar.text_btn->isHidden() || toolbar.text_btn->parentWidget() != &toolbar);
        QVERIFY(toolbar.pin_btn->x() < toolbar.pen_btn->x());
        // A hidden tool still works when selected another way.
        toolbar.select_tool("text");
        QCOMPARE(toolbar.current_tool, std::optional<QString>("text"));

        // Folded buttons live in the "..." popup and still work.
        QSignalSpy saved(&toolbar, &Toolbar::save_clicked);
        tl::save_layout(*config_, layout_with({}, {{"mosaic", tl::MORE}, {"save", tl::MORE}}));
        toolbar.reload_layout();
        toolbar.more_btn->click();
        auto *popup = toolbar.mosaic_btn->parentWidget();
        QVERIFY(popup != &toolbar && popup->isVisible());
        QCOMPARE(toolbar.save_btn->parentWidget(), popup);
        toolbar.save_btn->click();
        QCOMPARE(saved.count(), 1);
        QVERIFY(!popup->isVisible());
        toolbar.more_btn->click();
        toolbar.mosaic_btn->click();
        QCOMPARE(toolbar.current_tool, std::optional<QString>("mosaic"));
        QVERIFY(!popup->isVisible());
        // Moving between "..." and the popup keeps it open; leaving both closes it after a grace period.
        QEnterEvent enter(QPointF(1, 1), QPointF(1, 1), QPointF(1, 1));
        QApplication::sendEvent(toolbar.more_btn, &enter);
        QTRY_VERIFY(popup->isVisible());
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(toolbar.more_btn, &leave);
        QVERIFY(popup->isVisible());
        QTRY_VERIFY(!popup->isVisible());
        // Unfolded buttons move back onto the toolbar.
        tl::save_layout(*config_, tl::default_layout());
        toolbar.reload_layout();
        QCOMPARE(toolbar.save_btn->parentWidget(), &toolbar);
    }

    // The pin toolbar keeps its former row; the tools added since wait in "...".
    void thePinToolbarKeepsItsRowAndFoldsTheNewTools() {
        PinToolbar toolbar(nullptr, config_);
        QCOMPARE(toolbar_row(toolbar), QStringList({"save", "copy", "pen", "highlighter", "mosaic", "arrow", "number", "rect",
                                                    "ellipse", "text", "eraser", "undo", "redo", "more"}));
        toolbar.show();
        toolbar.more_btn->click();
        auto *popup = toolbar.more_popup();
        QVERIFY(popup && popup->isVisible());
        QCOMPARE(toolbar.spotlight_btn->parentWidget(), popup);
        QCOMPARE(toolbar.watermark_btn->parentWidget(), popup);
    }

    void pinAndScreenshotLayoutsAreSeparate() {
        tl::save_layout(*config_, layout_with({}, {{"pen", tl::HIDE}, {"save", tl::MORE}}));
        tl::save_layout(*config_, layout_with("text", {{"mosaic", tl::HIDE}}, tl::PIN), tl::PIN);
        PinToolbar pin_toolbar(nullptr, config_);
        const auto row = toolbar_row(pin_toolbar);
        QCOMPARE(row.first(), QString("text"));
        QCOMPARE(row.last(), QString("more"));
        QVERIFY(!row.contains("mosaic"));
        for (const char *key : {"pen", "save", "spotlight", "watermark"}) QVERIFY2(row.contains(key), key);
        for (const char *key : {"screenshot_translate", "long_screenshot", "confirm"}) QVERIFY2(!row.contains(key), key);
        Toolbar toolbar;
        const auto screenshot_row = toolbar_row(toolbar);
        QVERIFY(!screenshot_row.contains("pen"));
        QVERIFY(screenshot_row.contains("mosaic"));
    }

    // Showing rereads the layout, which another pin may have changed.
    void showingPicksUpALayoutChangedInAnotherPin() {
        PinToolbar toolbar(nullptr, config_);
        tl::save_layout(*config_, layout_with({}, {{"pen", tl::HIDE}}, tl::PIN), tl::PIN);
        toolbar.show();
        QVERIFY(!toolbar_row(toolbar).contains("pen"));
        toolbar.hide();
    }

    void uncommonToolbarPanelsStayAbsentUntilUsed() {
        Toolbar toolbar;
        auto absent = [&] {
            return toolbar.paint_panel && toolbar.shape_panel && toolbar.number_panel && !toolbar.arrow_panel &&
                   !toolbar.text_panel && !toolbar.mosaic_panel && !toolbar.watermark_panel;
        };
        QVERIFY(absent());
        toolbar.apply_scale();
        toolbar.hide_all_panels();
        toolbar.set_current_color(QColor("blue"));
        toolbar.set_stroke_width(7);
        toolbar.set_opacity(180);
        toolbar.set_number_next_value(4);
        QVERIFY(absent());
        config_->get_tool_settings("number")->set("style", "hollow_bg");
        toolbar.select_tool("number");
        QVERIFY(toolbar.number_panel);
        QCOMPARE(toolbar.number_panel->current_style(), QString("hollow_bg"));
        QVERIFY(toolbar.paint_panel && toolbar.shape_panel && !toolbar.text_panel && !toolbar.mosaic_panel);
        auto *number = toolbar.number_panel;
        toolbar.select_tool("pen");
        auto *paint = toolbar.paint_panel;
        QVERIFY(paint);
        toolbar.select_tool("highlighter");
        QCOMPARE(toolbar.paint_panel, paint);
        QSignalSpy mode(&toolbar, &Toolbar::highlighter_mode_changed);
        emit paint->highlighter_mode_changed("rect");
        QCOMPARE(mode.count(), 1);
        toolbar.select_tool("number");
        QCOMPARE(toolbar.number_panel, number);
        toolbar.set_temporary_edit_active(true);
        toolbar.ensure_panel_for_tool("arrow");
        QVERIFY(toolbar.arrow_panel);
        QVERIFY(!paint->mode_widget->isEnabled());
        QVERIFY(!number->next_up_btn->isEnabled());
    }

    void toolbarPlacementFollowsTheSelectionAndScreen() {
        Toolbar toolbar;
        toolbar.show();
        const auto screen = QGuiApplication::primaryScreen()->geometry();
        // Below the selection, right-aligned.
        const QRectF selection(screen.right() - 250, screen.height() / 4.0, 200, 100);
        toolbar.position_near_rect(selection);
        QVERIFY(toolbar.y() >= selection.bottom());
        // Aligned by "Confirm", so the pointer lands on it after the drag.
        const int confirm_right = toolbar.x() + toolbar.confirm_btn->geometry().right();
        QVERIFY2(std::abs(confirm_right - selection.right()) <= 6, qPrintable(QString("%1 vs %2").arg(confirm_right).arg(selection.right())));
        // No room below: above.
        const QRectF bottom(selection.x(), screen.bottom() - 30, 200, 30);
        toolbar.position_near_rect(bottom);
        QVERIFY(toolbar.geometry().bottom() <= bottom.top());
        // Never off the left edge.
        toolbar.position_near_rect(QRectF(0, 100, 20, 20));
        QVERIFY(toolbar.x() >= screen.left());
        // Selecting tools is exclusive; clicking the active one again cancels it.
        toolbar.pen_btn->click();
        QVERIFY(toolbar.pen_btn->isChecked());
        toolbar.rect_btn->click();
        QVERIFY(!toolbar.pen_btn->isChecked() && toolbar.rect_btn->isChecked());
        toolbar.rect_btn->click();
        QVERIFY(!toolbar.current_tool);
        // Every tool brings up its own panel.
        for (const char *tool : {"pen", "highlighter", "rect", "arrow", "number", "text", "mosaic"}) {
            toolbar.select_tool(tool);
            QVERIFY2(toolbar.current_tool == std::optional<QString>(tool), tool);
        }
        QVERIFY(toolbar.mosaic_panel && toolbar.mosaic_panel->isVisible());
        toolbar.hide_all_panels();
        QVERIFY(!toolbar.mosaic_panel->isVisible());
        toolbar.set_current_color(QColor("#00ff00"));
        QCOMPARE(toolbar.current_color, QColor("#00ff00"));
        toolbar.set_stroke_width(9);
        toolbar.set_opacity(128);
        toolbar.set_number_next_value(7);
    }

    void theLayoutDialogReordersChangesModesAndRestores() {
        QMap<QString, QIcon> icons;
        for (const auto &key : tl::default_order()) icons[key] = QIcon();
        ToolbarLayoutDialog dialog(tl::default_layout(), icons);
        dialog.show();
        QApplication::processEvents();
        auto *list = dialog.findChild<ReorderableRowList *>();
        QVERIFY(list);
        QMap<QString, ToolbarLayoutRow *> rows;
        for (auto *row : list->rows()) rows[static_cast<ToolbarLayoutRow *>(row)->key] = static_cast<ToolbarLayoutRow *>(row);
        // Confirm is locked and not listed.
        QVERIFY(!rows.contains("confirm"));
        QCOMPARE(rows.size(), tl::default_order().size() - 1);
        drag_grip(rows["pin"], list->rows().first()->mapToGlobal(QPoint(0, 1)).y());
        rows["mosaic"]->set_mode(tl::MORE);
        QCOMPARE(rows["mosaic"]->mode(), tl::MORE);
        const auto entries = dialog.entries();
        QStringList keys;
        for (const auto &entry : entries) keys << entry.toArray()[0].toString();
        keys.removeAll("confirm");
        QCOMPARE(keys.first(), QString("pin"));
        QCOMPARE(entries.size(), tl::default_order().size());
        // The dialog fits its rows on screen.
        QVERIFY(dialog.height() <= QGuiApplication::primaryScreen()->availableGeometry().height());
        auto *restore = button_with_text(&dialog, "Restore defaults");
        QVERIFY(restore);
        restore->click();
        QCOMPARE(dialog.entries(), tl::default_layout());
    }

    void everyConfigurableButtonHasARowName() {
        for (const auto *profile : {&tl::SCREENSHOT, &tl::PIN}) {
            QMap<QString, QIcon> icons;
            for (const auto &key : profile->order) icons[key] = QIcon();
            ToolbarLayoutDialog dialog(tl::default_layout(*profile), icons, nullptr, *profile);
            for (auto *row : dialog_rows(dialog)) {
                auto *name = row->findChild<BodyLabel *>();
                QVERIFY2(name && !name->text().isEmpty(), qPrintable(row->key));
            }
        }
    }

    void thePinDialogEditsThePinButtons() {
        QMap<QString, QIcon> icons;
        for (const auto &key : tl::PIN.order) icons[key] = QIcon();
        ToolbarLayoutDialog dialog(tl::default_layout(tl::PIN), icons, nullptr, tl::PIN);
        dialog.show();
        QApplication::processEvents();
        const auto rows = dialog_rows(dialog);
        auto keys = rows.keys();
        auto order = tl::PIN.order;
        keys.sort();
        order.sort();
        QCOMPARE(keys, order);
        rows["watermark"]->set_mode(tl::SHOW);
        QCOMPARE(mode_in(dialog.entries(), "watermark"), tl::SHOW);
        button_with_text(&dialog, "Restore defaults")->click();
        QCOMPARE(dialog.entries(), tl::default_layout(tl::PIN));
    }

    // ---- Watermark panel and text input ----

    void theStylePopupReportsEachField() {
        WatermarkSettingsPanel panel;
        panel.show();
        QStringList changes;
        connect(&panel, &WatermarkSettingsPanel::style_changed, this, [&](const QString &key, const QVariant &value) { changes << key + "=" + value.toString(); });
        WatermarkStyle style;
        style.color = "#000000";
        style.font_size = 16;
        style.opacity = 1.0;
        panel.load_style(style);
        QVERIFY(changes.isEmpty());
        panel.show_style_popup();
        WatermarkStylePopup *popup = nullptr;
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *found = qobject_cast<WatermarkStylePopup *>(widget)) popup = found;
        QVERIFY(popup && popup->isVisible());
        const auto sliders = popup->findChildren<QSlider *>();
        QCOMPARE(sliders.size(), 4);
        QCOMPARE(sliders[0]->value(), 100);
        QCOMPARE(sliders[1]->value(), 16);
        sliders[0]->setValue(30);
        sliders[2]->setValue(55);
        sliders[3]->setValue(-45);
        popup->color_row->swatches.first().first->click();
        QCOMPARE(changes, (QStringList{"opacity=0.3", "spacing=55", "angle=-45", "color=#ff0000"}));
        // The angle applies to tiling and the diamond grid, not to a single position.
        panel.layout_combo->setCurrentIndex(panel.layout_combo->findData("center"));
        QVERIFY(!sliders[3]->isEnabled());
        QCOMPARE(changes.last(), QString("layout=center"));
        panel.layout_combo->setCurrentIndex(panel.layout_combo->findData(LAYOUT_TILE));
        QVERIFY(sliders[3]->isEnabled());
        if (panel.font_combo->count() > 1) {
            panel.font_combo->setCurrentIndex(1);
            QVERIFY(changes.last().startsWith("font_family="));
        }
        panel.retranslate();
        panel.apply_scale();
        QVERIFY(!panel.grab().isNull());
    }

    void theStylePopupAngleAndGridLinesFollowTheLayout() {
        WatermarkSettingsPanel panel;
        panel.show();
        QStringList changes;
        connect(&panel, &WatermarkSettingsPanel::style_changed, this, [&](const QString &key, const QVariant &value) { changes << key + "=" + value.toString(); });
        WatermarkStyle style;
        style.color = "#000000";
        style.font_size = 16;
        style.opacity = 1.0;
        style.layout = LAYOUT_DIAMOND;
        style.angle = -30;
        style.diamond_angle = 10;
        style.line_style = LINE_DASH;
        panel.load_style(style);
        panel.show_style_popup();
        WatermarkStylePopup *popup = nullptr;
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *found = qobject_cast<WatermarkStylePopup *>(widget)) popup = found;
        QVERIFY(popup && popup->isVisible());
        const auto sliders = popup->findChildren<QSlider *>();
        const auto buttons = popup->findChildren<QPushButton *>("GridLineButton");
        QCOMPARE(buttons.size(), 3);

        // Diamond: the angle slider reads and writes the diamond's own angle; the grid lines can be chosen.
        QCOMPARE(sliders[3]->value(), 10);
        QVERIFY(sliders[3]->isEnabled());
        QVERIFY(buttons[2]->isChecked() && buttons[1]->isEnabled());
        sliders[3]->setValue(25);
        buttons[0]->click();
        QCOMPARE(changes, (QStringList{"diamond_angle=25", "line_style=none"}));

        // Tile: the slider reads the tile's angle and the grid lines grey out.
        changes.clear();
        panel.layout_combo->setCurrentIndex(panel.layout_combo->findData(LAYOUT_TILE));
        QCOMPARE(sliders[3]->value(), -30);
        QVERIFY(!buttons[1]->isEnabled());
        sliders[3]->setValue(-40);
        QCOMPARE(changes, (QStringList{"layout=tile", "angle=-40"}));

        // A single position uses neither.
        panel.layout_combo->setCurrentIndex(panel.layout_combo->findData("center"));
        QVERIFY(!sliders[3]->isEnabled());
        QVERIFY(!buttons[0]->isEnabled());
    }

    void theTextFieldShowsItsOwnTimeAndClears() {
        WatermarkTextField field;
        field.resize(dialog_scaled(WatermarkTextField::BASE_WIDTH), dialog_scaled(WatermarkTextField::BASE_HEIGHT));
        field.show();
        field.set_template("ACME $HH:mm:ss$", NOW);
        QCOMPARE(field.text(), QString("ACME 09:05:07"));
        QCOMPARE(field.template_text(), QString("ACME $HH:mm:ss$"));
        field.retranslate();
        QVERIFY(!field.grab().isNull());
        QCOMPARE(field.text(), QString("ACME 09:05:07"));
        QSignalSpy clicked(&field, &WatermarkTextField::clicked);
        QSignalSpy cleared(&field, &WatermarkTextField::cleared);
        QTest::mouseMove(&field, QPoint(10, field.height() / 2));
        QTest::mouseClick(&field, Qt::LeftButton, {}, QPoint(10, field.height() / 2));
        QCOMPARE(clicked.count(), 1);
        QTest::mouseClick(&field, Qt::LeftButton, {}, QPoint(field.width() - dialog_scaled(WatermarkTextField::BASE_PADDING), field.height() / 2));
        QCOMPARE(cleared.count(), 1);
        // Empty: no cross, a click asks for text.
        field.set_template("");
        QTest::mouseClick(&field, Qt::LeftButton, {}, QPoint(field.width() - dialog_scaled(WatermarkTextField::BASE_PADDING), field.height() / 2));
        QCOMPARE(clicked.count(), 2);
        QVERIFY(!field.grab().isNull());
    }

    void theInputDialogInsertsTimeAndPreviews() {
        WatermarkInputDialog dialog("ACME  ", "yyyy-MM-dd HH:mm:ss");
        dialog.show();
        auto cursor = dialog.edit->textCursor();
        cursor.setPosition(4);
        dialog.edit->setTextCursor(cursor);
        dialog.insert_time();
        QCOMPARE(dialog.edit->toPlainText(), QString("ACME$yyyy-MM-dd HH:mm:ss$  "));
        QCOMPARE(dialog.time_button->text(), time_format_short_name("yyyy-MM-dd HH:mm:ss"));
        // Picking from the menu inserts that format and makes it the button's format.
        dialog.edit->moveCursor(QTextCursor::End);
        bool picked = false;
        QTimer::singleShot(0, &dialog, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) return;
            for (auto *action : menu->actions())
                if (action->text().startsWith(time_format_short_name("yyyyMMddHHmmss"))) {
                    menu->setActiveAction(action);
                    picked = true;
                }
            QTest::keyClick(menu, Qt::Key_Return);
        });
        // The menu's own loop runs until a format is picked.
        dialog.time_button->showMenu();
        QVERIFY(picked);
        QVERIFY(dialog.edit->toPlainText().endsWith(time_token("yyyyMMddHHmmss")));
        QCOMPARE(dialog.time_format(), QString("yyyyMMddHHmmss"));
        QCOMPARE(dialog.time_menu->actions().size(), static_cast<qsizetype>(watermark_time_formats().size()));
        // Leading and trailing blanks go; the preview shows the expanded text.
        QVERIFY(dialog.template_text().startsWith("ACME$"));
        QVERIFY(!dialog.preview->text().section("ACME", 1).contains('$'));
        QVERIFY(dialog.preview->isVisibleTo(&dialog));
        dialog.edit->setPlainText("");
        QVERIFY(!dialog.preview->isVisibleTo(&dialog));
        QWidget anchor;
        anchor.setGeometry(200, 200, 100, 30);
        anchor.show();
        dialog.place_near(&anchor);
        QVERIFY(dialog.isVisible());
    }

    void editingTheTextReturnsItAndRemembersTheFormat() {
        QWidget anchor;
        anchor.show();
        on_next_modal([](QWidget *modal) {
            auto *dialog = qobject_cast<WatermarkInputDialog *>(modal);
            dialog->edit->setPlainText("A ");
            dialog->edit->moveCursor(QTextCursor::End);
            dialog->insert_time("HH:mm:ss");
            dialog->accept();
        });
        QCOMPARE(edit_watermark_text(&anchor, config_), std::optional<QString>("A $HH:mm:ss$"));
        QCOMPARE(config_->get_screenshot_watermark_time_format(), QString("HH:mm:ss"));
        on_next_modal([](QWidget *modal) { static_cast<QDialog *>(modal)->reject(); });
        QCOMPARE(edit_watermark_text(&anchor, config_), std::optional<QString>());
    }

    void theToolbarWatermarkButtonAddsAndAsksForText() {
        Toolbar toolbar;
        toolbar.show();
        QSignalSpy requested(&toolbar, &Toolbar::watermark_requested);
        QSignalSpy texts(&toolbar, &Toolbar::watermark_text_changed);
        QVERIFY(!toolbar.watermark_panel);
        toolbar.watermark_btn->click();
        QVERIFY(toolbar.watermark_panel && toolbar.watermark_panel->isVisible());
        QCOMPARE(requested.count(), 1);
        QCOMPARE(toolbar.watermark_panel->text_field->template_text(), QString("WATERMARK"));
        // The text field opens the editor; its answer becomes the new text.
        on_next_modal([](QWidget *modal) {
            auto *dialog = qobject_cast<WatermarkInputDialog *>(modal);
            dialog->edit->setPlainText("ACME");
            dialog->accept();
        });
        emit toolbar.watermark_panel->text_field->clicked();
        QCOMPARE(texts.count(), 1);
        QCOMPARE(texts.last().at(0).toString(), QString("ACME"));
        QCOMPARE(toolbar.watermark_panel->text_field->template_text(), QString("ACME"));
        // Cancelling changes nothing; clearing empties it.
        on_next_modal([](QWidget *modal) { static_cast<QDialog *>(modal)->reject(); });
        emit toolbar.watermark_panel->text_field->clicked();
        QCOMPARE(texts.count(), 1);
        emit toolbar.watermark_panel->text_field->cleared();
        QCOMPARE(texts.last().at(0).toString(), QString());
        // Another tool hides the panel; clicking the button again only collapses it.
        toolbar.pen_btn->click();
        QVERIFY(!toolbar.watermark_panel->isVisible());
        toolbar.watermark_btn->click();
        QCOMPARE(requested.count(), 2);
        toolbar.watermark_btn->click();
        QVERIFY(!toolbar.current_tool);
        QCOMPARE(requested.count(), 2);
    }

    void turningOnAWatermarkWithoutTextAsksForIt() {
        config_->set_screenshot_watermark_text("");
        Toolbar toolbar;
        toolbar.show();
        QSignalSpy texts(&toolbar, &Toolbar::watermark_text_changed);
        connect(&toolbar, &Toolbar::watermark_text_changed, this, [this](const QString &text) { config_->set_screenshot_watermark_text(text); });
        int asked = 0;
        on_next_modal([&asked](QWidget *modal) {
            ++asked;
            auto *dialog = qobject_cast<WatermarkInputDialog *>(modal);
            dialog->edit->setPlainText("HELLO");
            dialog->accept();
        });
        toolbar.watermark_btn->click();
        QCOMPARE(asked, 1);
        QCOMPARE(texts.last().at(0).toString(), QString("HELLO"));
        toolbar.watermark_btn->click();
        toolbar.watermark_btn->click();
        QCOMPARE(asked, 1);
    }

    // ---- Colour formats ----

    void theColorFormatDialogTracksChecksOrderAndDefaults() {
        ColorFormatDialog dialog(color_formats::default_formats());
        dialog.show();
        QApplication::processEvents();
        auto *list = dialog.findChild<ReorderableRowList *>();
        QVERIFY(list);
        QMap<QString, DraggableRow *> rows;
        for (auto *row : list->rows()) rows[row->findChildren<QLabel *>().first()->text()] = row;
        QVERIFY(rows.contains("CSS rgb()"));
        rows["CSS rgb()"]->findChild<QCheckBox *>()->setChecked(true);
        QStringList enabled;
        for (const auto &format : dialog.entries())
            if (format.enabled) enabled << format.name;
        QVERIFY(enabled.contains("CSS rgb()"));
        // Dragging a row to the top makes it the copy format.
        rows["CSS hsl()"]->findChild<QCheckBox *>()->setChecked(true);
        drag_grip(rows["CSS hsl()"], list->rows().first()->mapToGlobal(QPoint(0, 1)).y());
        QCOMPARE(dialog.entries().first().name, QString("CSS hsl()"));
        QCOMPARE(color_formats::enabled_formats(dialog.entries()).first().name, QString("CSS hsl()"));
        // Rows have nothing to add, edit or delete.
        for (auto *row : list->rows()) QCOMPARE(row->findChildren<QAbstractButton *>().size(), 1);
        button_with_text(&dialog, "Restore defaults")->click();
        const auto restored = dialog.entries();
        const auto defaults = color_formats::default_formats();
        QCOMPARE(restored.size(), defaults.size());
        for (int i = 0; i < defaults.size(); ++i) {
            QCOMPARE(restored[i].name, defaults[i].name);
            QCOMPARE(restored[i].enabled, defaults[i].enabled);
        }
        // Removing a row from the list drops it.
        list->remove_row(list->rows().last());
        QCOMPARE(list->rows().size(), defaults.size() - 1);
        QVERIFY(list->content_height() > 0);
        list->clear();
        QVERIFY(list->rows().isEmpty());
    }

    // The in-app shortcut editor records combinations as the parser's lower-case text.
    void inAppKeyEditRecordsCombinationsAndTheMiddleButton() {
        // Focused, as when a person types into it: unfocused keys go to the shortcut handlers.
        InAppKeyEdit edit;
        edit.show();
        edit.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&edit));
        edit.setFocus();
        const auto press = [&edit](int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier, const QString &text = {}) {
            QKeyEvent event(QEvent::KeyPress, key, modifiers, text);
            QApplication::sendEvent(&edit, &event);
        };
        const auto release = [&edit](int key, Qt::KeyboardModifiers modifiers) {
            QKeyEvent event(QEvent::KeyRelease, key, modifiers);
            QApplication::sendEvent(&edit, &event);
        };
        // Holding modifiers shows the unfinished prefix; letting go walks it back. A modifier
        // key's own event carries the state before it, and modifiers() toggles the key in.
        press(Qt::Key_Control, Qt::NoModifier);
        QCOMPARE(edit.text(), QString("ctrl+"));
        press(Qt::Key_Shift, Qt::ControlModifier);
        QCOMPARE(edit.text(), QString("ctrl+shift+"));
        release(Qt::Key_Shift, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(edit.text(), QString("ctrl+"));
        release(Qt::Key_Control, Qt::ControlModifier);
        QCOMPARE(edit.text(), QString());
        press(Qt::Key_Z, Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(edit.text(), QString("ctrl+alt+z"));
        // Releasing after a full combination keeps it.
        release(Qt::Key_Control, Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(edit.text(), QString("ctrl+alt+z"));
        press(Qt::Key_F7);
        QCOMPARE(edit.text(), QString("f7"));
        // Letting go of a lone modifier puts back the value from before it, not an empty one.
        press(Qt::Key_Alt, Qt::NoModifier);
        QCOMPARE(edit.text(), QString("alt+"));
        release(Qt::Key_Alt, Qt::AltModifier);
        QCOMPARE(edit.text(), QString("f7"));
        // So does leaving it unfinished.
        press(Qt::Key_Control, Qt::NoModifier);
        edit.clearFocus();
        QCOMPARE(edit.text(), QString("f7"));
        edit.setFocus();
        // Tab is recorded instead of moving the focus.
        press(Qt::Key_Tab);
        QCOMPARE(edit.text(), QString("tab"));
        press(Qt::Key_Backspace);
        QCOMPARE(edit.text(), QString());
        press(Qt::Key_A, Qt::NoModifier, "a");
        QCOMPARE(edit.text(), QString("a"));
        // Escape is reserved and clears.
        press(Qt::Key_Escape);
        QCOMPARE(edit.text(), QString());
        // The middle button with modifiers records a mouse binding.
        QTest::mousePress(&edit, Qt::MiddleButton, Qt::ShiftModifier, QPoint(5, 5));
        QCOMPARE(edit.text(), QString("shift+") + MOUSE_BUTTON_MIDDLE);
        QVERIFY(edit.property("_captures_inapp_mouse_shortcut").toBool());

        // For shortcuts used inside text fields, typing and caret keys are refused.
        InAppKeyEdit strict(nullptr, false, false);
        strict.show();
        strict.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&strict));
        strict.setFocus();
        const auto strict_press = [&strict](int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier, const QString &text = {}) {
            QKeyEvent event(QEvent::KeyPress, key, modifiers, text);
            QApplication::sendEvent(&strict, &event);
        };
        strict_press(Qt::Key_A, Qt::NoModifier, "a");
        strict_press(Qt::Key_Left);
        QCOMPARE(strict.text(), QString());
        strict_press(Qt::Key_A, Qt::ControlModifier, "\x01");
        QCOMPARE(strict.text(), QString("ctrl+a"));
        // Without mouse support the middle button records nothing.
        QTest::mousePress(&strict, Qt::MiddleButton, Qt::NoModifier, QPoint(5, 5));
        QCOMPARE(strict.text(), QString("ctrl+a"));
    }

    // XML catalogs: per-context lookups, a shared fallback, and untranslated entries kept as source.
    void xmlTranslationsLoadAndFallBack() {
        QTemporaryDir dir;
        QFile file(dir.filePath("app_xx.xml"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"(<?xml version="1.0" encoding="utf-8"?>
<TS version="2.1" language="xx">
<context><name>First</name>
  <message><source>Save</source><translation>Sauver</translation></message>
  <message><source>Open</source><translation type="unfinished">Ouvrir</translation></message>
  <message><source>Close</source><translation></translation></message>
</context>
<context><name>Second</name>
  <message><source>Only here</source><translation>Seulement ici</translation></message>
</context>
</TS>)");
        file.close();
        XmlTranslator translator;
        QVERIFY(translator.isEmpty());
        QVERIFY(translator.load_from_xml(file.fileName()));
        QVERIFY(!translator.isEmpty());
        QCOMPARE(translator.translate("First", "Save"), QString("Sauver"));
        QCOMPARE(translator.translate("First", "Open"), QString("Open"));
        QCOMPARE(translator.translate("First", "Close"), QString("Close"));
        QCOMPARE(translator.translate("Elsewhere", "Only here"), QString("Seulement ici"));
        QCOMPARE(translator.translate("First", "Missing"), QString());
        QVERIFY(!translator.load_from_xml(dir.filePath("missing.xml")));
        QFile broken(dir.filePath("broken.xml"));
        QVERIFY(broken.open(QIODevice::WriteOnly));
        broken.write("<TS><context><name>x</name>");
        broken.close();
        QVERIFY(!translator.load_from_xml(broken.fileName()));
        const auto tr = make_tr("First");
        QCOMPARE(tr("Not in any catalog"), QString("Not in any catalog"));
        QVERIFY(!I18nManager::load_language("xx-unsupported"));
    }

    // The number panel steps the next number and picks a style from the popup over its preview.
    void numberPanelStepsAndPicksAStyle() {
        NumberSettingsPanel panel;
        panel.show();
        QSignalSpy next(&panel, &NumberSettingsPanel::next_number_changed);
        QSignalSpy style(&panel, &NumberSettingsPanel::style_changed);
        panel.set_next_number(3);
        panel.next_up_btn->click();
        QCOMPARE(next.last().first().toInt(), 4);
        for (int i = 0; i < 6; ++i) panel.next_down_btn->click();
        // Never below one.
        QCOMPARE(next.last().first().toInt(), 1);
        // Hovering the preview opens the style choices.
        QEnterEvent enter(QPointF(2, 2), QPointF(2, 2), QPointF(2, 2));
        QApplication::sendEvent(panel.next_preview, &enter);
        NumberStylePopup *popup = panel.findChild<NumberStylePopup *>();
        QVERIFY(popup && popup->isVisible());
        auto buttons = popup->findChildren<QToolButton *>();
        QCOMPARE(buttons.size(), 4);
        buttons.last()->click();
        QCOMPARE(style.size(), 1);
        QCOMPARE(panel.current_style(), style.first().first().toString());
        QVERIFY(buttons.last()->isChecked());
        QVERIFY(!popup->isVisible());
        QApplication::sendEvent(panel.next_preview, &enter);
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(panel.next_preview, &leave);
        QTRY_VERIFY(!popup->isVisible());
        panel.apply_scale();
    }

    // The text panel mirrors a selected text item and reports effect changes.
    void textPanelMirrorsTheSelectedItem() {
        TextSettingsPanel panel;
        panel.show();
        TextItem item("sample", QPointF(), QFont("Arial", 21, QFont::Bold), QColor("#336699"));
        item.has_background = true;
        item.background_color = QColor(255, 255, 0, 140);
        item.set_outline(true, QColor("#ffffff"), QVariant(TextItem::OUTLINE_WIDTH_LEVELS.back()));
        item.set_shadow(true, QColor(0, 0, 0, 90));
        QSignalSpy outline(&panel, &TextSettingsPanel::outline_changed);
        panel.set_state_from_item(&item);
        panel.set_state_from_item(nullptr);
        QVERIFY(panel.bold_btn->isChecked());
        QVERIFY(panel.background_btn->isChecked());
        QVERIFY(panel.outline_btn->isChecked());
        QVERIFY(panel.shadow_btn->isChecked());
        QCOMPARE(panel.background_opacity_slider->value(), 140);
        QCOMPARE(panel.outline_width_slider->value(), panel.outline_width_slider->maximum());
        // Mirroring never reports changes back.
        QCOMPARE(outline.size(), 0);
        panel.outline_width_slider->setValue(0);
        QCOMPARE(outline.size(), 1);
        // The same level again is no change.
        panel.outline_width_slider->setValue(1);
        panel.outline_width_slider->setValue(0);
        QCOMPARE(outline.size(), 3);
        QVERIFY(!panel.outline_width_slider->grab().isNull());
        QSignalSpy background(&panel, &TextSettingsPanel::background_changed);
        panel.background_btn->click();
        QCOMPARE(background.size(), 1);
        QCOMPARE(background.first().first().toBool(), false);
        panel.background_btn->click();
        QCOMPARE(background.last().first().toBool(), true);
        panel.retranslate();
        QVERIFY(!panel.grab().isNull());
    }

    // The arrow panel: preset colours, a picked colour carrying opacity, and styles drawn as previews in the list.
    void arrowPanelColoursAndStylePreviews() {
        ArrowSettingsPanel panel;
        panel.show();
        QSignalSpy colour(&panel, &ArrowSettingsPanel::color_changed);
        auto swatches = panel.findChildren<ColorSwatch *>();
        QVERIFY(!swatches.isEmpty());
        swatches.last()->click();
        QCOMPARE(colour.size(), 1);
        QCOMPARE(panel.color_btn->color(), panel.current_color);
        QColor faint("#3366ff");
        faint.setAlpha(128);
        emit panel.color_btn->color_changed(faint);
        QCOMPARE(colour.size(), 2);
        QCOMPARE(panel.opacity_spin->value(), 50);
        panel.arrow_style_combo->showPopup();
        QApplication::processEvents();
        QVERIFY(!panel.arrow_style_combo->view()->grab().isNull());
        panel.arrow_style_combo->hidePopup();
    }

    // ---- Colour dialog ----

    // Black has no saturation and greys no hue; the field and the hue track keep theirs.
    void colorDialogKeepsHueThroughBlackAndGrey() {
        ColorPickerDialog dialog(QColor("#3366CC"));
        dialog.show();
        auto *square = dialog.square;
        const double hue = 220.0 / 360;
        QVERIFY(close_to(square->hue(), hue));
        QVERIFY(close_to(dialog.hue_slider->value(), hue));
        const QRectF field = square->field_rect();
        // Dragged far past the bottom-right corner: clamped to full saturation and black.
        send_mouse(square, QEvent::MouseButtonPress, QPointF(field.center().x(), field.bottom()));
        send_mouse(square, QEvent::MouseMove, field.bottomRight() + QPointF(60, 60));
        QCOMPARE(square->saturation(), 1.0);
        QCOMPARE(square->value(), 0.0);
        QCOMPARE(dialog.currentColor(), QColor(0, 0, 0));
        QCOMPARE(dialog.value_edit->text(), QString("#000000"));
        // Up past the top edge: the same hue again at full strength, not red.
        send_mouse(square, QEvent::MouseMove, QPointF(field.right(), field.top() - 60));
        send_mouse(square, QEvent::MouseButtonRelease, QPointF(field.right(), field.top() - 60));
        QCOMPARE(square->value(), 1.0);
        QVERIFY(close_to(dialog.currentColor().hsvHueF(), hue));
        QCOMPARE(square->handle_center(), field.topRight());
        // A grey typed in keeps the hue as well.
        dialog.value_edit->selectAll();
        QTest::keyClicks(dialog.value_edit, "#808080");
        QCOMPARE(dialog.currentColor(), QColor("#808080"));
        QVERIFY(close_to(square->hue(), hue));
        QVERIFY(close_to(dialog.hue_slider->value(), hue));
        QCOMPARE(square->saturation(), 0.0);
        // Arrow keys nudge the handle; Shift by ten steps.
        QTest::keyClick(square, Qt::Key_Right, Qt::ShiftModifier);
        QVERIFY(close_to(square->saturation(), 0.1));
        QTest::keyClick(square, Qt::Key_Down);
        QTest::keyClick(square, Qt::Key_Left);
        QTest::keyClick(square, Qt::Key_Up);
        QVERIFY(close_to(square->saturation(), 0.09));
        QVERIFY(close_to(square->hue(), hue));
        // The hue track: a click jumps, the right end is red and stays at the right end.
        auto *track = dialog.hue_slider;
        send_mouse(track, QEvent::MouseButtonPress, QPointF(track->width() + 40, track->height() / 2.0));
        send_mouse(track, QEvent::MouseButtonRelease, QPointF(track->width() + 40, track->height() / 2.0));
        QCOMPARE(track->value(), 1.0);
        QCOMPARE(track->thumb_x(), track->track_rect().right());
        dialog.setCurrentColor(QColor(255, 0, 0));
        QCOMPARE(track->value(), 1.0);
        QTest::keyClick(track, Qt::Key_Home);
        QCOMPARE(track->value(), 0.0);
        QTest::keyClick(track, Qt::Key_Right);
        QVERIFY(close_to(track->value(), 0.01));
        QTest::keyClick(track, Qt::Key_End);
        QTest::keyClick(track, Qt::Key_Left, Qt::ShiftModifier);
        QVERIFY(close_to(track->value(), 0.9));
        QVERIFY(!dialog.grab().isNull());
    }

    void colorTextParsesAndFormatsEachFormat() {
        using D = ColorPickerDialog;
        const QColor blue("#3366CC");
        QCOMPARE(D::parse_color(D::HEX, "#3366cc")->color, blue);
        QCOMPARE(D::parse_color(D::HEX, " 36c ")->color, blue);
        const auto translucent = D::parse_color(D::HEX, "3366CC80");
        QVERIFY(translucent && translucent->has_alpha);
        QCOMPARE(translucent->color.alpha(), 128);
        QVERIFY(!D::parse_color(D::HEX, "#3366C").has_value());
        QVERIFY(!D::parse_color(D::HEX, "#GG0000").has_value());
        QCOMPARE(D::parse_color(D::RGB, "51, 102, 204")->color, blue);
        QCOMPARE(D::parse_color(D::RGB, "rgb(51 102 204)")->color, blue);
        QVERIFY(!D::parse_color(D::RGB, "256, 0, 0").has_value());
        QVERIFY(!D::parse_color(D::RGB, "51, 102").has_value());
        QVERIFY(!D::parse_color(D::RGB, "51, -1, 204").has_value());
        // hsl(220, 60%, 50%): chroma 0.6, so channels 0.2 / 0.4 / 0.8.
        QCOMPARE(D::parse_color(D::HSL, "220, 60%, 50%")->color, blue);
        QCOMPARE(D::parse_color(D::HSL, "hsl(220deg 60% 50%)")->color, blue);
        QCOMPARE(D::parse_color(D::HSL, "360, 100%, 50%")->color, QColor(255, 0, 0));
        QVERIFY(!D::parse_color(D::HSL, "220, 120%, 50%").has_value());
        QCOMPARE(D::format_color(D::HEX, blue), QString("#3366CC"));
        QCOMPARE(D::format_color(D::RGB, blue), QString("51, 102, 204"));
        QCOMPARE(D::format_color(D::HSL, blue), QString("220, 60%, 50%"));
        // A grey has no hue of its own and shows the one it is given.
        QCOMPARE(D::format_color(D::HSL, QColor(128, 128, 128), 0.5), QString("180, 0%, 50%"));
    }

    void colorDialogFieldsFollowTheFormatAndRejectBadText() {
        ColorPickerDialog dialog(QColor("#3366CC"));
        dialog.show();
        QSignalSpy changes(&dialog, &ColorPickerDialog::currentColorChanged);
        dialog.format_combo->setCurrentIndex(dialog.format_combo->findData(ColorPickerDialog::RGB));
        QCOMPARE(dialog.value_edit->text(), QString("51, 102, 204"));
        dialog.value_edit->selectAll();
        QTest::keyClicks(dialog.value_edit, "255, 0, 0");
        QCOMPARE(dialog.currentColor(), QColor(255, 0, 0));
        QCOMPARE(changes.size(), 1);
        // Unreadable text changes nothing, and Enter puts the colour back instead of confirming.
        dialog.value_edit->selectAll();
        QTest::keyClicks(dialog.value_edit, "300, 0, 0");
        QCOMPARE(dialog.currentColor(), QColor(255, 0, 0));
        QTest::keyClick(dialog.value_edit, Qt::Key_Return);
        QVERIFY(dialog.isVisible());
        QCOMPARE(dialog.value_edit->text(), QString("255, 0, 0"));
        dialog.format_combo->setCurrentIndex(dialog.format_combo->findData(ColorPickerDialog::HSL));
        QCOMPARE(dialog.value_edit->text(), QString("0, 100%, 50%"));
        // The next dialog opens in the format last chosen.
        ColorPickerDialog next(QColor("#3366CC"));
        QCOMPARE(next.value_edit->text(), QString("220, 60%, 50%"));
        next.format_combo->setCurrentIndex(next.format_combo->findData(ColorPickerDialog::HEX));
        // Readable text and Enter confirm the dialog with that colour.
        dialog.format_combo->setCurrentIndex(dialog.format_combo->findData(ColorPickerDialog::HEX));
        dialog.value_edit->selectAll();
        QTest::keyClicks(dialog.value_edit, "#00aa00");
        QTest::keyClick(dialog.value_edit, Qt::Key_Return);
        QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
        QCOMPARE(dialog.selectedColor(), QColor("#00AA00"));
    }

    void colorDialogOpacityFromTrackFieldAndHex() {
        ColorPickerDialog dialog(QColor(51, 102, 204));
        dialog.show();
        QCOMPARE(dialog.alpha_edit->text(), QString("100%"));
        dialog.alpha_edit->selectAll();
        QTest::keyClicks(dialog.alpha_edit, "50");
        QCOMPARE(dialog.currentColor().alpha(), 128);
        QVERIFY(close_to(dialog.alpha_slider->value(), 128 / 255.0));
        // Each readable step applies; past 100 the last one stays and Enter shows it again.
        dialog.alpha_edit->selectAll();
        QTest::keyClicks(dialog.alpha_edit, "150");
        QCOMPARE(dialog.currentColor().alpha(), 38);
        QTest::keyClick(dialog.alpha_edit, Qt::Key_Return);
        QCOMPARE(dialog.alpha_edit->text(), QString("15%"));
        QVERIFY(dialog.isVisible());
        // Dragged off the left end: transparent, the colour itself unchanged.
        auto *track = dialog.alpha_slider;
        send_mouse(track, QEvent::MouseButtonPress, QPointF(track->width() / 2.0, track->height() / 2.0));
        send_mouse(track, QEvent::MouseMove, QPointF(-50, 0));
        send_mouse(track, QEvent::MouseButtonRelease, QPointF(-50, 0));
        QCOMPARE(dialog.currentColor(), QColor(51, 102, 204, 0));
        QCOMPARE(dialog.alpha_edit->text(), QString("0%"));
        // Eight hex digits carry the opacity; six keep it.
        dialog.value_edit->selectAll();
        QTest::keyClicks(dialog.value_edit, "#FF000040");
        QCOMPARE(dialog.currentColor(), QColor(255, 0, 0, 64));
        dialog.value_edit->selectAll();
        QTest::keyClicks(dialog.value_edit, "#00FF00");
        QCOMPARE(dialog.currentColor(), QColor(0, 255, 0, 64));
        QVERIFY(!dialog.grab().isNull());

        // Without the alpha channel the colour is opaque and the opacity controls are gone.
        ColorPickerDialog opaque(QColor(255, 0, 0, 100), ColorPickerDialog::NoOption);
        QVERIFY(opaque.options() == ColorPickerDialog::NoOption);
        QCOMPARE(opaque.currentColor(), QColor(255, 0, 0));
        QVERIFY(opaque.alpha_slider->isHidden());
        QVERIFY(opaque.alpha_edit->isHidden());
        opaque.value_edit->selectAll();
        QTest::keyClicks(opaque.value_edit, "#0000FF80");
        QCOMPARE(opaque.currentColor(), QColor(0, 0, 255));
    }

    // A window: it follows the window scale, not the toolbar and panel scale.
    void colorDialogFollowsTheWindowScale() {
        const auto restore = qScopeGuard([] {
            get_dialog_scale().set_percent(100);
            get_ui_scale().set_percent(100);
        });
        get_dialog_scale().set_percent(100);
        ColorPickerDialog normal(QColor("#3366CC"));
        QCOMPARE(normal.size(), QSize(245, 380));
        QCOMPARE(normal.square->field_rect().size(), QSizeF(229, 199));
        QCOMPARE(normal.format_combo->size(), QSize(61, 30));
        QCOMPARE(normal.value_group->size(), QSize(133, 30));
        QCOMPARE(normal.ok_button->size(), QSize(60, 31));
        QCOMPARE(normal.eyedropper_button->size(), QSize(32, 33));
        // Track ends in line with the eyedropper's right side plus the gap, thumbs whole at both ends.
        QCOMPARE(normal.hue_slider->track_rect().height(), 10.0);
        get_dialog_scale().set_percent(150);
        ColorPickerDialog large(QColor("#3366CC"));
        // Rounded half to even: 367.5 -> 368, 298.5 -> 298, 91.5 -> 92, 46.5 -> 46.
        QCOMPARE(large.width(), 368);
        QCOMPARE(large.square->field_rect().size(), QSizeF(344, 298));
        QCOMPARE(large.format_combo->size(), QSize(92, 45));
        QCOMPARE(large.ok_button->size(), QSize(90, 46));
        QCOMPARE(large.hue_slider->track_rect().height(), 15.0);
        normal.value_edit->ensurePolished();
        large.value_edit->ensurePolished();
        QCOMPARE(normal.value_edit->font().pixelSize(), 14);
        QCOMPARE(large.value_edit->font().pixelSize(), 21);
        get_ui_scale().set_percent(200);
        ColorPickerDialog panels_scaled(QColor("#3366CC"));
        QCOMPARE(panels_scaled.size(), large.size());
    }

    // Every colour control goes through choose_color; the style setting picks the dialog.
    void chooseColorFollowsTheStyleSetting() {
        QVERIFY(!use_system_color_dialog(config_));
        on_next_modal([](QWidget *modal) {
            auto *dialog = qobject_cast<ColorPickerDialog *>(modal);
            QVERIFY(dialog);
            dialog->setCurrentColor(QColor("#00AA00"));
            dialog->accept();
        });
        auto chosen = choose_color(QColor("#123456"));
        QVERIFY(chosen.has_value());
        QCOMPARE(*chosen, QColor("#00AA00"));

        config_->set_app_setting("color_picker_style", "system");
        QVERIFY(use_system_color_dialog(config_));
        QList<QColor> seen;
        const auto record = [&seen](const QColor &color) { seen.append(color); };
        // Without the alpha channel the system dialog reports opaque colours too.
        on_next_modal([](QWidget *modal) {
            auto *dialog = qobject_cast<QColorDialog *>(modal);
            QVERIFY(dialog);
            QVERIFY(!dialog->testOption(QColorDialog::ShowAlphaChannel));
            QCOMPARE(dialog->windowTitle(), QString("Mask"));
            dialog->setCurrentColor(QColor(17, 34, 51, 90));
            dialog->accept();
        });
        chosen = choose_color(QColor("#123456"), nullptr, "Mask", ColorPickerDialog::NoOption, record);
        QVERIFY(chosen.has_value());
        QCOMPARE(*chosen, QColor(17, 34, 51));
        QVERIFY(!seen.isEmpty());
        QCOMPARE(seen.last(), QColor(17, 34, 51));
        // A cancel hands the initial colour back to the live callback.
        seen.clear();
        on_next_modal([](QWidget *modal) {
            auto *dialog = qobject_cast<QColorDialog *>(modal);
            QVERIFY(dialog);
            QVERIFY(dialog->testOption(QColorDialog::ShowAlphaChannel));
            QCOMPARE(dialog->windowTitle(), i18n_tr("Select Color", "ColorPickerDialog"));
            dialog->setCurrentColor(QColor("#FF0000"));
            dialog->reject();
        });
        QVERIFY(!choose_color(QColor("#123456"), nullptr, {}, ColorPickerDialog::ShowAlphaChannel, record).has_value());
        QVERIFY(seen.contains(QColor("#FF0000")));
        QCOMPARE(seen.last(), QColor("#123456"));
        // The panels' colour button opens the same choice.
        ColorPickerButton button(QColor("red"), true);
        on_next_modal([](QWidget *modal) {
            auto *dialog = qobject_cast<QColorDialog *>(modal);
            QVERIFY(dialog);
            dialog->setCurrentColor(QColor("#00AA00"));
            dialog->accept();
        });
        button.click();
        QCOMPARE(button.color(), QColor("#00AA00"));
    }

    // getColor: live changes reach the callback, a cancel hands back the initial colour.
    void getColorFollowsLiveChangesAndRevertsOnCancel() {
        QList<QColor> seen;
        const auto record = [&seen](const QColor &color) { seen.append(color); };
        on_next_modal([](QWidget *modal) {
            auto *dialog = qobject_cast<ColorPickerDialog *>(modal);
            QVERIFY(dialog);
            QCOMPARE(dialog->windowTitle(), QString("Border"));
            dialog->setCurrentColor(QColor("#00AA00"));
            dialog->reject();
        });
        QVERIFY(!ColorPickerDialog::getColor(QColor("#123456"), nullptr, "Border", ColorPickerDialog::ShowAlphaChannel, record).has_value());
        QCOMPARE(seen, (QList<QColor>{QColor("#00AA00"), QColor("#123456")}));
        seen.clear();
        on_next_modal([](QWidget *modal) {
            auto *dialog = qobject_cast<ColorPickerDialog *>(modal);
            QVERIFY(dialog);
            QCOMPARE(dialog->windowTitle(), i18n_tr("Select Color", "ColorPickerDialog"));
            dialog->setCurrentColor(QColor("#00AA00"));
            dialog->accept();
        });
        const auto chosen = ColorPickerDialog::getColor(QColor("#123456"), nullptr, {}, ColorPickerDialog::ShowAlphaChannel, record);
        QVERIFY(chosen.has_value());
        QCOMPARE(*chosen, QColor("#00AA00"));
        QCOMPARE(seen, (QList<QColor>{QColor("#00AA00")}));
    }

    // The colour button opens the dialog below itself and reports the accepted colour.
    void colorButtonPicksWithTheDialog() {
        // Near the top of the 800x600 offscreen screen, so the dialog fits below the button.
        QWidget host;
        host.setGeometry(100, 20, 200, 100);
        auto *button = new ColorPickerButton(QColor("red"), false, 28, true, true, &host);
        button->move(10, 10);
        host.show();
        QSignalSpy picked(button, &ColorPickerButton::color_changed);
        QPoint dialog_pos;
        on_next_modal([&dialog_pos](QWidget *modal) {
            auto *dialog = qobject_cast<ColorPickerDialog *>(modal);
            QVERIFY(dialog);
            QVERIFY(dialog->alpha_edit->isHidden());
            dialog_pos = dialog->pos();
            dialog->setCurrentColor(QColor("#00AA00"));
            dialog->accept();
        });
        button->click();
        QCOMPARE(picked.size(), 1);
        QCOMPARE(button->color(), QColor("#00AA00"));
        QCOMPARE(dialog_pos.x(), button->mapToGlobal(QPoint(0, 0)).x());
        QVERIFY(dialog_pos.y() > button->mapToGlobal(QPoint(0, 0)).y());
    }

    // The eyedropper over a frozen capture: hovering previews, leaving restores, a click
    // picks with the dialog's opacity, Esc and closing the dialog cancel.
    void eyedropperPreviewsPicksAndCancels() {
        QImage capture(100, 50, QImage::Format_RGB32);
        capture.fill(QColor(255, 0, 0));
        for (int y = 0; y < 50; ++y)
            for (int x = 50; x < 100; ++x) capture.setPixelColor(x, y, QColor(0, 0, 255));
        ScreenColorPicker::set_capture_for_test([capture] { return std::make_pair(capture, QRectF(0, 0, 100, 50)); });
        const QColor start(0, 255, 0, 128);
        ColorPickerDialog dialog(start);
        dialog.show();
        dialog.eyedropper_button->click();
        QVERIFY(dialog.picking_screen());
        QVERIFY(dialog.eyedropper_button->isChecked());
        QPointer<ScreenColorPicker> picker = dialog.screen_picker();
        QVERIFY(picker && picker->isVisible());
        QCOMPARE(picker->geometry(), QRect(0, 0, 100, 50));
        QCOMPARE(picker->color_at(QPoint(10, 10)), QColor(255, 0, 0));
        QVERIFY(!picker->color_at(QPoint(100, 10)).isValid());
        const auto at = [&picker](int x, int y) { return picker->mapFromGlobal(QPointF(x, y)); };
        send_mouse(picker, QEvent::MouseMove, at(10, 10));
        QCOMPARE(dialog.currentColor(), QColor(255, 0, 0, 128));
        QVERIFY(picker->magnifier->isVisible());
        QWheelEvent wheel(at(10, 10), QPointF(10, 10), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        const double zoom = picker->magnifier->get_zoom_factor();
        QApplication::sendEvent(picker, &wheel);
        QVERIFY(picker->magnifier->get_zoom_factor() > zoom);
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(picker, &leave);
        QCOMPARE(dialog.currentColor(), start);
        send_mouse(picker, QEvent::MouseMove, at(80, 10));
        send_mouse(picker, QEvent::MouseButtonPress, at(80, 10));
        QCOMPARE(dialog.currentColor(), QColor(0, 0, 255, 128));
        QVERIFY(!dialog.picking_screen());
        QVERIFY(!dialog.eyedropper_button->isChecked());
        QTRY_VERIFY(picker.isNull());

        // Esc on the eyedropper restores the colour, hue included. Arrows move the pointer
        // (wherever the offscreen pointer is) and keep the eyedropper running.
        dialog.eyedropper_button->click();
        picker = dialog.screen_picker();
        QVERIFY(picker);
        send_mouse(picker, QEvent::MouseMove, at(10, 10));
        QCOMPARE(dialog.currentColor(), QColor(255, 0, 0, 128));
        QTest::keyClick(picker, Qt::Key_Right);
        QVERIFY(dialog.picking_screen());
        QTest::keyClick(picker, Qt::Key_Escape);
        QCOMPARE(dialog.currentColor(), QColor(0, 0, 255, 128));
        QVERIFY(close_to(dialog.square->hue(), 240.0 / 360));
        QTRY_VERIFY(picker.isNull());

        // Unchecking the button, a right click and an edit in the dialog end it as well.
        dialog.eyedropper_button->click();
        dialog.eyedropper_button->click();
        QVERIFY(!dialog.picking_screen());
        dialog.eyedropper_button->click();
        picker = dialog.screen_picker();
        send_mouse(picker, QEvent::MouseButtonPress, at(10, 10), Qt::RightButton);
        QVERIFY(!dialog.picking_screen());
        QCOMPARE(dialog.currentColor(), QColor(0, 0, 255, 128));
        // The pointer leaves the eyedropper for the dialog first, which drops the preview.
        dialog.eyedropper_button->click();
        QApplication::sendEvent(dialog.screen_picker(), &leave);
        dialog.alpha_edit->selectAll();
        QTest::keyClicks(dialog.alpha_edit, "100");
        QVERIFY(!dialog.picking_screen());
        QCOMPARE(dialog.currentColor(), QColor(0, 0, 255));
        // Closing the dialog closes the eyedropper.
        dialog.eyedropper_button->click();
        picker = dialog.screen_picker();
        dialog.reject();
        QTRY_VERIFY(picker.isNull());

        // A failed capture leaves the button unchecked.
        ScreenColorPicker::set_capture_for_test([]() -> std::pair<QImage, QRectF> { throw std::runtime_error("no capture"); });
        ColorPickerDialog failing(start);
        failing.show();
        failing.eyedropper_button->click();
        QVERIFY(!failing.picking_screen());
        QVERIFY(!failing.eyedropper_button->isChecked());
        QCOMPARE(failing.currentColor(), start);
        ScreenColorPicker::set_capture_for_test({});
    }
};

QTEST_MAIN(UiWidgetTests)
#include "test_ui_widgets.moc"
