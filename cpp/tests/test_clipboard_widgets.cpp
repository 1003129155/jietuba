#include "clipboard_test_support.h"
#include "clipboard/controllers/foreground_tracker.h"
#include "clipboard/ui/panels/setting_panel.h"
#include "clipboard/ui/theme/theme_styles.h"
#include "clipboard/ui/theme/themes.h"
#include "clipboard/ui/widgets/item_delegate.h"
#include "clipboard/ui/widgets/quick_edit_popup.h"
#include "core/ui_scale.h"
#include "settings/special_paste.h"
#include "ui/fluent_lite/cards.h"
#include "ui/fluent_lite/labels.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/segmented.h"
#include "ui/settings_ui/dialog.h"
#include "ui/settings_ui/special_paste_dialog.h"
#include <QBuffer>
#include <QDesktopServices>
#include <QListWidget>
#include <QPainter>
#include <QScreen>
#include <QSignalSpy>
#include <QStyleOptionViewItem>
#include <QTest>
#include <QToolButton>
#include <QWidgetAction>

using namespace clipboard_test;

namespace {
ClipboardItem make_item(qint64 id, const QString &type, const QString &content) {
    ClipboardItem item;
    item.id = id;
    item.content_type = type;
    item.content = content;
    item.created_at = QDateTime(QDate(2026, 10, 1), QTime(9, 30));
    return item;
}

QString png_data_url(const QSize &size) {
    QImage image(size, QImage::Format_RGB32);
    image.fill(Qt::darkGreen);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return "data:image/png;base64," + QString::fromLatin1(bytes.toBase64());
}
}

class UrlCatcher : public QObject {
    Q_OBJECT
public:
    QList<QUrl> urls;
public slots:
    void open(const QUrl &url) { urls << url; }
};

// The clipboard's smaller pieces: row painting, the hover preview and quick edit popups,
// the gear menu on its own, theme styles, foreground tracking and the settings page's data actions.
class ClipboardWidgetTests : public QObject {
    Q_OBJECT
    Environment env_;
    UrlCatcher urls_;

private slots:
    void initTestCase() {
        QVERIFY(env_.init(this));
        QDesktopServices::setUrlHandler("file", &urls_, "open");
    }

    void cleanupTestCase() { QDesktopServices::unsetUrlHandler("file"); }

    void init() {
        QVERIFY(env_.fresh_database());
        urls_.urls.clear();
    }

    void rowsSizeAndPaintEveryKind() {
        QCOMPARE(image_row_span("small"), image_row_span("unknown"));
        QVERIFY(image_row_span("medium") > image_row_span("small"));
        QVERIFY(image_row_span("large") > image_row_span("medium"));

        const Theme theme = get_theme_manager()->get_current_theme();
        QListWidget list;
        list.resize(360, 400);
        QList<ClipboardItem> items{make_item(1, "text", "a short line"), make_item(2, "text", QString("long ").repeated(80)),
                                   make_item(3, "image", "[300x100]"), make_item(4, "file", file_payload({"C:\\a.txt", "C:\\b.txt"})),
                                   make_item(5, "image", "[10x10]")};
        items[2].thumbnail = png_data_url(QSize(300, 100));
        items[1].is_pinned = true;
        items[0].source_app = "Notepad";
        for (const auto &item : items) {
            auto *row = new QListWidgetItem;
            row->setData(ROLE_ITEM_ID, item.id);
            row->setData(ROLE_ITEM_DATA, QVariant::fromValue(item));
            list.addItem(row);
        }
        ClipboardItemDelegate delegate(&list, theme, 17, 0, true, 8, true, "small");
        delegate.set_pick_keys("123");
        QStyleOptionViewItem option;
        option.rect = QRect(0, 0, 360, 60);
        const int text_height = delegate.sizeHint(option, list.model()->index(0, 0)).height();
        const int image_small = delegate.sizeHint(option, list.model()->index(2, 0)).height();
        delegate.set_image_size("large");
        QVERIFY(delegate.sizeHint(option, list.model()->index(2, 0)).height() > image_small);
        QVERIFY(image_small >= text_height);
        delegate.set_show_metadata(false);
        QVERIFY(delegate.sizeHint(option, list.model()->index(0, 0)).height() < text_height);

        auto paint_row = [&](int row) {
            QImage image(360, 120, QImage::Format_ARGB32);
            image.fill(Qt::white);
            QPainter painter(&image);
            QStyleOptionViewItem row_option;
            row_option.rect = QRect(0, 0, 360, 120);
            delegate.paint(&painter, row_option, list.model()->index(row, 0));
            painter.end();
            return image;
        };
        for (int row = 0; row < list.count(); ++row) QVERIFY(!paint_row(row).isNull());
        // A selected or hovered row gets the left bar; with a multiple selection only selected rows do.
        const QColor bar(theme.colors.border_selected);
        delegate.set_highlighted_id(1);
        QCOMPARE(paint_row(0).pixelColor(1, 60), bar);
        delegate.set_marked_ids({1});
        delegate.set_highlighted_id(2);
        QCOMPARE(paint_row(0).pixelColor(1, 60), bar);
        QVERIFY(paint_row(1).pixelColor(1, 60) != bar);
        // Opacity, other themes, hidden icons and shortcuts all still paint.
        delegate.set_window_opacity(40);
        delegate.set_hide_file_icon(true);
        delegate.set_show_shortcuts(false);
        delegate.set_display_lines(13);
        for (const auto &[name, theme_entry] : PRESET_THEMES()) {
            delegate.set_theme(theme_entry);
            QVERIFY(!paint_row(3).isNull());
        }
        delegate.apply_scale();
        ClipboardItemDelegate::clear_thumb_cache();
        QVERIFY(!paint_row(2).isNull());
    }

