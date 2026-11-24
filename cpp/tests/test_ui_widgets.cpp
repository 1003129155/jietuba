#include "annotation/annotation_toolbar.h"
#include "canvas/items/watermark_item.h"
#include "core/qt_utils.h"
#include "core/i18n.h"
#include "core/shortcut_manager.h"
#include "core/ui_scale.h"
#include "core/updater_process.h"
#include "pin/pin_toolbar.h"
#include "settings/color_formats.h"
#include "settings/tool_settings.h"
#include "stitch/scroll_toolbar.h"
#include "ui/base_settings_panel.h"
#include "ui/busy_ring.h"
#include "ui/dialogs.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/cards.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/labels.h"
#include "ui/fluent_lite/switch.h"
#include "ui/hotkey_edit.h"
#include "ui/mosaic_settings_panel.h"
#include "ui/arrow_settings_panel.h"
#include "ui/paint_settings_panel.h"
#include "ui/shape_settings_panel.h"
#include "ui/color_picker_button.h"
#include "ui/color_picker_dialog.h"
#include "caption_check.h"
#include "ui/magnifier.h"
#include "ui/number_settings_panel.h"
#include "ui/text_settings_panel.h"
#include "canvas/items/text_item.h"
#include "ui/inapp_key_edit.h"
#include "ui/key_chip.h"
#include "ui/reorderable_rows.h"
#include "ui/screenshot_countdown.h"
#include "ui/settings_ui/color_format_dialog.h"
#include "ui/settings_ui/components.h"
#include "ui/settings_ui/dialog.h"
#include "ui/toolbar.h"
#include "ui/toolbar_layout.h"
#include "ui/toolbar_layout_dialog.h"
#include "ui/wait_bar.h"
#include "ui/watermark_input_dialog.h"
#include "ui/translation_panel.h"
#include "translation/languages.h"
#include "ui/segmented_toggle.h"
#include "ui/watermark_settings_panel.h"
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QFrame>
#include <QFontDatabase>
#include <QDialogButtonBox>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QToolButton>
#include <QAbstractItemView>
#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>
#include <stdexcept>
#include <memory>
#include <QMouseEvent>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QSlider>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QHash>
#include <QSet>
#include <QXmlStreamReader>
#include <QTest>
#include <QTimer>
#include <QScrollArea>
#include <QStackedWidget>
#include <QToolButton>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

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

    // Over a window: shows after its delay unless dismissed first, tells done of total or runs back
    // and forth, and lets clicks through.
    void aWaitBarWaitsBeforeShowingAndTellsProgress() {
        QWidget host;
        host.resize(400, 200);
        host.show();
        WaitBar bar(&host, QSize(200, 10));
        QVERIFY(bar.testAttribute(Qt::WA_TransparentForMouseEvents));
        bar.set_progress(0, 0);
        QCOMPARE(bar.maximum(), 0);
        bar.set_progress(3, 4);
        QCOMPARE(bar.maximum(), 100);
        QCOMPARE(bar.value(), 75);
        bar.set_progress(9, 4);
        QCOMPARE(bar.value(), 100);
        bar.center_on(QPoint(200, 100));
        QCOMPARE(bar.geometry(), QRect(100, 95, 200, 10));
        bar.show_after(50);
        QVERIFY(bar.show_pending());
        bar.dismiss();
        QTest::qWait(100);
        QVERIFY(!bar.isVisible());
        bar.show_after(30);
        QVERIFY(!bar.isVisible());
        QTRY_VERIFY(bar.isVisible());
        bar.dismiss();
        QVERIFY(!bar.isVisible());
    }

    void aWaitBarWithoutAParentIsATopmostWindow() {
        WaitBar bar;
        QCOMPARE(bar.size(), QSize(260, 14));
        QVERIFY(bar.isWindow());
        QVERIFY(bar.windowFlags() & Qt::WindowStaysOnTopHint);
        QVERIFY(bar.testAttribute(Qt::WA_TranslucentBackground));
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
        // Enter answers with the button drawn as the primary one.
        on_next_modal([](QWidget *modal) { QTest::keyClick(modal, Qt::Key_Return); });
        QVERIFY(show_confirm_dialog(&parent, "Delete", "Delete this item?"));

        const std::vector<DialogButton> buttons = {{"save", "Save", QDialogButtonBox::AcceptRole}, {"cancel", "Cancel", QDialogButtonBox::RejectRole}};
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

    // A ring turns around the translate button while the translation is worked on, once the
    // wait outlasts a short delay; it follows the toolbar's scale and lets clicks through.
    void theTranslateButtonTurnsARingWhileTheTranslationRuns() {
        Toolbar toolbar;
        toolbar.show();
        auto *button = toolbar.screenshot_translate_btn;
        toolbar.set_translation_busy(true);
        toolbar.set_translation_busy(false);
        auto *ring = toolbar.translation_busy_ring;
        QVERIFY(ring);
        QTest::qWait(BusyRing::SHOW_DELAY_MS + 100);
        QVERIFY(ring->isHidden());

        toolbar.set_translation_busy(true);
        QVERIFY(ring->isHidden());
        QTRY_VERIFY(!ring->isHidden());
        QCOMPARE(ring->parentWidget(), button);
        QCOMPARE(ring->geometry(), button->rect());
        QVERIFY(ring->testAttribute(Qt::WA_TransparentForMouseEvents));
        const QRectF at_100 = ring->circle();
        QVERIFY(QRectF(button->rect()).contains(at_100));
        QVERIFY(at_100.width() > button->iconSize().width());

        get_ui_scale().set_percent(150);
        const auto restore = qScopeGuard([] { get_ui_scale().set_percent(100); });
        QTRY_COMPARE(ring->geometry(), button->rect());
        QVERIFY2(ring->circle().width() >= at_100.width() * 1.4, qPrintable(QString::number(ring->circle().width())));
        QVERIFY(QRectF(button->rect()).contains(ring->circle()));

        toolbar.set_translation_busy(false);
        QVERIFY(ring->isHidden());
    }

    // Once the number of paragraphs is known, the ring fills clockwise from the top as they come
    // in; the next run turns it again until its own number is known.
    void theTranslateRingFillsAsParagraphsComeIn() {
        Toolbar toolbar;
        toolbar.show();
        toolbar.set_translation_busy(true);
        auto *ring = toolbar.translation_busy_ring;
        QTRY_VERIFY(ring && !ring->isHidden());
        QVERIFY(!ring->progress());
        // The most opaque pixel near the point a share of a turn clockwise from the top.
        const auto ink_at = [ring](double turn) {
            QImage image(ring->size(), QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            ring->render(&image);
            const QRectF circle = ring->circle();
            const double angle = turn * 2 * std::numbers::pi, radius = circle.width() / 2;
            const QPoint at(qRound(circle.center().x() + radius * std::sin(angle)), qRound(circle.center().y() - radius * std::cos(angle)));
            int most = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (image.rect().contains(at + QPoint(dx, dy))) most = std::max(most, qAlpha(image.pixel(at + QPoint(dx, dy))));
            return most;
        };
        toolbar.set_translation_progress(1, 4);
        QCOMPARE(ring->progress(), std::optional<double>(0.25));
        QTRY_VERIFY2(ink_at(0.15) > 200, qPrintable(QString::number(ink_at(0.15))));
        QVERIFY2(ink_at(0.6) < 120, qPrintable(QString::number(ink_at(0.6))));
        toolbar.set_translation_progress(4, 4);
        QTRY_VERIFY(ink_at(0.6) > 200);

        toolbar.set_translation_progress(0, 0);
        QVERIFY(!ring->progress());
        toolbar.set_translation_progress(3, 4);
        toolbar.set_translation_busy(false);
        QVERIFY(ring->isHidden());
        QVERIFY(!ring->progress());
    }

    void captureAndAnnotationTooltipsUseShortLabels() {
        XmlTranslator translator;
        QVERIFY(translator.load_from_xml(":/translations/app_zh.xml"));
        QVERIFY(QCoreApplication::installTranslator(&translator));
        const auto uninstall = qScopeGuard([&translator] { QCoreApplication::removeTranslator(&translator); });
        Toolbar toolbar;
        PinToolbar pin_toolbar(nullptr, config_);
        FloatingToolbar scroll_toolbar;
        AnnotationToolbar annotation_toolbar(nullptr);

        const auto recognize_text = QStringLiteral("文字识别");
        QCOMPARE(toolbar.text_recognize_btn->toolTip(), recognize_text);
        QCOMPARE(pin_toolbar.text_recognize_btn->toolTip(), recognize_text);
        QCOMPARE(scroll_toolbar.ocr_btn->toolTip(), recognize_text);
        QCOMPARE(toolbar.long_screenshot_btn->toolTip(), QStringLiteral("长截图"));
        QCOMPARE(toolbar.quick_save_btn->toolTip(), QStringLiteral("快捷保存（不弹窗）"));
        QCOMPARE(toolbar.screenshot_translate_btn->toolTip(), QStringLiteral("截图翻译"));
        QCOMPARE(toolbar.scan_code_btn->toolTip(), QStringLiteral("扫码"));
        QCOMPARE(toolbar.gif_btn->toolTip(), QStringLiteral("录屏"));
        QCOMPARE(annotation_toolbar.board_btn->toolTip(), QStringLiteral("切换白板"));
        QCOMPARE(annotation_toolbar.highlight_btn->toolTip(), QStringLiteral("高亮光标和点击位置"));
    }

    void theToolbarFoldsHidesAndRereadsItsLayout() {
        Toolbar toolbar;
        toolbar.show();
        QCOMPARE(toolbar.confirm_btn->toolTip(), QStringLiteral("Confirm"));
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
        QCOMPARE(toolbar.screenshot_translate_btn->parentWidget(), popup);
    }

    void pinAndScreenshotLayoutsAreSeparate() {
        tl::save_layout(*config_, layout_with({}, {{"pen", tl::HIDE}, {"save", tl::MORE}}));
        tl::save_layout(*config_, layout_with("text", {{"mosaic", tl::HIDE}}, tl::PIN), tl::PIN);
        PinToolbar pin_toolbar(nullptr, config_);
        const auto row = toolbar_row(pin_toolbar);
        QCOMPARE(row.first(), QString("text"));
        QCOMPARE(row.last(), QString("more"));
        QVERIFY(!row.contains("mosaic"));
        for (const char *key : {"pen", "save", "spotlight", "watermark", "screenshot_translate"}) QVERIFY2(row.contains(key), key);
        for (const char *key : {"long_screenshot", "confirm"}) QVERIFY2(!row.contains(key), key);
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

    // While a translation is shown its panel sits where a tool's panel would, and gives the
    // place up to one: the translate button brings it back.
    void theTranslationPanelSharesTheToolPanelsPlace() {
        Toolbar toolbar;
        toolbar.show();
        QVERIFY(!toolbar.translation_panel);
        toolbar.set_translation_shown(true);
        QVERIFY(toolbar.translation_panel && toolbar.translation_panel->isVisible());
        QVERIFY(toolbar.screenshot_translate_btn->isChecked());
        toolbar.select_tool("pen");
        QVERIFY(!toolbar.translation_panel->isVisible());
        QVERIFY(toolbar.paint_panel->isVisible());
        toolbar.select_tool("cursor");
        QVERIFY(toolbar.translation_panel->isVisible());
        // The button closes and reopens the panel; it stays lit, since the translation is still there.
        toolbar.toggle_translation_panel();
        QVERIFY(!toolbar.translation_panel->isVisible());
        QVERIFY(toolbar.screenshot_translate_btn->isChecked());
        toolbar.select_tool("pen");
        toolbar.select_tool("cursor");
        QVERIFY(!toolbar.translation_panel->isVisible());
        toolbar.toggle_translation_panel();
        QVERIFY(toolbar.translation_panel->isVisible());
        toolbar.select_tool("pen");
        toolbar.toggle_translation_panel();
        QVERIFY(toolbar.translation_panel->isVisible());
        QVERIFY(!toolbar.current_tool);
        toolbar.set_translation_shown(false);
        QVERIFY(!toolbar.translation_panel->isVisible());
        QVERIFY(!toolbar.screenshot_translate_btn->isCheckable());
        toolbar.toggle_translation_panel();
        QVERIFY(!toolbar.translation_panel->isVisible());
        toolbar.hide();
    }

    // The capture hides the toolbar while the selection is dragged: the translation panel goes
    // with it and comes back under it, unless the translate button had closed it.
    void theTranslationPanelHidesAndShowsWithTheToolbar() {
        Toolbar toolbar;
        toolbar.show();
        toolbar.set_translation_shown(true);
        auto *panel = toolbar.translation_panel;
        QVERIFY(panel->isVisible());
        toolbar.hide();
        QVERIFY(!panel->isVisible());
        toolbar.move(toolbar.pos() + QPoint(40, 30));
        toolbar.show();
        QTRY_VERIFY(panel->isVisible());
        QCOMPARE(panel->mapToGlobal(QPoint(0, 0)).x(), toolbar.mapToGlobal(QPoint(0, 0)).x());
        toolbar.toggle_translation_panel();
        QVERIFY(!panel->isVisible());
        toolbar.hide();
        toolbar.show();
        QTest::qWait(30);
        QVERIFY(!panel->isVisible());
        toolbar.hide();
    }

    // The arrow slides the service out and folds it back; the next panel opens the same way.
    void theTranslationPanelSlidesTheServiceOutAndRemembersIt() {
        config_->set_translation_panel_expanded(false);
        TranslationPanel panel;
        panel.set_providers({{"deepl", "DeepL API"}, {"baidu", "Baidu"}}, "baidu");
        panel.show();
        QVERIFY(!panel.expanded());
        QVERIFY(!panel.service_combo->isVisible());
        const int folded = panel.width();
        QCOMPARE(panel.expand_btn->geometry().right(), folded - 1 - scaled(TranslationPanel::BASE_MARGIN_H));

        QTest::mouseClick(panel.expand_btn, Qt::LeftButton);
        QVERIFY(panel.expanded());
        QVERIFY(config_->get_translation_panel_expanded());
        bool between = false;
        for (int i = 0; i < 60 && panel.sliding(); ++i) {
            between = between || (panel.width() > folded && panel.width() < folded + panel.service_combo->width());
            QTest::qWait(5);
        }
        QVERIFY(between);
        QTRY_VERIFY(!panel.sliding());
        QVERIFY(panel.service_combo->isVisible());
        // The service sits whole between the language and the arrow.
        const QRect service(panel.service_combo->mapTo(&panel, QPoint(0, 0)), panel.service_combo->size());
        QVERIFY(service.left() > panel.language_combo->geometry().right());
        QVERIFY(service.right() < panel.expand_btn->geometry().left());
        const int unfolded = panel.width();
        QCOMPARE(unfolded, folded + panel.service_combo->width() + 2 * scaled(TranslationPanel::BASE_SPACING) + 1);

        // Opened unfolded, without sliding.
        {
            TranslationPanel next;
            next.set_providers({{"deepl", "DeepL API"}, {"baidu", "Baidu"}}, "baidu");
            next.show();
            QVERIFY(next.expanded() && !next.sliding());
            QCOMPARE(next.width(), unfolded);
        }

        QTest::mouseClick(panel.expand_btn, Qt::LeftButton);
        QVERIFY(!config_->get_translation_panel_expanded());
        QTRY_VERIFY(!panel.sliding());
        QCOMPARE(panel.width(), folded);
        QVERIFY(!panel.service_combo->isVisible());
        TranslationPanel next;
        QVERIFY(!next.expanded());
    }

    void theTranslationPanelReportsItsChoicesAndFollowsThePanelScale() {
        const auto restore = qScopeGuard([] {
            get_dialog_scale().set_percent(100);
            get_ui_scale().set_percent(100);
        });
        get_ui_scale().set_percent(100);
        TranslationPanel panel;
        QSignalSpy original(&panel, &TranslationPanel::original_shown_changed);
        QSignalSpy language(&panel, &TranslationPanel::target_language_changed);
        QSignalSpy provider(&panel, &TranslationPanel::provider_changed);
        // What the host sets is not reported back to it.
        panel.set_providers({{"deepl", "DeepL API"}, {"baidu", "Baidu"}}, "baidu");
        panel.set_target_language("ja");
        panel.set_original_shown(true);
        QCOMPARE(panel.provider(), QString("baidu"));
        QCOMPARE(panel.target_language(), QString("JA"));
        QVERIFY(panel.original_shown());
        QCOMPARE(panel.view_toggle->current(), 1);
        QCOMPARE(original.count() + language.count() + provider.count(), 0);
        // The translation segment clicked: the translation shows again.
        panel.show();
        QTest::mouseClick(panel.view_toggle, Qt::LeftButton, {}, panel.view_toggle->segment_rect(0).center());
        QCOMPARE(original.count(), 1);
        QCOMPARE(original.last().first().toBool(), false);
        // Clicking the chosen segment again changes nothing.
        QTest::mouseClick(panel.view_toggle, Qt::LeftButton, {}, panel.view_toggle->segment_rect(0).center());
        QCOMPARE(original.count(), 1);
        panel.language_combo->setCurrentIndex(panel.language_combo->findData("KO"));
        QCOMPARE(language.last().first().toString(), QString("KO"));
        // The service list behind the arrow holds the services with the current one chosen.
        QCOMPARE(panel.service_combo->count(), 2);
        QCOMPARE(panel.service_combo->currentText(), QString("Baidu"));
        panel.service_combo->setCurrentIndex(0);
        QCOMPARE(provider.count(), 1);
        QCOMPARE(provider.last().first().toString(), QString("deepl"));
        QCOMPARE(panel.provider(), QString("deepl"));
        QVERIFY(!panel.view_toggle->toolTip().isEmpty());
        QVERIFY(!panel.service_combo->toolTip().isEmpty());
        const QString expand_tip = panel.expand_btn->toolTip();
        QVERIFY(!expand_tip.isEmpty());
        panel.set_expanded(true);
        QVERIFY(!panel.expand_btn->toolTip().isEmpty() && panel.expand_btn->toolTip() != expand_tip);
        panel.set_expanded(false);
        QTRY_VERIFY(!panel.sliding());
        // A long custom model name is cut rather than widening the panel without end.
        panel.set_providers({{"custom_llm", QString(80, 'W')}}, "custom_llm");
        QCOMPARE(panel.service_combo->width(), scaled(TranslationPanel::BASE_SERVICE_MAX_WIDTH));
        QCOMPARE(panel.provider(), QString("custom_llm"));
        QCOMPARE(provider.count(), 1);
        panel.set_providers({{"deepl", "DeepL API"}, {"baidu", "Baidu"}}, "baidu");

        // The language list holds the longest name whole, whatever is chosen.
        int longest = 0;
        for (const auto &[code, name] : translation_languages()) longest = std::max(longest, panel.language_combo->fontMetrics().horizontalAdvance(name));
        QVERIFY(panel.language_combo->width() - scaled(PillComboBox::BASE_PADDING_LEFT) - scaled(PillComboBox::BASE_ARROW_COLUMN) >= longest);

        const QSize normal = panel.sizeHint();
        const int normal_language = panel.language_combo->width();
        // The window scale leaves a panel alone; the panel scale resizes it.
        get_dialog_scale().set_percent(150);
        panel.apply_scale();
        QCOMPARE(panel.sizeHint(), normal);
        get_ui_scale().set_percent(150);
        panel.apply_scale();
        QCOMPARE(panel.view_toggle->height(), SegmentedToggle::BASE_HEIGHT * 3 / 2);
        QCOMPARE(panel.expand_btn->width(), TranslationPanel::BASE_BTN * 3 / 2);
        QVERIFY(panel.language_combo->width() > normal_language * 13 / 10);
        QVERIFY(panel.sizeHint().width() > normal.width());
    }

    // Every language's words fit the translation bar at every panel scale, measured with the
    // desktop's fonts: both segments of the toggle, "To", and the longest language name.
    void theTranslationBarFitsEveryLanguage() {
        if (QGuiApplication::platformName() != "windows") QSKIP("needs -platform windows");
        const QString language = I18nManager::get_current_language();
        const auto restore = qScopeGuard([language] {
            get_ui_scale().set_percent(100);
            I18nManager::load_language(language);
        });
        for (const int percent : {100, 150, 200}) {
            get_ui_scale().set_percent(percent);
            for (const char *code : {"zh", "zh_TW", "en", "ja", "ko"}) {
                QVERIFY(I18nManager::load_language(code));
                TranslationPanel panel;
                panel.show();
                QApplication::processEvents();
                panel.layout()->activate();
                const auto where = [&](const QString &what) { return qPrintable(QString("%1 at %2%: %3").arg(code).arg(percent).arg(what)); };
                if (QString(code) != "en") QVERIFY2(panel.view_toggle->labels() != QStringList({"Translated", "Original"}), where("not translated"));
                QFont bold = panel.view_toggle->font();
                bold.setWeight(QFont::DemiBold);
                for (int index = 0; index < panel.view_toggle->labels().size(); ++index) {
                    const auto &label = panel.view_toggle->labels()[index];
                    QVERIFY2(panel.view_toggle->segment_rect(index).width() >= QFontMetrics(bold).horizontalAdvance(label), where(label));
                }
                QVERIFY2(panel.view_toggle->width() >= panel.view_toggle->sizeHint().width(), where("the toggle is squeezed"));
                QVERIFY2(panel.into_label->width() >= panel.into_label->sizeHint().width(), where(panel.into_label->text()));
                int longest = 0;
                for (const auto &[language_code, name] : translation_languages()) longest = std::max(longest, panel.language_combo->fontMetrics().horizontalAdvance(name));
                QVERIFY2(panel.language_combo->width() - scaled(PillComboBox::BASE_PADDING_LEFT) - scaled(PillComboBox::BASE_ARROW_COLUMN) >= longest, where("a language name is cut"));
                for (QWidget *part : std::initializer_list<QWidget *>{panel.view_toggle, panel.into_label, panel.language_combo, panel.expand_btn})
                    QVERIFY2(panel.rect().contains(part->geometry()), where(QString("the bar is %1 wide").arg(panel.width())));
                QVERIFY2(panel.width() <= scaled(420), where(QString("the bar is %1 wide").arg(panel.width())));
                // Unfolded, the service list and the arrow are inside it too.
                panel.set_providers({{"deepl", "DeepL API"}, {"amazon", "Amazon Translate"}}, "deepl");
                panel.set_expanded(true);
                QTRY_VERIFY(!panel.sliding());
                const QRect service(panel.service_combo->mapTo(&panel, QPoint(0, 0)), panel.service_combo->size());
                QVERIFY2(panel.rect().contains(service) && panel.rect().contains(panel.expand_btn->geometry()), where("the unfolded bar"));
                QVERIFY2(service.right() < panel.expand_btn->geometry().left(), where("the service runs under the arrow"));
                panel.set_expanded(false);
            }
        }
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
        // No room on either side: inside the selection, along its bottom edge.
        toolbar.position_near_rect(QRectF(screen));
        QVERIFY(toolbar.geometry().bottom() <= screen.bottom());
        QVERIFY(toolbar.geometry().bottom() >= screen.bottom() - toolbar.height());
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

    // XML catalogs: per-context lookups, a shared fallback, untranslated entries kept as source, and
    // plural messages picking their form by count.
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
  <message numerus="yes"><source>%n file(s)</source><translation>
      <numerusform>%n fichier</numerusform>
      <numerusform>%n fichiers</numerusform>
  </translation></message>
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
        QCOMPARE(translator.translate("Second", "%n file(s)", nullptr, 1), QString("%n fichier"));
        QCOMPARE(translator.translate("Second", "%n file(s)", nullptr, 3), QString("%n fichiers"));
        QCOMPARE(translator.translate("Second", "%n file(s)"), QString("%n fichier"));
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

    void traditionalChineseIsAListedLanguage() {
        const auto items = I18nManager::language_items();
        QCOMPARE(items.size(), size_t(5));
        QCOMPARE(items.back().first, QString("zh_TW"));
        QCOMPARE(items.back().second, QString::fromUtf8("繁體中文"));
        const auto previous = I18nManager::get_current_language();
        QVERIFY(I18nManager::load_language("zh_TW"));
        QCOMPARE(QCoreApplication::translate("SettingsDialog", "Language"), QString::fromUtf8("語言"));
        QCOMPARE(QCoreApplication::translate("SaveImageDialog", "Save Image"), QString::fromUtf8("儲存圖片"));
        I18nManager::load_language(previous);
    }

    // Every embedded catalog loads, and a count reads naturally in each language.
    void everyEmbeddedCatalogLoadsWithItsPlurals() {
        const std::vector<std::tuple<QString, QString, QString>> languages = {
            {"en", "1 line", "3 lines"}, {"zh", QString::fromUtf8("1行"), QString::fromUtf8("3行")}, {"zh_TW", QString::fromUtf8("1行"), QString::fromUtf8("3行")},
            {"ja", QString::fromUtf8("1行"), QString::fromUtf8("3行")}, {"ko", QString::fromUtf8("1줄"), QString::fromUtf8("3줄")}};
        for (const auto &[language, one, three] : languages) {
            XmlTranslator translator;
            QVERIFY2(translator.load_from_xml(QString(":/translations/app_%1.xml").arg(language)), qPrintable(language));
            QCoreApplication::installTranslator(&translator);
            const auto uninstall = qScopeGuard([&translator] { QCoreApplication::removeTranslator(&translator); });
            QCOMPARE(QCoreApplication::translate("ClipboardWindow", "%n line(s)", nullptr, 1), one);
            QCOMPARE(QCoreApplication::translate("ClipboardWindow", "%n line(s)", nullptr, 3), three);
        }
    }

    // Every catalog carries the same sources, so a switch never drops a line back to English.
    void catalogsCoverTheSameSources() {
        const QDir dir(QStringLiteral(JIETUBA_TRANSLATIONS_DIR));
        const auto names = dir.entryList({"app_*.xml"}, QDir::Files, QDir::Name);
        QCOMPARE(names.size(), 5);
        QHash<QString, QSet<QString>> sources;
        for (const auto &name : names) {
            QFile file(dir.filePath(name));
            QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(name));
            QXmlStreamReader xml(&file);
            QSet<QString> set;
            while (!xml.atEnd()) {
                if (xml.readNext() == QXmlStreamReader::StartElement && xml.name() == QLatin1String("source"))
                    set.insert(xml.readElementText());
            }
            QVERIFY2(!xml.hasError(), qPrintable(name));
            sources.insert(name, set);
        }
        const auto reference = sources.value(names.first());
        QVERIFY(reference.size() > 1000);
        for (const auto &name : names) {
            const auto difference = reference - sources.value(name);
            QVERIFY2(difference.isEmpty(),
                     qPrintable(QString("%1 misses: %2").arg(name, QStringList(difference.values()).join(", "))));
        }
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

    // The panels' stylesheet makes every widget transparent; a combo list left to it opens as a
    // see-through window, which Windows shows black.
    void everyPanelsComboListsOpenOnWhite() {
        std::vector<std::unique_ptr<QWidget>> panels;
        panels.push_back(std::make_unique<ArrowSettingsPanel>());
        panels.push_back(std::make_unique<ShapeSettingsPanel>());
        panels.push_back(std::make_unique<PaintSettingsPanel>());
        panels.push_back(std::make_unique<TextSettingsPanel>());
        auto number = std::make_unique<NumberSettingsPanel>();
        number->set_label_section_visible(true);
        panels.push_back(std::move(number));
        panels.push_back(std::make_unique<MosaicSettingsPanel>());
        panels.push_back(std::make_unique<WatermarkSettingsPanel>());
        auto translation = std::make_unique<TranslationPanel>();
        translation->set_providers({{"deepl", "DeepL API"}, {"baidu", "Baidu"}}, "baidu");
        translation->set_expanded(true);
        panels.push_back(std::move(translation));
        int checked = 0;
        for (const auto &panel : panels) {
            panel->show();
            const auto combos = panel->findChildren<QComboBox *>();
            for (int index = 0; index < combos.size(); ++index) {
                auto *combo = combos[index];
                if (combo->count() == 0) continue;
                combo->showPopup();
                QApplication::processEvents();
                const QImage list = combo->view()->grab().toImage();
                // Inside the border, in the padding under the last row.
                const QColor below(list.pixelColor(list.width() / 2, list.height() - 2));
                QVERIFY2(below.alpha() == 255 && below.lightness() > 240,
                         qPrintable(QString("%1 combo %2: %3").arg(panel->metaObject()->className()).arg(index).arg(below.name(QColor::HexArgb))));
                combo->hidePopup();
                ++checked;
            }
        }
        // The number panel's three label combos are among them.
        QVERIFY(checked >= 3);
    }

    void mosaicPanelSwitchesKindsAndDisablesStrengthForSmartErase() {
        MosaicSettingsPanel panel;
        QSignalSpy styles(&panel, &MosaicSettingsPanel::style_changed);
        const auto strength_enabled = [&panel] {
            return std::all_of(panel.block_size_buttons.begin(), panel.block_size_buttons.end(), [](QPushButton *button) { return button->isEnabled(); });
        };
        const auto strength_disabled = [&panel] {
            return std::none_of(panel.block_size_buttons.begin(), panel.block_size_buttons.end(), [](QPushButton *button) { return button->isEnabled(); });
        };
        QVERIFY(panel.kind_buttons[MosaicTool::STYLE_PIXELATE]->isChecked());
        QVERIFY(strength_enabled());
        const int width = panel.sizeHint().width();
        panel.kind_buttons[MosaicTool::STYLE_SMART_ERASE]->click();
        QCOMPARE(styles.size(), 1);
        QCOMPARE(styles.last().first().toString(), MosaicTool::STYLE_SMART_ERASE);
        QVERIFY(strength_disabled());
        // The levels stay in place, so the panel keeps its width.
        QCOMPARE(panel.sizeHint().width(), width);
        // The current kind again is no change.
        panel.kind_buttons[MosaicTool::STYLE_SMART_ERASE]->click();
        QCOMPARE(styles.size(), 1);
        panel.kind_buttons[MosaicTool::STYLE_BLUR]->click();
        QCOMPARE(styles.size(), 2);
        QCOMPARE(panel.style(), MosaicTool::STYLE_BLUR);
        QVERIFY(strength_enabled());
        // Mirroring a selected item does not emit.
        panel.set_style(MosaicTool::STYLE_SMART_ERASE);
        QCOMPARE(styles.size(), 2);
        QVERIFY(panel.kind_buttons[MosaicTool::STYLE_SMART_ERASE]->isChecked());
        QVERIFY(!panel.kind_buttons[MosaicTool::STYLE_BLUR]->isChecked());
        QVERIFY(strength_disabled());
        panel.set_style("unknown");
        QCOMPARE(panel.style(), MosaicTool::STYLE_PIXELATE);
        QVERIFY(panel.kind_buttons[MosaicTool::STYLE_PIXELATE]->isChecked());
        QVERIFY(strength_enabled());
    }

    // The kinds are icon buttons sized like the mode buttons and following the panel scale; each is named
    // by its tooltip, so no language changes the panel's width.
    void mosaicPanelKindsAreIconsNamedByTooltips() {
        const QString language = I18nManager::get_current_language();
        const auto restore = qScopeGuard([language] {
            get_ui_scale().set_percent(100);
            I18nManager::load_language(language);
        });
        const QMap<QString, QStringList> names = {{"zh", {QString::fromUtf8("马赛克"), QString::fromUtf8("模糊"), QString::fromUtf8("智能擦除")}},
                                                  {"en", {"Mosaic", "Blur", "Smart Erase"}}};
        const QStringList kinds = {MosaicTool::STYLE_PIXELATE, MosaicTool::STYLE_BLUR, MosaicTool::STYLE_SMART_ERASE};
        for (const int percent : {100, 150}) {
            get_ui_scale().set_percent(percent);
            int width = -1;
            for (auto it = names.begin(); it != names.end(); ++it) {
                QVERIFY(I18nManager::load_language(it.key()));
                MosaicSettingsPanel panel;
                QCOMPARE(panel.freehand_btn->size(), QSize(scaled(MosaicSettingsPanel::BASE_BTN), scaled(MosaicSettingsPanel::BASE_BTN)));
                for (int index = 0; index < kinds.size(); ++index) {
                    auto *button = panel.kind_buttons.value(kinds[index]);
                    QVERIFY(button);
                    QVERIFY(button->text().isEmpty());
                    QVERIFY(!button->icon().isNull());
                    QCOMPARE(button->size(), panel.freehand_btn->size());
                    QCOMPARE(button->iconSize(), panel.freehand_btn->iconSize());
                    QCOMPARE(button->toolTip(), it.value()[index]);
                }
                if (width < 0) width = panel.sizeHint().width();
                QCOMPARE(panel.sizeHint().width(), width);
            }
        }
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
        QCOMPARE(D::parse_color(D::HSV, "hsv(220deg 75% 80%)")->color, blue);
        QCOMPARE(D::parse_color(D::HSV, "360, 100, 100")->color, QColor(255, 0, 0));
        QVERIFY(!D::parse_color(D::HSV, "220, 101, 80").has_value());
        QVERIFY(!D::parse_color(D::HSV, "361, 75, 80").has_value());
        QCOMPARE(D::format_color(D::HEX, blue), QString("#3366CC"));
        QCOMPARE(D::format_color(D::RGB, blue), QString("51, 102, 204"));
        QCOMPARE(D::format_color(D::HSV, blue), QString("220, 75%, 80%"));
        QCOMPARE(D::format_color(D::HSV, QColor(128, 128, 128), 0.5), QString("180, 0%, 50%"));
        QCOMPARE(D::format_color(D::HSL, blue), QString("220, 60%, 50%"));
        // A grey has no hue of its own and shows the one it is given.
        QCOMPARE(D::format_color(D::HSL, QColor(128, 128, 128), 0.5), QString("180, 0%, 50%"));
    }

    void colorDialogFieldsFollowTheFormatAndRejectBadText() {
        ColorPickerDialog dialog(QColor("#3366CC"));
        dialog.show();
        QSignalSpy changes(&dialog, &ColorPickerDialog::currentColorChanged);
        dialog.format_combo->setCurrentIndex(dialog.format_combo->findData(ColorPickerDialog::RGB));
        QVERIFY(dialog.value_edit->isHidden());
        QCOMPARE(dialog.channel_edits[0]->text(), QString("51"));
        QCOMPARE(dialog.channel_edits[1]->text(), QString("102"));
        QCOMPARE(dialog.channel_edits[2]->text(), QString("204"));
        auto *red = dialog.channel_edits[0];
        red->selectAll();
        QTest::keyClicks(red, "255");
        QCOMPARE(dialog.currentColor(), QColor(255, 102, 204));
        QCOMPARE(changes.size(), 3);
        // Unreadable text changes nothing, and Enter puts the colour back instead of confirming.
        red->selectAll();
        QTest::keyClicks(red, "300");
        QCOMPARE(dialog.currentColor(), QColor(30, 102, 204));
        QTest::keyClick(red, Qt::Key_Return);
        QVERIFY(dialog.isVisible());
        QCOMPARE(red->text(), QString("30"));
        dialog.setCurrentColor(QColor(255, 0, 0));
        dialog.format_combo->setCurrentIndex(dialog.format_combo->findData(ColorPickerDialog::HSL));
        QCOMPARE(dialog.channel_edits[0]->text(), QString("0"));
        QCOMPARE(dialog.channel_edits[1]->text(), QString("100"));
        QCOMPARE(dialog.channel_edits[2]->text(), QString("50"));
        dialog.channel_edits[2]->selectAll();
        QTest::keyClicks(dialog.channel_edits[2], "25");
        QCOMPARE(dialog.currentColor(), QColor(128, 0, 0));
        // The next dialog opens in the format last chosen.
        ColorPickerDialog next(QColor("#3366CC"));
        QCOMPARE(next.channel_edits[0]->text(), QString("220"));
        QCOMPARE(next.channel_edits[1]->text(), QString("60"));
        QCOMPARE(next.channel_edits[2]->text(), QString("50"));
        next.format_combo->setCurrentIndex(next.format_combo->findData(ColorPickerDialog::HEX));
        // Readable text and Enter confirm the dialog with that colour.
        dialog.format_combo->setCurrentIndex(dialog.format_combo->findData(ColorPickerDialog::HEX));
        dialog.value_edit->selectAll();
        QTest::keyClicks(dialog.value_edit, "#00aa00");
        QTest::keyClick(dialog.value_edit, Qt::Key_Return);
        QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
        QCOMPARE(dialog.selectedColor(), QColor("#00AA00"));
    }

    void colorDialogHsvChannelsKeepHueAndSaturationAtBlack() {
        using D = ColorPickerDialog;
        D dialog(QColor(51, 102, 204, 128));
        dialog.format_combo->setCurrentIndex(dialog.format_combo->findData(D::HSV));
        dialog.show();
        QCOMPARE(dialog.channel_edits[0]->text(), QString("220"));
        QCOMPARE(dialog.channel_edits[1]->text(), QString("75"));
        QCOMPARE(dialog.channel_edits[2]->text(), QString("80"));
        dialog.channel_edits[2]->selectAll();
        QTest::keyClicks(dialog.channel_edits[2], "0");
        QCOMPARE(dialog.currentColor(), QColor(0, 0, 0, 128));
        dialog.channel_edits[0]->selectAll();
        QTest::keyClicks(dialog.channel_edits[0], "120");
        QTest::keyClick(dialog.channel_edits[0], Qt::Key_Tab);
        QCOMPARE(dialog.channel_edits[0]->text(), QString("120"));
        QCOMPARE(dialog.channel_edits[1]->text(), QString("75"));
        dialog.channel_edits[2]->selectAll();
        QTest::keyClicks(dialog.channel_edits[2], "100");
        QCOMPARE(dialog.currentColor(), QColor(64, 255, 64, 128));
        dialog.format_combo->setCurrentIndex(dialog.format_combo->findData(D::HEX));
        QVERIFY(!dialog.value_edit->isHidden());
        for (auto *edit : dialog.channel_edits) QVERIFY(edit->isHidden());
        dialog.value_edit->setFocus();
        const QColor before_switch = dialog.currentColor();
        dialog.format_combo->setCurrentIndex(dialog.format_combo->findData(D::HSV));
        QCOMPARE(dialog.currentColor(), before_switch);
        QTest::keyClick(dialog.channel_edits[0], Qt::Key_Return);
        QCOMPARE(dialog.selectedColor(), before_switch);
        dialog.format_combo->setCurrentIndex(dialog.format_combo->findData(D::HEX));
    }

    void colorDialogInputsFitWithoutHorizontalScrolling() {
#ifdef Q_OS_WIN
        // The offscreen platform needs a real font for text measurements.
        const int font_id = QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR") + "/Fonts/segoeui.ttf");
        QVERIFY(font_id >= 0);
        const auto remove_font = qScopeGuard([font_id] { QFontDatabase::removeApplicationFont(font_id); });
#endif
        const auto restore = qScopeGuard([] { get_dialog_scale().set_percent(100); });
        using D = ColorPickerDialog;
        for (int scale : {100, 125, 150, 200}) {
            get_dialog_scale().set_percent(scale);
            for (auto options : {D::ShowAlphaChannel, D::NoOption}) {
                D dialog(QColor(Qt::white), options);
                dialog.show();
                for (auto format : {D::HEX, D::RGB, D::HSV, D::HSL}) {
                    dialog.format_combo->setCurrentIndex(dialog.format_combo->findData(format));
                    QApplication::processEvents();
                    QList<LineEdit *> fields;
                    if (format == D::HEX) fields.append(dialog.value_edit);
                    else for (auto *edit : dialog.channel_edits) fields.append(edit);
                    if (options == D::ShowAlphaChannel) fields.append(dialog.alpha_edit);
                    for (auto *edit : fields) {
                        QVERIFY(edit->isVisible());
                        const QString longest = edit == dialog.value_edit ? "#FFFFFFFF" : edit == dialog.alpha_edit ? "100%" : "360";
                        const int text_width = QFontMetrics(edit->font()).horizontalAdvance(longest);
                        QVERIFY2(edit->contentsRect().width() - 2 * dialog_scaled(4) - 4 >= text_width,
                                 qPrintable(QString("scale=%1 format=%2 text=%3 width=%4 textWidth=%5 font=%6")
                                                .arg(scale).arg(format).arg(longest).arg(edit->width()).arg(text_width).arg(edit->font().toString())));
                        QVERIFY(dialog.value_group->rect().contains(edit->geometry()));
                    }
                    QCOMPARE(dialog.format_combo->geometry().right() + 1, dialog.value_group->x());
                }
                dialog.format_combo->setCurrentIndex(dialog.format_combo->findData(D::HEX));
            }
        }
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
    // Both buttons share a width that holds the label in the current language.
    void colorDialogButtonsFitTheirLabels() {
        ColorPickerDialog dialog(QColor("#3366CC"));
        QCOMPARE(dialog.ok_button->width(), dialog.cancel_button->width());
        QFont font = dialog.font();
        font.setPixelSize(ColorPickerDialog::BASE_FONT);
        for (auto *button : {dialog.ok_button, dialog.cancel_button})
            QVERIFY(QFontMetrics(font).horizontalAdvance(button->text()) < button->width());
    }

    void colorDialogUsesTheStandardCaption() {
        use_test_app_icon();
        ColorPickerDialog dialog(QColor("#3366CC"));
        VERIFY_STANDARD_CAPTION(dialog);
    }

    // The old per-window divergences, rebuilt on a plain bar: each one must fail the shared caption check.
    void theCaptionCheckCatchesEachDivergence() {
        use_test_app_icon();
        const auto problem_after = [](const std::function<void(FluentTitleBar &)> &diverge) {
            FrostedFramelessDialog dialog;
            auto *bar = new FluentTitleBar(&dialog);
            dialog.setTitleBar(bar);
            dialog.setWindowTitle("Title");
            diverge(*bar);
            return caption_problem(dialog);
        };
        QVERIFY(problem_after([](FluentTitleBar &) {}).isEmpty());
        QVERIFY(!problem_after([](FluentTitleBar &bar) { bar.iconLabel->hide(); }).isEmpty());
        QVERIFY(!problem_after([](FluentTitleBar &bar) { bar.hBoxLayout->setContentsMargins(12, 0, 0, 0); }).isEmpty());
        QVERIFY(!problem_after([](FluentTitleBar &bar) {
            bar.hBoxLayout->removeWidget(bar.iconLabel);
            bar.hBoxLayout->removeWidget(bar.titleLabel);
            bar.iconLabel->move(200, 4);
            bar.titleLabel->move(230, 4);
        }).isEmpty());
        QVERIFY(!problem_after([](FluentTitleBar &bar) { bar.setIcon(QIcon()); bar.iconLabel->clear(); }).isEmpty());
    }

    // A width pinned with setFixedWidth survives the theme and scale refresh; a free combo
    // keeps the scaled floor.
    void comboKeepsAPinnedWidth() {
        ComboBox pinned;
        pinned.setFixedWidth(168);
        configure_dialog_control(pinned);
        QCOMPARE(pinned.minimumWidth(), 168);
        QCOMPARE(pinned.maximumWidth(), 168);
        ComboBox free;
        configure_dialog_control(free);
        QCOMPARE(free.minimumWidth(), widget_scaled(&free, ComboBox::BASE_MIN_WIDTH));
    }

    void colorDialogFollowsTheWindowScale() {
        const auto restore = qScopeGuard([] {
            get_dialog_scale().set_percent(100);
            get_ui_scale().set_percent(100);
        });
        get_dialog_scale().set_percent(100);
        ColorPickerDialog normal(QColor("#3366CC"));
        QCOMPARE(normal.size(), QSize(260, 380));
        QCOMPARE(normal.square->field_rect().size(), QSizeF(244, 199));
        QCOMPARE(normal.format_combo->size(), QSize(61, 30));
        QCOMPARE(normal.value_group->size(), QSize(181, 30));
        QVERIFY(normal.ok_button->width() >= 60);
        QCOMPARE(normal.ok_button->height(), 31);
        QCOMPARE(normal.eyedropper_button->size(), QSize(32, 33));
        // Track ends in line with the eyedropper's right side plus the gap, thumbs whole at both ends.
        QCOMPARE(normal.hue_slider->track_rect().height(), 10.0);
        get_dialog_scale().set_percent(150);
        ColorPickerDialog large(QColor("#3366CC"));
        // Rounded half to even: 298.5 -> 298, 91.5 -> 92, 46.5 -> 46.
        QCOMPARE(large.width(), 390);
        QCOMPARE(large.square->field_rect().size(), QSizeF(366, 298));
        QCOMPARE(large.format_combo->size(), QSize(92, 45));
        QVERIFY(large.ok_button->width() >= 90);
        QCOMPARE(large.ok_button->height(), 46);
        QCOMPARE(large.hue_slider->track_rect().height(), 15.0);
        normal.value_edit->ensurePolished();
        large.value_edit->ensurePolished();
        QCOMPARE(normal.value_edit->font().pixelSize(), 14);
        QCOMPARE(large.value_edit->font().pixelSize(), 21);
        get_ui_scale().set_percent(200);
        ColorPickerDialog panels_scaled(QColor("#3366CC"));
        QCOMPARE(panels_scaled.size(), large.size());
    }

    // A page note is an info icon and the text without the light-bulb emoji; it follows the window scale only.
    void pageHintDropsTheEmojiAndFollowsTheWindowScale() {
        const auto restore = qScopeGuard([] {
            get_dialog_scale().set_percent(100);
            get_ui_scale().set_percent(100);
        });
        const auto build = [](int window, int panel) {
            get_dialog_scale().set_percent(window);
            get_ui_scale().set_percent(panel);
            auto parent = std::make_unique<QWidget>();
            auto *hint = page_hint(parent.get(), QString::fromUcs4(U"💡") + QString(" Hint: Colors apply next time."));
            return std::make_pair(std::move(parent), hint);
        };
        const auto [normal, normal_hint] = build(100, 100);
        QCOMPARE(normal_hint->findChild<QLabel *>()->text(), QString("Hint: Colors apply next time."));
        QCOMPARE(normal_hint->findChild<IconBadge *>()->size(), QSize(16, 16));
        QCOMPARE(page_bottom_margin(), 24);
        const auto [large, large_hint] = build(150, 100);
        QCOMPARE(large_hint->findChild<IconBadge *>()->size(), QSize(24, 24));
        QCOMPARE(page_bottom_margin(), 36);
        const auto [panels, panels_hint] = build(100, 200);
        QCOMPARE(panels_hint->findChild<IconBadge *>()->size(), QSize(16, 16));
        QCOMPARE(page_bottom_margin(), 24);
    }

    // Each page is titled like its navigation item. A group named like its page would show up twice
    // in the navigation ("Screenshot > Screenshot"), and "Settings" adds nothing inside the settings window.
    void pagesAreTitledLikeTheirNavigationItems() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.build_all_pages();
        auto *title = dialog.findChild<QLabel *>("SettingsPageTitle");
        QVERIFY(title);
        const auto buttons = dialog.findChildren<QPushButton *>("FluentLiteNavItem");
        QCOMPARE(buttons.size(), 11);
        for (auto *button : buttons) {
            button->click();
            QCOMPARE(title->text(), button->text());
            auto *scroll = qobject_cast<QScrollArea *>(dialog.content_stack->currentWidget());
            QVERIFY2(scroll, qPrintable(button->text()));
            for (auto *group : scroll->widget()->findChildren<SettingCardGroup *>()) {
                const QString name = group->titleLabel->text();
                QVERIFY2(name != button->text(), qPrintable(name));
                QVERIFY2(!name.endsWith("Settings") && !name.endsWith("Options"), qPrintable(name));
            }
        }
    }

    // The information page shows the application's mark, name and version in the middle instead of a
    // title over a group of cards; the update check only where the updater is bundled.
    void aboutPageShowsTheApplicationInsteadOfATitle() {
        const auto restore = qScopeGuard([] {
            get_dialog_scale().set_percent(100);
            set_helper_source_for_test({});
        });
        for (const int percent : {100, 150}) {
            get_dialog_scale().set_percent(percent);
            // Bundled at 150% only, so both kinds of build are seen.
            const bool bundled = percent == 150;
            set_helper_source_for_test(bundled ? QCoreApplication::applicationFilePath() : QString());
            SettingsDialog dialog(config_, config_->get_hotkey());
            dialog.resize(dialog_scaled(1050), dialog_scaled(750));
            dialog.show();
            auto *title = dialog.findChild<QLabel *>("SettingsPageTitle");
            dialog.on_nav_changed(8, "about");
            QApplication::processEvents();
            QVERIFY(!title->isVisible());
            auto *page = qobject_cast<QScrollArea *>(dialog.content_stack->currentWidget());
            QVERIFY(page);
            QVERIFY(page->widget()->findChildren<SettingCardGroup *>().isEmpty());
            auto *mark = page->widget()->findChild<QWidget *>("AboutAppMark");
            QVERIFY(mark);
            QCOMPARE(mark->size(), QSize(dialog_scaled(128), dialog_scaled(128)));
            // Centred over the card column, which keeps clear of the scroll bar like on the other pages.
            const auto margins = page->widget()->layout()->contentsMargins();
            const int middle = (margins.left() + page->widget()->width() - margins.right()) / 2;
            QVERIFY(qAbs(mark->geometry().center().x() - middle) <= 1);
            for (const char *name : {"AboutName", "AboutVersion"}) {
                auto *label = page->widget()->findChild<QLabel *>(name);
                QVERIFY2(label && qAbs(label->geometry().center().x() - middle) <= 1, name);
            }
            QVERIFY(page->widget()->findChild<QPushButton *>("aboutSourceButton"));
            QVERIFY(page->widget()->findChild<QPushButton *>("aboutLicenseButton"));
            QCOMPARE(page->widget()->findChild<QPushButton *>("checkUpdateButton") != nullptr, bundled);
            // The repository link sits at the bottom of the page, below the buttons.
            auto *link = page->widget()->findChild<QPushButton *>("aboutRepositoryLink");
            QVERIFY(link && link->geometry().top() > page->widget()->findChild<QPushButton *>("aboutSourceButton")->geometry().bottom());
            dialog.on_nav_changed(1, "capture");
            QVERIFY(title->isVisible());
        }
    }

    // The last group does not touch the window's bottom edge, at any window scale.
    void settingsPagesLeaveRoomBelowTheirLastGroup() {
        const auto restore = qScopeGuard([] { get_dialog_scale().set_percent(100); });
        for (const int percent : {100, 150}) {
            get_dialog_scale().set_percent(percent);
            SettingsDialog dialog(config_, config_->get_hotkey());
            dialog.build_all_pages();
            for (const int index : {0, 10, 4, 1, 11, 12, 2, 3, 5, 6, 7, 8}) {
                auto *scroll = qobject_cast<QScrollArea *>(dialog.content_stack->widget(index));
                QVERIFY2(scroll, qPrintable(QString::number(index)));
                QCOMPARE(scroll->widget()->layout()->contentsMargins().bottom(), page_bottom_margin());
            }
        }
    }

    // The log folder and the latest log are ordinary cards: the path is the description, the buttons sit in the control column.
    void logPageStorageUsesStandardCards() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.build_all_pages();
        for (const char *name : {"path_lbl", "latest_log_lbl"}) {
            auto *label = dialog.attr<QLabel>(name);
            QVERIFY2(label, name);
            auto *card = qobject_cast<SettingCard *>(label->parentWidget());
            QVERIFY2(card && card->contentLabel == label, name);
            QVERIFY2(!label->isHidden() && !label->text().isEmpty(), name);
            QVERIFY2(!card->controlContainer->findChildren<QPushButton *>().isEmpty(), name);
        }
        QCOMPARE(dialog.attr<QLabel>("path_lbl")->text(), config_->get_log_dir());
    }

    // Settings cards, switches and buttons are window-scale widgets: they follow it and ignore the panel scale.
    void settingsControlsFollowTheWindowScale() {
        const auto restore = qScopeGuard([] {
            get_dialog_scale().set_percent(100);
            get_ui_scale().set_percent(100);
        });
        struct Built {
            std::unique_ptr<SwitchSettingCard> card;
            int sub_height, button_height, combo_height;
        };
        const auto build = [](int window, int panel) {
            get_dialog_scale().set_percent(window);
            get_ui_scale().set_percent(panel);
            Built built{std::make_unique<SwitchSettingCard>(IconSource{}, "Title", "Description"), 0, 0, 0};
            auto *card = built.card.get();
            auto *sub = new SubSwitchSettingCard("Sub", "", card);
            auto *button = new PushButton("Edit", card);
            auto *combo = new ComboBox(card);
            combo->addItem("One");
            configure_dialog_controls(*card);
            built.sub_height = sub->minimumHeight();
            button->ensurePolished();
            combo->ensurePolished();
            built.button_height = button->sizeHint().height();
            built.combo_height = combo->sizeHint().height();
            return built;
        };
        {
            const auto built = build(100, 100);
            QCOMPARE(built.card->minimumHeight(), 68);
            QCOMPARE(built.sub_height, 52);
            QCOMPARE(built.card->switchButton->size(), QSize(40, 20));
            QCOMPARE(built.card->iconBadge->size(), QSize(20, 20));
            QCOMPARE(built.button_height, 32);
            QCOMPARE(built.combo_height, 32);
        }
        {
            const auto built = build(150, 100);
            QCOMPARE(built.card->minimumHeight(), 102);
            QCOMPARE(built.sub_height, 78);
            QCOMPARE(built.card->switchButton->size(), QSize(60, 30));
            QCOMPARE(built.card->iconBadge->size(), QSize(30, 30));
            QCOMPARE(built.button_height, 48);
            QCOMPARE(built.combo_height, 48);
        }
        {
            const auto built = build(100, 200);
            QCOMPARE(built.card->minimumHeight(), 68);
            QCOMPARE(built.sub_height, 52);
            QCOMPARE(built.card->switchButton->size(), QSize(40, 20));
            QCOMPARE(built.button_height, 32);
            QCOMPARE(built.combo_height, 32);
        }
    }

    // A page sets its switches before it shows, so they must open in their final state; a click on screen still slides.
    void switchSlidesOnlyOnScreen() {
        QWidget host;
        auto *hidden = new SwitchButton(&host);
        hidden->setChecked(true);
        QCOMPARE(hidden->progress(), 1.0);
        hidden->setChecked(false);
        QCOMPARE(hidden->progress(), 0.0);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        hidden->setChecked(true);
        QVERIFY(hidden->progress() < 1.0);
        QTRY_COMPARE(hidden->progress(), 1.0);
    }

    // A switch row added after a card folds under it; the arrow hides the rows, and hidden rows leave no line behind.
    void settingsGroupFoldsSwitchRowsUnderTheirCard() {
        get_dialog_scale().set_percent(100);
        SettingCardGroup group("Saving");
        auto *header = new SettingCard(IconSource{}, "Auto-save", "Saves screenshots.", &group);
        group.addSettingCard(header);
        auto *first = new SubSwitchSettingCard("When done", "", &group);
        auto *second = new SubSwitchSettingCard("When pinned", "", &group);
        group.addSettingCard(first);
        group.addSettingCard(second);
        auto *plain = new SettingCard(IconSource{}, "Other", {}, &group);
        group.addSettingCard(plain);
        group.resize(700, 600);
        group.show();
        QApplication::processEvents();
        auto *expander = qobject_cast<SettingExpander *>(header->parentWidget());
        QVERIFY(expander);
        QVERIFY(expander->isExpanded());
        QVERIFY(first->isVisible() && second->isVisible());
        QVERIFY(!qobject_cast<SettingExpander *>(plain->parentWidget()));
        // Rows align with the title of the header, and their controls with each other and clear of the arrow.
        QCOMPARE(first->titleLabel->mapTo(&group, QPoint()).x(), header->titleLabel->mapTo(&group, QPoint()).x());
        const auto switch_right = [&](SwitchSettingCard *card) { return card->switchButton->mapTo(&group, QPoint(card->switchButton->width(), 0)).x(); };
        QCOMPARE(switch_right(first), switch_right(second));
        QVERIFY(switch_right(first) <= expander->arrow()->mapTo(&group, QPoint()).x());
        QCOMPARE(first->minimumHeight(), 52);
        const int open_height = group.cardLayout->heightForWidth(group.width());
        QTest::mouseClick(expander->arrow(), Qt::LeftButton);
        QApplication::processEvents();
        QVERIFY(!expander->isExpanded());
        QVERIFY(!first->isVisible() && !second->isVisible());
        QVERIFY(group.cardLayout->heightForWidth(group.width()) < open_height);
        expander->setExpanded(true);
        // One row hidden: its line goes too, and the height follows.
        group.setCardVisible(first, false);
        QApplication::processEvents();
        QVERIFY(!first->isVisible() && second->isVisible());
        QVERIFY(group.cardLayout->heightForWidth(group.width()) < open_height);
        // No row left: no body and no arrow.
        group.setCardVisible(second, false);
        QApplication::processEvents();
        QVERIFY(!expander->arrow()->isVisible());
        group.setCardVisible(header, false);
        QVERIFY(!expander->isVisible());
    }

    // A row handed to a group gets the card background; a row inside a larger card stays bare.
    void groupRowsPaintTheirOwnCard() {
        get_ui_theme()->set_mode("light", false);
        const auto restore = qScopeGuard([] { get_ui_theme()->set_mode("system", false); });
        SettingCardGroup group("Storage");
        auto *row = new CardSurface(&group);
        row->setFixedHeight(40);
        auto *bare = new CardSurface(&group);
        bare->setFixedHeight(40);
        group.addSettingCard(row);
        group.resize(300, 200);
        group.show();
        bare->move(0, 120);
        bare->show();
        QApplication::processEvents();
        // Inside the border the card is a translucent white over the ground; a bare row shows the ground only.
        const auto center = [](QWidget *widget) { return widget->grab().toImage().pixelColor(widget->width() / 2, widget->height() / 2).rgb(); };
        QVERIFY(center(row) != center(bare));
        const QImage painted = row->grab().toImage();
        QVERIFY(painted.pixelColor(0, row->height() / 2) != painted.pixelColor(row->width() / 2, row->height() / 2));
    }

    // The settings window sits on a flat grey, with the content layer over it rounded only at its top-left corner.
    void layeredBackgroundRoundsOnlyTheContentLayersTopLeftCorner() {
        struct Probe : QWidget {
            void paintEvent(QPaintEvent *) override { paint_layered_background(this, 8, QRect(100, 40, 200, 160), 8); }
        };
        const auto restore = qScopeGuard([] { get_ui_theme()->set_mode("system", false); });
        for (const bool dark : {false, true}) {
            get_ui_theme()->set_mode(dark ? "dark" : "light", false);
            Probe probe;
            probe.setAttribute(Qt::WA_TranslucentBackground);
            probe.resize(300, 200);
            probe.show();
            QApplication::processEvents();
            const QImage image = probe.grab().toImage();
            const QColor page = css_color(get_ui_theme()->tokens().page_background);
            // Left of the layer: the page colour; inside it: the layer's veil over it.
            QCOMPARE(image.pixelColor(20, 100).rgb(), page.rgb());
            QVERIFY(image.pixelColor(200, 100).rgb() != page.rgb());
            // The corner of the layer is cut away at the top left and square at the bottom left.
            QCOMPARE(image.pixelColor(101, 41).rgb(), page.rgb());
            QVERIFY(image.pixelColor(101, 198).rgb() != page.rgb());
            // The window's own rounding clips the layer at the bottom right as well.
            QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
            QCOMPARE(image.pixelColor(299, 199).alpha(), 0);
        }
    }

    // Combo lists open on an opaque ground in both themes.
    void fluentComboListsOpenOpaque() {
        const auto restore = qScopeGuard([] { get_ui_theme()->set_mode("system", false); });
        for (const bool dark : {false, true}) {
            get_ui_theme()->set_mode(dark ? "dark" : "light", false);
            QWidget host;
            auto *combo = new ComboBox(&host);
            for (const auto *item : {"One", "Two", "Three"}) combo->addItem(item);
            host.resize(200, 100);
            host.show();
            combo->showPopup();
            QApplication::processEvents();
            const QImage list = combo->view()->grab().toImage();
            const QColor below(list.pixelColor(list.width() / 2, list.height() - 2));
            QCOMPARE(below.alpha(), 255);
            QCOMPARE(below.lightness() > 128, !dark);
            combo->hidePopup();
        }
    }

    // Without its chevron a combo leaves the drop-down column empty, for the centred entry to use.
    void comboChevronCanBeLeftOut() {
        QWidget host;
        auto *combo = new ComboBox(&host);
        combo->setFixedWidth(160);
        host.resize(200, 60);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        const auto ink_in_chevron_column = [combo] {
            const QImage image = combo->grab().toImage();
            int dark = 0;
            for (int x = image.width() - 26; x < image.width() - 10; ++x)
                for (int y = image.height() / 2 - 4; y <= image.height() / 2 + 4; ++y)
                    if (image.pixelColor(x, y).lightness() < 140) ++dark;
            return dark;
        };
        QVERIFY(combo->chevronVisible());
        QVERIFY(ink_in_chevron_column() > 0);
        combo->setChevronVisible(false);
        QVERIFY(!combo->chevronVisible());
        QCOMPARE(ink_in_chevron_column(), 0);
    }

    // An overlay of the capture, so it follows the panel scale and ignores the window scale.
    void screenshotCountdownFollowsThePanelScale() {
        const auto restore = qScopeGuard([] {
            get_dialog_scale().set_percent(100);
            get_ui_scale().set_percent(100);
        });
        get_dialog_scale().set_percent(100);
        get_ui_scale().set_percent(100);
        ScreenshotCountdown normal(nullptr);
        QCOMPARE(normal.size(), QSize(96, 96));
        get_dialog_scale().set_percent(150);
        ScreenshotCountdown window_scaled(nullptr);
        QCOMPARE(window_scaled.size(), normal.size());
        get_ui_scale().set_percent(150);
        ScreenshotCountdown panel_scaled(nullptr);
        QCOMPARE(panel_scaled.size(), QSize(144, 144));
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

    // The QColorDialog eyedropper: the dialog grabs mouse and keyboard with a cross cursor and
    // previews the pixel under the pointer; a release picks there with the dialog's opacity,
    // Esc restores the colour, hue included. Space toggles it, Enter picks and accepts.
    void eyedropperPreviewsPicksAndRestores() {
        // Red left of x = 50, blue from there.
        ColorPickerDialog::set_screen_sampler_for_test([](const QPoint &pt) { return pt.x() < 50 ? QColor(255, 0, 0) : QColor(0, 0, 255); });
        const auto reset_sampler = qScopeGuard([] { ColorPickerDialog::set_screen_sampler_for_test({}); });
        const QColor start(0, 255, 0, 128);
        ColorPickerDialog dialog(start);
        dialog.show();
        dialog.move(300, 200);
        dialog.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&dialog));
        const auto move_to = [&dialog](int x, int y) {
            QMouseEvent move(QEvent::MouseMove, dialog.mapFromGlobal(QPointF(x, y)), QPointF(x, y), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&dialog, &move);
        };
        const auto release = [&dialog] {
            const QPointF global = QCursor::pos();
            QMouseEvent up(QEvent::MouseButtonRelease, dialog.mapFromGlobal(global), global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&dialog, &up);
        };

        QCursor::setPos(10, 10);
        dialog.eyedropper_button->click();
        QVERIFY(dialog.picking_screen());
        QVERIFY(dialog.eyedropper_button->isChecked());
        QCOMPARE(dialog.eyedropper_button->grab().toImage().pixelColor(4, 4), QColor("#58748D"));
        QCOMPARE(QWidget::mouseGrabber(), &dialog);
        QCOMPARE(QWidget::keyboardGrabber(), &dialog);
        QVERIFY(QApplication::overrideCursor());
        QCOMPARE(QApplication::overrideCursor()->shape(), Qt::CrossCursor);
        QVERIFY(dialog.pick_catcher()->isVisible());
        QCOMPARE(dialog.currentColor(), QColor(255, 0, 0, 128));
        move_to(80, 10);
        QCOMPARE(dialog.currentColor(), QColor(0, 0, 255, 128));
        // Outside the dialog the timer follows the pointer and keeps the catcher under it.
        QCursor::setPos(20, 30);
        QTRY_COMPARE(dialog.currentColor(), QColor(255, 0, 0, 128));
        QCOMPARE(dialog.pick_catcher()->geometry(), QRect(18, 28, 5, 5));
        QTest::keyClick(&dialog, Qt::Key_Escape);
        QVERIFY(!dialog.picking_screen());
        QVERIFY(dialog.isVisible());
        QCOMPARE(dialog.currentColor(), start);
        QVERIFY(close_to(dialog.square->hue(), 120.0 / 360));
        QVERIFY(!dialog.eyedropper_button->isChecked());
        QVERIFY(!QWidget::mouseGrabber());
        QVERIFY(!QWidget::keyboardGrabber());
        QVERIFY(!QApplication::overrideCursor());
        QVERIFY(!dialog.pick_catcher()->isVisible());

        // The release picks where the pointer is.
        dialog.eyedropper_button->click();
        move_to(10, 10);
        QCursor::setPos(80, 10);
        release();
        QVERIFY(!dialog.picking_screen());
        QCOMPARE(dialog.currentColor(), QColor(0, 0, 255, 128));

        // Space starts and picks; Enter picks and accepts. Offscreen windows take the activation
        // when shown, the catcher too, and the shortcut needs the dialog active.
        dialog.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&dialog));
        QCOMPARE(QApplication::focusWidget(), &dialog);
        QCursor::setPos(10, 10);
        QTest::keyClick(&dialog, Qt::Key_Space);
        QVERIFY(dialog.picking_screen());
        QTest::keyClick(&dialog, Qt::Key_Space);
        QVERIFY(!dialog.picking_screen());
        QCOMPARE(dialog.currentColor(), QColor(255, 0, 0, 128));
        dialog.eyedropper_button->click();
        QCursor::setPos(80, 10);
        QTest::keyClick(&dialog, Qt::Key_Return);
        QVERIFY(!dialog.picking_screen());
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(dialog.selectedColor(), QColor(0, 0, 255, 128));
        QVERIFY(!QWidget::mouseGrabber());

        // Closing the dialog ends it.
        ColorPickerDialog closing(start);
        closing.show();
        closing.eyedropper_button->click();
        QVERIFY(closing.picking_screen());
        closing.reject();
        QVERIFY(!closing.picking_screen());
        QVERIFY(!QWidget::mouseGrabber());
        QVERIFY(!QApplication::overrideCursor());
    }

    // On the real desktop the catcher sits under the pointer, so the click lands on it rather
    // than on the window below, and the never-painted catcher stays out of the screen grab.
    void eyedropperCatcherTakesTheClickOnTheDesktop() {
        if (QGuiApplication::platformName() != "windows") QSKIP("needs -platform windows");
#ifdef Q_OS_WIN
        const QRect area = QGuiApplication::primaryScreen()->availableGeometry();
        QWidget target;
        target.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        target.setStyleSheet("background: #c81e3c;");
        target.setGeometry(area.left() + 40, area.top() + 40, 80, 80);
        target.show();
        QVERIFY(QTest::qWaitForWindowExposed(&target));
        ColorPickerDialog dialog(QColor(0, 128, 0));
        dialog.show();
        dialog.move(area.left() + 200, area.top() + 40);
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        const QPoint saved = QCursor::pos();
        const auto restore_pointer = qScopeGuard([saved] { QCursor::setPos(saved); });
        const QPoint over = target.geometry().center();
        const auto top_at = [](const QPoint &pt) { return GetAncestor(WindowFromPoint(POINT{pt.x(), pt.y()}), GA_ROOT); };
        dialog.eyedropper_button->click();
        QVERIFY(dialog.picking_screen());
        QCursor::setPos(over);
        QTRY_COMPARE(dialog.currentColor(), QColor(0xc8, 0x1e, 0x3c));
        QCOMPARE(top_at(over), reinterpret_cast<HWND>(dialog.pick_catcher()->winId()));
        QCOMPARE(QWidget::mouseGrabber(), &dialog);
        QTest::keyClick(&dialog, Qt::Key_Escape);
        QVERIFY(!dialog.picking_screen());
        QCOMPARE(dialog.currentColor(), QColor(0, 128, 0));
        QCOMPARE(top_at(over), reinterpret_cast<HWND>(target.winId()));
#endif
    }
};

QTEST_MAIN(UiWidgetTests)
#include "test_ui_widgets.moc"