    void thePreviewShowsTextFilesAndImages() {
        auto *preview = PreviewPopup::instance();
        preview->set_manager(&ClipboardManager::instance());
        // Disabled: nothing shows.
        preview->set_display_enabled(false);
        preview->show_preview(make_item(1, "text", "hidden"), QPoint(100, 100), 0);
        QVERIFY(!preview->isVisible());
        preview->set_display_enabled(true);
        preview->show_preview(make_item(1, "text", "hello\nworld"), QPoint(100, 100), 0);
        QVERIFY(preview->isVisible());
        QVERIFY(preview->title_label->text().startsWith("2026-10-01"));
        QCOMPARE(preview->content_widget->toPlainText(), QString("hello\nworld"));
        // A long text wraps at the maximum width.
        preview->show_preview(make_item(2, "text", QString("word ").repeated(400)), QPoint(100, 100), 0);
        QCOMPARE(preview->content_widget->width(), scaled(500));
        // Files: one, several in one folder, several folders, none and broken content.
        preview->show_preview(make_item(3, "file", file_payload({"C:\\docs\\a.txt"})), QPoint(10, 10), 0);
        QCOMPARE(preview->content_widget->toPlainText(), QString("a.txt\n\nC:\\docs\\a.txt"));
        preview->show_preview(make_item(4, "file", file_payload({"C:\\docs\\a.txt", "C:\\docs\\b.txt"})), QPoint(10, 10), 0);
        QCOMPARE(preview->content_widget->toPlainText(), QString("a.txt\nb.txt\n\nC:\\docs"));
        preview->show_preview(make_item(5, "file", file_payload({"C:\\x\\a.txt", "C:\\y\\b.txt", "C:\\y\\c.txt"})), QPoint(10, 10), 0);
        QCOMPARE(preview->content_widget->toPlainText(), QString("[1] a.txt\nC:\\x\\a.txt\n\n[2]\n  b.txt\n  c.txt\nC:\\y"));
        preview->show_preview(make_item(6, "file", "{\"files\": []}"), QPoint(10, 10), 0);
        QVERIFY(!preview->content_widget->toPlainText().isEmpty());
        preview->show_preview(make_item(7, "file", "not json"), QPoint(10, 10), 0);
        QCOMPARE(preview->content_widget->toPlainText(), QString("not json"));
        // An image without stored pixels falls back to its thumbnail.
        auto image = make_item(8, "image", "[30x20]");
        image.thumbnail = png_data_url(QSize(30, 20));
        preview->show_preview(image, QPoint(10, 10), 0);
        QVERIFY(preview->image_label->isVisible());
        QVERIFY(!preview->content_widget->isVisible());
        // Several pictures copied from Word are kept without pixels: their text shows instead of the last picture.
        preview->show_preview(make_item(10, "image", "[PNG+CF_DIB 7.9 MB]"), QPoint(10, 10), 0);
        QVERIFY(preview->isVisible());
        QVERIFY(!preview->image_label->isVisible());
        QCOMPARE(preview->content_widget->toPlainText(), QString("[PNG+CF_DIB 7.9 MB]"));
        // Delayed: hiding before the delay cancels it; the same item shown twice stays put.
        preview->hide_preview();
        preview->show_preview(make_item(9, "text", "later"), QPoint(10, 10), 50);
        QVERIFY(!preview->isVisible());
        preview->hide_preview();
        QTest::qWait(100);
        QVERIFY(!preview->isVisible());
        preview->show_preview(make_item(9, "text", "later"), QPoint(10, 10), 20);
        QTRY_VERIFY(preview->isVisible());
        preview->show_preview(make_item(9, "text", "later"), QPoint(10, 10), 0);
        preview->show_preview(std::nullopt, QPoint());
        QVERIFY(!preview->isVisible());
        preview->apply_scale();
        preview->set_display_enabled(false);
    }

    void sidePositionsAvoidTheWindowAndStayOnScreen() {
        const QRect screen = QGuiApplication::primaryScreen()->availableGeometry();
        const QSize size(100, 80);
        const QRect middle(screen.center().x() - 50, screen.center().y() - 50, 100, 100);
        // Left preferred and possible.
        QCOMPARE(side_position(size, middle.topLeft(), middle, "left", 10).x(), middle.left() - 100 - 10);
        // Right preferred, or automatic.
        QCOMPARE(side_position(size, middle.topLeft(), middle, "right", 10).x(), middle.right() + 10);
        QCOMPARE(side_position(size, middle.topLeft(), middle, "auto", 10).x(), middle.right() + 10);
        // A window filling the left: the right side.
        const QRect left_edge(screen.left(), screen.top(), 50, 100);
        QCOMPARE(side_position(size, left_edge.topLeft(), left_edge, "left", 10).x(), left_edge.right() + 10);
        // Neither side fits: the roomier one, clamped on screen.
        const QRect wide(screen.left() + 20, screen.top(), screen.width() - 40, 100);
        const QPoint clamped = side_position(size, wide.topLeft(), wide, "left", 10);
        QVERIFY(clamped.x() >= screen.left() && clamped.x() + size.width() <= screen.right() + 1);
        // Without a window: right of the point, or left of it near the screen edge; never below the screen.
        const QPoint near_right(screen.right() - 10, screen.bottom() - 10);
        const QPoint placed = side_position(size, near_right);
        QVERIFY(placed.x() + size.width() <= near_right.x());
        QVERIFY(placed.y() + size.height() <= screen.bottom());
        QVERIFY(side_position(size, QPoint(screen.left() + 10, screen.top() + 10)).x() > screen.left() + 10);
    }

    void quickEditKeysSaveCancelAndPaste() {
        QWidget owner;
        QuickEditPopup popup(&owner);
        QSignalSpy finished(&popup, &QuickEditPopup::finished);
        auto item = make_item(5, "text", "body");
        item.html_content = "<b>body</b>";
        const Theme theme = get_theme_manager()->get_current_theme();
        popup.open_for(item, QPoint(200, 200), QRect(300, 100, 200, 300), theme);
        QVERIFY(popup.is_open());
        QCOMPARE(popup.editor->toPlainText(), QString("body"));
        QVERIFY(!popup.format_hint->text().isEmpty());
        popup.editor->setPlainText("changed");
        QTest::keyClick(popup.editor, Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(finished.size(), 1);
        auto result = finished.last().first().value<QuickEditResult>();
        QCOMPARE(result.item_id, qint64(5));
        QCOMPARE(result.text.value_or(QString()), QString("changed"));
        QVERIFY(!result.paste);
        QVERIFY(!popup.is_open());

        item.html_content.reset();
        popup.open_for(item, QPoint(200, 200), QRect(), theme);
        QVERIFY(popup.format_hint->text().isEmpty());
        QTest::keyClick(popup.editor, Qt::Key_Return, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(finished.last().first().value<QuickEditResult>().paste);

        popup.open_for(item, QPoint(200, 200), QRect(), theme);
        QTest::keyClick(popup.editor, Qt::Key_Escape);
        QVERIFY(!finished.last().first().value<QuickEditResult>().text);

        popup.open_for(item, QPoint(200, 200), QRect(), theme);
        popup.save_and_paste_button->click();
        QVERIFY(finished.last().first().value<QuickEditResult>().paste);
        popup.open_for(item, QPoint(200, 200), QRect(), theme);
        popup.save_button->click();
        QVERIFY(!finished.last().first().value<QuickEditResult>().paste);
        // Focus leaving keeps the text and says so.
        popup.open_for(item, QPoint(200, 200), QRect(), theme);
        popup.close_for_focus_loss();
        QVERIFY(finished.last().first().value<QuickEditResult>().focus_left);
        // Finishing twice reports once.
        const int count = finished.size();
        popup.cancel();
        QCOMPARE(finished.size(), count);
    }

    void theGearMenuReportsEveryChoice() {
        QWidget parent;
        QStringList calls;
        SettingMenuOptions options;
        options.tr = [](const char *text) { return QString::fromUtf8(text); };
        options.paste_with_html = true;
        options.auto_paste = false;
        options.window_opacity = 20;
        options.current_font_size = 15;
        options.current_image_size = "medium";
        options.current_theme_name = "light";
        options.current_group_bar_position = "left";
        options.opacity_options = {0, 20};
        options.font_size_options = {15, 17};
        options.on_toggle_paste_html = [&calls](bool checked) { calls << QString("html %1").arg(checked); };
        options.on_toggle_auto_paste = [&calls](bool checked) { calls << QString("auto %1").arg(checked); };
        options.on_toggle_close_after_paste = [&calls](bool) { calls << "close"; };
        options.on_toggle_move_to_top = [&calls](bool) { calls << "top"; };
        options.on_toggle_show_metadata = [&calls](bool) { calls << "meta"; };
        options.on_toggle_preserve_search = [&calls](bool) { calls << "search"; };
        options.on_set_opacity = [&calls](int value) { calls << QString("opacity %1").arg(value); };
        options.on_set_font_size = [&calls](int value) { calls << QString("font %1").arg(value); };
        options.on_set_image_size = [&calls](const QString &value) { calls << "image " + value; };
        options.on_set_theme = [&calls](const QString &value) { calls << "theme " + value; };
        options.on_add_item = [&calls] { calls << "add"; };
        options.on_set_group_bar_position = [&calls](const QString &value) { calls << "bar " + value; };
        options.multi_paste_separator = " | ";
        options.multi_paste_order = "newest_first";
        options.multi_paste_keep_merged = true;
        options.on_set_multi_paste_separator = [&calls](const QString &value) { calls << "separator " + escape_separator(value); };
        options.on_set_multi_paste_order = [&calls](const QString &value) { calls << "order " + value; };
        options.on_toggle_multi_paste_keep_merged = [&calls](bool checked) { calls << QString("keep %1").arg(checked); };
        QPointer<QMenu> menu = show_setting_menu(&parent, options);
        QVERIFY(menu->isVisible());
        QHash<QString, QAction *> actions;
        for (auto *action : menu->findChildren<QAction *>()) actions.insert(action->text(), action);
        // Current values are checked.
        QVERIFY(actions["Paste with Format"]->isChecked());
        QVERIFY(!actions["Auto Paste After Selection"]->isChecked());
        QVERIFY(actions["20%"]->isChecked());
        QVERIFY(actions["15px"]->isChecked());
        QVERIFY(actions["Medium"]->isChecked());
        QVERIFY(actions[QString::fromUtf8("◀ Left")]->isChecked());
        QVERIFY(actions["Last Selected First"]->isChecked());
        QVERIFY(actions["Record Content"]->isChecked());
        // A custom separator shows in the field; no preset is checked.
        auto *custom = menu->findChild<LineEdit *>();
        QVERIFY(custom);
        QCOMPARE(custom->text(), QString(" | "));
        for (const char *label : {"New Line", "Comma", "No Separator"}) QVERIFY(!actions[label]->isChecked());
        for (const char *label : {"Paste with Format", "Auto Paste After Selection", "Close After Paste", "Move to Top After Paste", "Show Time and Source",
                                  "Preserve Search on Reopen", "Opaque", "17px", "Large", "First Selected First", "Record Content", "Comma", "Add Content"})
            actions[label]->trigger();
        actions[QString::fromUtf8("▲ Top")]->trigger();
        QVERIFY(calls.contains("html 0"));
        QVERIFY(calls.contains("auto 1"));
        QVERIFY(calls.contains("opacity 0"));
        QVERIFY(calls.contains("font 17"));
        QVERIFY(calls.contains("image large"));
        QVERIFY(calls.contains("order oldest_first"));
        QVERIFY(calls.contains("keep 0"));
        QVERIFY(calls.contains("separator , "));
        QVERIFY(calls.contains("add"));
        QVERIFY(calls.contains("bar top"));
        // Theme swatches are buttons inside the menu.
        QPushButton *swatch = nullptr;
        for (auto *button : menu->findChildren<QPushButton *>()) swatch = button;
        QVERIFY(swatch);
        swatch->click();
        QVERIFY(calls.last().startsWith("theme "));
        // The custom separator understands escapes and closes the menu.
        custom->setText("\\n--\\n");
        emit custom->returnPressed();
        QVERIFY(calls.contains("separator \\n--\\n"));
        QTRY_VERIFY(!menu);
    }

    void themesAndTheirStyles() {
        auto *manager = get_theme_manager();
        QSignalSpy changed(manager, &ThemeManager::theme_changed);
        const QString original = manager->get_current_theme().name;
        QVERIFY(!manager->set_theme("no such theme"));
        QCOMPARE(changed.size(), 0);
        QVERIFY(!preset_theme("no such theme"));
        QCOMPARE(THEME_LIGHT().name, QString("light"));
        QCOMPARE(PRESET_THEME_SWATCHES().size(), PRESET_THEMES().size());
        for (const auto &[name, theme] : PRESET_THEMES()) {
            QVERIFY(manager->set_theme(name));
            QCOMPARE(env_.config->get_clipboard_theme(), name);
            const ThemeStyleGenerator styles(theme);
            for (const QString &css : {styles.generate_window_style(), styles.generate_window_style(50), styles.generate_list_widget_style(30),
                                       styles.generate_search_bar_style(30), styles.generate_time_filter_bar_style(30), styles.generate_quick_edit_style(),
                                       styles.generate_menu_style(), styles.generate_search_input_style(true), styles.generate_search_input_style(false),
                                       styles.generate_clear_search_btn_style(), styles.generate_filter_toggle_btn_style(),
                                       styles.generate_time_filter_date_edit_style(), styles.generate_time_filter_calendar_style(),
                                       styles.generate_time_filter_action_btn_style(true), styles.generate_time_filter_action_btn_style(false),
                                       styles.generate_time_filter_type_btn_style(), styles.generate_menu_btn_style()})
                QVERIFY2(css.contains("{") && css.contains("}"), qPrintable(name));
        }
        QVERIFY(!changed.isEmpty());
        QCOMPARE(ThemeStyleGenerator::hex_to_rgb("#FF8000"), QString("255, 128, 0"));
        QVERIFY(manager->set_theme(original));
    }

    void foregroundTrackingSkipsThisProcess() {
        QVERIFY(!is_alive(0));
        QVERIFY(!get_window_pid(0));
        QVERIFY(get_window_class(0).isEmpty());
        QCOMPARE(get_current_pid(), static_cast<quint32>(QCoreApplication::applicationPid()));
        if (const Hwnd foreground = get_foreground_hwnd()) {
            QVERIFY(is_alive(foreground));
            QVERIFY(get_window_pid(foreground));
        }
        ForegroundWindowTracker tracker;
        tracker.set_excluded([](Hwnd) { return true; });
        tracker.start();
        // Everything excluded: no target.
        QCOMPARE(tracker.target_hwnd(), Hwnd(0));
        QVERIFY(!tracker.sample());
        tracker.stop();
        tracker.set_excluded({});
        tracker.sample();
        tracker.stop();
    }

    void theSpecialPasteEditorHasATabPerKind() {
        SpecialPasteDialog editor(special_paste::default_entries());
        editor.show();
        QVERIFY(editor.tabs);
        editor.tabs->setCurrentItem("file");
        QVERIFY(!editor.grab().isNull());
        editor.tabs->setCurrentItem("text");
        QCOMPARE(editor.entries(), special_paste::default_entries());
    }

    void theSettingsPageMovesClearsAndOpensTheData() {
        const auto group = ClipboardManager::instance().create_group("Keep");
        const auto ids = add_texts({"loose", "grouped"});
        QVERIFY(ClipboardManager::instance().move_to_group(ids[1], *group));
        SettingsDialog dialog(env_.config, "");
        dialog.show();
        dialog.on_nav_changed(2, "clipboard");
        auto button = [&dialog](const char *text) -> QPushButton * {
            for (auto *candidate : dialog.findChildren<QPushButton *>())
                if (candidate->text() == settings_tr(text) && candidate->isVisible()) return candidate;
            return nullptr;
        };
        QVERIFY(button("Change") && button("Open") && button("Clear History"));

        // The size is measured on demand.
        auto *size_label = dialog.attr<QLabel>("_clipboard_size_label");
        auto *refresh = qobject_cast<QToolButton *>(dialog.attr_widget("_clipboard_refresh_btn"));
        QVERIFY(size_label && refresh);
        refresh->click();
        QTRY_VERIFY(size_label->text().contains("B"));

        // Opening shows the data folder.
        button("Open")->click();
        QTRY_COMPARE(urls_.urls.size(), 1);
        QCOMPARE(QDir::cleanPath(urls_.urls.first().toLocalFile()), QDir::cleanPath(env_.db_folder()));

        // The same folder, and a folder inside the images folder, are refused.
        {
            FileDialogAnswerer same(env_.db_folder());
            ModalAnswerer ok(true);
            button("Change")->click();
            QVERIFY(same.answered);
            QCOMPARE(ok.answered(), 1);
            QVERIFY(ok.texts.first().contains("already the current location"));
        }
        const QString images = ClipboardManager::instance().get_images_dir().value_or(QString());
        QDir().mkpath(QDir(images).filePath("inner"));
        {
            FileDialogAnswerer inside(QDir(images).filePath("inner"));
            ModalAnswerer ok(true);
            button("Change")->click();
            QCOMPARE(ok.answered(), 1);
            QVERIFY(ok.texts.first().contains("outside"));
        }

        // Moving copies the database, reopens it there and removes the old files.
        const QString old_db = ClipboardManager::instance().get_db_path().value_or(QString());
        const QString target = QDir::toNativeSeparators(env_.dir.filePath("moved"));
        // A repeated run (check.ps1 -Mode Leak) would find the first run's database there and be asked to replace it.
        QVERIFY(QDir(target).removeRecursively());
        QDir().mkpath(target);
        {
            FileDialogAnswerer move(target);
            button("Change")->click();
            QVERIFY(move.answered);
        }
        const QString new_db = QDir::toNativeSeparators(QDir(target).filePath("clipboard.db"));
        QTRY_COMPARE_WITH_TIMEOUT(env_.config->get_clipboard_db_path(), new_db, 10000);
        QTRY_COMPARE(ClipboardManager::instance().get_db_path().value_or(QString()), new_db);
        QVERIFY(!QFileInfo::exists(old_db));
        QCOMPARE(contents_of(ClipboardManager::instance().get_history()).size(), 2);
        QCOMPARE(dialog.attr<SettingCard>("_clipboard_storage_card")->contentLabel->text(), new_db);

        // A folder holding clipboard data asks before replacing it; No keeps everything.
        const QString occupied = QDir::toNativeSeparators(env_.dir.filePath("occupied"));
        QDir().mkpath(occupied);
        QFile stale(QDir(occupied).filePath("clipboard.db"));
        QVERIFY(stale.open(QIODevice::WriteOnly));
        stale.close();
        {
            FileDialogAnswerer pick(occupied);
            ModalAnswerer no(false);
            button("Change")->click();
            QCOMPARE(no.answered(), 1);
        }
        QCOMPARE(ClipboardManager::instance().get_db_path().value_or(QString()), new_db);

        // Clearing asks, and may keep the grouped items.
        {
            ModalAnswerer yes(true);
            button("Clear History")->click();
            QTRY_COMPARE(yes.answered(), 2);
        }
        QCOMPARE(contents_of(ClipboardManager::instance().get_history()), QStringList{"grouped"});
        {
            ModalAnswerer yes(true, true);
            button("Clear History")->click();
            QTRY_COMPARE(yes.answered(), 2);
        }
        QVERIFY(ClipboardManager::instance().get_history().isEmpty());
        {
            ModalAnswerer no(false);
            button("Clear History")->click();
            QCOMPARE(no.answered(), 1);
        }
        dialog.hide();
    }
};

QTEST_MAIN(ClipboardWidgetTests)
#include "test_clipboard_widgets.moc"
