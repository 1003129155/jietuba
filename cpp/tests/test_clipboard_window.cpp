#include "clipboard_test_support.h"
#include "clipboard/controllers/clipboard_controller.h"
#include "clipboard/ui/dialogs/manage_dialog.h"
#include "clipboard/ui/theme/themes.h"
#include "clipboard/ui/widgets/group_bar.h"
#include "clipboard/ui/widgets/item_delegate.h"
#include "clipboard/ui/widgets/quick_edit_popup.h"
#include "core/i18n.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/inputs.h"
#include <QDateEdit>
#include <QDesktopServices>
#include <QDrag>
#include <QFile>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTest>
#include <QToolButton>
#include <algorithm>
#include <memory>
#include <cstring>
#include <windows.h>
#include <shlobj.h>

using namespace clipboard_test;

// 手势测试只记录拖动请求，避免在离屏 CI 中进入系统拖放循环。
class DragRecorderWindow : public ClipboardWindow {
public:
    QList<qint64> dragged;
    void start_item_drag(qint64 item_id) override { dragged << item_id; }
};

// Records the URLs the window asks the desktop to open.
class UrlCatcher : public QObject {
    Q_OBJECT
public:
    QList<QUrl> urls;
public slots:
    void open(const QUrl &url) { urls << url; }
};

// The clipboard window on a real database, offscreen: what a click, a key or a menu entry
// does to the list, the database and the paste.
class ClipboardWindowTests : public QObject {
    Q_OBJECT
    Environment env_;
    std::unique_ptr<ClipboardWindow> window_;
    UrlCatcher urls_;

    ClipboardManager &manager() { return ClipboardManager::instance(); }

    ClipboardWindow *open_window() {
        window_ = std::make_unique<ClipboardWindow>();
        window_->show();
        QApplication::processEvents();
        return window_.get();
    }

    QList<qint64> row_ids() const {
        QList<qint64> ids;
        for (int row = 0; row < window_->list_widget->count(); ++row) ids << window_->list_widget->item(row)->data(Qt::UserRole).toLongLong();
        return ids;
    }

    int row_of(qint64 item_id) const { return static_cast<int>(row_ids().indexOf(item_id)); }

    QPoint row_center(qint64 item_id) const {
        return window_->list_widget->visualItemRect(window_->list_widget->item(row_of(item_id))).center();
    }

    void click(qint64 item_id, Qt::MouseButton button = Qt::LeftButton, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QTest::mouseClick(window_->list_widget->viewport(), button, modifiers, row_center(item_id));
    }

    bool key(int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        ClipboardShortcutHandler handler(window_.get());
        QKeyEvent event(QEvent::KeyPress, key, modifiers);
        return handler.handle_key(&event);
    }

    // Picks an entry of the gear menu; the menu ignores a click right after it closed.
    bool gear_pick(const QString &entry) {
        QTest::qWait(350);
        MenuPicker picker(entry);
        window_->menu_btn->click();
        if (!QTest::qWaitFor([&picker] { return picker.shown; }, 2000)) return false;
        return picker.picked;
    }

    void right_click_pick(qint64 item_id, const QString &entry, MenuPicker **picker_out = nullptr) {
        auto *picker = new MenuPicker(entry);
        picker->setParent(this);
        emit window_->list_widget->customContextMenuRequested(row_center(item_id));
        QVERIFY(picker->shown);
        if (picker_out) *picker_out = picker;
    }

private slots:
    void initTestCase() {
        QVERIFY(env_.init(this));
        // Links too: an unhandled scheme would open the real browser.
        for (const char *scheme : {"file", "http", "https"}) QDesktopServices::setUrlHandler(scheme, &urls_, "open");
    }

    void init() {
        QVERIFY(env_.fresh_database());
        env_.config->set_clipboard_close_after_paste(false);
        env_.config->set_clipboard_auto_paste(true);
        env_.config->set_clipboard_move_to_top_on_paste(true);
        env_.config->set_clipboard_show_metadata(true);
        env_.config->set_clipboard_display_lines(1);
        env_.config->set_clipboard_preserve_search(false);
        env_.config->set_app_setting("clipboard_paste_with_html", true);
        env_.config->set_clipboard_multi_paste_keep_merged(false);
        env_.config->set_clipboard_group_bar_position("right");
        env_.config->set_inapp_clipboard_pick_mode("both");
        urls_.urls.clear();
    }

    void cleanup() {
        QTest::qWait(80);
        if (window_) window_->close();
        window_.reset();
        if (auto *dialog = get_existing_manage_dialog()) {
            dialog->hide();
            destroy_manage_dialog();
        }
        QApplication::processEvents();
    }

    void cleanupTestCase() {
        for (const char *scheme : {"file", "http", "https"}) QDesktopServices::unsetUrlHandler(scheme);
    }

    void theListShowsTheHistoryAndPaints() {
        const auto ids = add_texts({"first", "second\nwith a second line"});
        QVERIFY(manager().add_item(file_payload({"C:\\docs\\a.txt", "C:\\docs\\b.txt"}), "file"));
        QVERIFY(manager().add_item("[10x10]", "image"));
        QVERIFY(manager().toggle_pin(ids[0]));
        auto *window = open_window();
        QCOMPARE(window->list_widget->count(), 4);
        QCOMPARE(row_ids().first(), ids[0]);
        QVERIFY(!window->grab().isNull());
        // Every look of the rows paints: no metadata, larger font and image sizes, opacity.
        for (bool metadata : {false, true}) {
            env_.config->set_clipboard_show_metadata(metadata);
            window->load_settings();
            QVERIFY(!window->grab().isNull());
        }
    }

    void moreRowsLoadWhenScrolledToTheEnd() {
        QStringList texts;
        for (int i = 0; i < 120; ++i) texts << QString("entry %1").arg(i);
        add_texts(texts);
        auto *window = open_window();
        QTRY_VERIFY(window->list_widget->count() >= 38);
        const int before = window->list_widget->count();
        QScrollBar *bar = window->list_widget->verticalScrollBar();
        QTRY_VERIFY(bar->maximum() > 0);
        bar->setValue(bar->maximum());
        QTRY_VERIFY(window->list_widget->count() > before);
    }

    void aClickPastesAndMovesTheRowUp() {
        const auto ids = add_texts({"older", "newer"});
        auto *window = open_window();
        QSignalSpy pasted(window, &ClipboardWindow::item_pasted);
        click(ids[0]);
        QCOMPARE(pasted.size(), 1);
        QCOMPARE(pasted.first().first().toLongLong(), ids[0]);
        QCOMPARE(row_ids(), (QList<qint64>{ids[0], ids[1]}));
        QTRY_COMPARE(env_.pastes.size(), 1);
        // With close-after-paste on, the window closes.
        env_.config->set_clipboard_close_after_paste(true);
        click(ids[1]);
        QVERIFY(!window->isVisible());
    }

    void textDragExportsUtf8FileWithoutChangingHistory() {
        const QString text = QString::fromUtf8("中文 😀\n第二行\r\n") + QString(100000, u'x');
        const auto id = manager().add_item(text);
        QVERIFY(id);
        const auto item = manager().get_item(*id);
        QVERIFY(item);
        auto mime = clipboard_drag_mime(manager(), *item);
        QVERIFY(mime);
        const auto bytes = mime->data("application/x-qt-windows-mime;value=\"FileGroupDescriptorW\"");
        QCOMPARE(bytes.size(), qsizetype(sizeof(FILEGROUPDESCRIPTORW)));
        FILEGROUPDESCRIPTORW descriptor{};
        std::memcpy(&descriptor, bytes.constData(), sizeof(descriptor));
        QCOMPARE(descriptor.cItems, 1u);
        QVERIFY(descriptor.fgd[0].dwFlags & FD_FILESIZE);
        QCOMPARE(descriptor.fgd[0].nFileSizeLow, static_cast<DWORD>(text.toUtf8().size()));
        QCOMPARE(descriptor.fgd[0].nFileSizeHigh, 0u);
        QVERIFY(QString::fromWCharArray(descriptor.fgd[0].cFileName).endsWith(".txt"));
        QVERIFY(!mime->hasUrls()); // 不创建临时文件，也不暴露数据库目录。
        QCOMPARE(mime->data("application/x-qt-windows-mime;value=\"FileContents\";index=0"), text.toUtf8());
        QCOMPARE(manager().get_item(*id)->paste_count, item->paste_count);
        QCOMPARE(manager().get_item(*id)->content, text);
        // 每次拖拽保留自己的快照，后续编辑不能覆盖仍在传输的内容。
        QVERIFY(manager().update_item(*id, "changed"));
        auto later = clipboard_drag_mime(manager(), *manager().get_item(*id));
        QCOMPARE(later->data("application/x-qt-windows-mime;value=\"FileContents\";index=0"), QByteArray("changed"));
        QCOMPARE(mime->data("application/x-qt-windows-mime;value=\"FileContents\";index=0"), text.toUtf8());
    }

    void imageDragKeepsOriginalPngBytesAndRejectsMissingData() {
        ClipboardItem item;
        item.id = 42;
        item.content_type = "image";
        QVERIFY(!clipboard_drag_mime(manager(), item));
        item.image_id = "drag-image";
        QVERIFY(!clipboard_drag_mime(manager(), item));
        const auto folder = manager().get_images_dir();
        QVERIFY(folder);
        const QString path = QDir(*folder).filePath(*item.image_id + ".png");
        QImage image(32, 24, QImage::Format_ARGB32);
        image.fill(QColor(12, 34, 56, 78));
        QVERIFY(image.save(path));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto original = file.readAll();
        file.close();
        auto mime = clipboard_drag_mime(manager(), item);
        QVERIFY(mime);
        QCOMPARE(mime->data("application/x-qt-windows-mime;value=\"FileContents\";index=0"), original);
        const auto bytes = mime->data("application/x-qt-windows-mime;value=\"FileGroupDescriptorW\"");
        FILEGROUPDESCRIPTORW descriptor{};
        QCOMPARE(bytes.size(), qsizetype(sizeof(descriptor)));
        std::memcpy(&descriptor, bytes.constData(), sizeof(descriptor));
        QCOMPARE(QString::fromWCharArray(descriptor.fgd[0].cFileName), QString("clipboard_image_42.png"));
        QCOMPARE(descriptor.fgd[0].nFileSizeLow, static_cast<DWORD>(original.size()));
        // Shell 取走的是快照；删除历史或结束拖动都不会改变原图。
        mime.reset();
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), original);
        file.close();
        QVERIFY(QFile::remove(path));
        QVERIFY(!clipboard_drag_mime(manager(), item));
        item.content_type = "file";
        QVERIFY(!clipboard_drag_mime(manager(), item));
    }

    void rejectedDragReleasesDataWithoutPasting() {
        // 离屏平台不接受系统拖放；这里只验证返回后的对象和业务状态。
        if (QGuiApplication::platformName() != "offscreen") QSKIP("Requires the offscreen drag platform");
        const QString text = QString::fromUtf8("取消拖动 😀\n保留原内容");
        const auto ids = add_texts({text});
        auto *window = open_window();
        window->start_item_drag(-1); // 已不存在的记录不能启动拖动。
        QVERIFY(window->findChildren<QDrag *>().isEmpty());
        for (int i = 0; i < 2; ++i) {
            window->start_item_drag(ids[0]);
            const auto drags = window->findChildren<QDrag *>();
            QVERIFY(drags.size() <= 1);
            QList<QPointer<QMimeData>> payloads;
            for (auto *drag : drags) {
                auto *mime = const_cast<QMimeData *>(drag->mimeData());
                QVERIFY(mime);
                QCOMPARE(mime->data("application/x-qt-windows-mime;value=\"FileContents\";index=0"), text.toUtf8());
                payloads.append(mime);
            }
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QVERIFY(window->findChildren<QDrag *>().isEmpty());
            for (const auto &payload : payloads) QVERIFY(!payload);
        }
        QVERIFY(env_.pastes.isEmpty());
        QCOMPARE(manager().get_item(ids[0])->content, text);
        QCOMPARE(manager().get_item(ids[0])->paste_count, 0);
    }

    void draggingCancelsClicksAndUsesThePressedRow() {
        const auto ids = add_texts({"first", "second"});
        auto window = std::make_unique<DragRecorderWindow>();
        auto *recorded = window.get();
        window_ = std::move(window);
        window_->show();
        QApplication::processEvents();
        auto *viewport = window_->list_widget->viewport();
        const QPoint start = row_center(ids[0]);
        const QPoint end = row_center(ids[1]) + QPoint(QApplication::startDragDistance() + 1, 0);
        auto move = [&](const QPoint &pos, Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
            QMouseEvent event(QEvent::MouseMove, pos, viewport->mapToGlobal(pos), Qt::NoButton, buttons, modifiers);
            QApplication::sendEvent(viewport, &event);
        };
        QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, start);
        move(start + QPoint(1, 0), Qt::LeftButton);
        QVERIFY(recorded->dragged.isEmpty());
        move(end, Qt::LeftButton);
        QCOMPARE(recorded->dragged, QList<qint64>{ids[0]});
        move(start, Qt::LeftButton); // 一次按住只启动一次拖动。
        QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, start);
        QCOMPARE(recorded->dragged.size(), 1);
        QVERIFY(env_.pastes.isEmpty());
        QCOMPARE(manager().get_item(ids[0])->paste_count, 0);

        for (const auto modifiers : {Qt::ControlModifier, Qt::ShiftModifier}) {
            QTest::mousePress(viewport, Qt::LeftButton, modifiers, start);
            move(end, Qt::LeftButton, modifiers);
            QTest::mouseRelease(viewport, Qt::LeftButton, modifiers, end);
        }
        QTest::mousePress(viewport, Qt::RightButton, Qt::NoModifier, start);
        move(end, Qt::RightButton);
        QTest::mouseRelease(viewport, Qt::RightButton, Qt::NoModifier, end);
        QCOMPARE(recorded->dragged.size(), 1);
        QVERIFY(env_.pastes.isEmpty());
        QVERIFY(!window_->selection_manager->has_marks());

        // 上一次单击在等双击时开始拖动，也不能留下延迟粘贴。
        env_.config->set_app_setting("mouse_clipboard_quick_edit", "doubleleft");
        click(ids[0]);
        QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, start);
        move(end, Qt::LeftButton);
        QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, end);
        QTest::qWait(QApplication::doubleClickInterval() + 20);
        QCOMPARE(recorded->dragged.size(), 2);
        QVERIFY(env_.pastes.isEmpty());
        env_.config->set_app_setting("mouse_clipboard_quick_edit", "middle");
    }

    void ctrlAndShiftClicksSelectAndAClickOnTheSelectionPastesItAll() {
        const auto ids = add_texts({"a", "b", "c", "d"});
        auto *window = open_window();
        click(ids[3], Qt::LeftButton, Qt::ControlModifier);
        click(ids[1], Qt::LeftButton, Qt::ControlModifier);
        QCOMPARE(window->selection_manager->marked_ids(true), (QList<qint64>{ids[3], ids[1]}));
        QVERIFY(window->mark_count_label->isVisible());
        QVERIFY(window->mark_count_label->text().contains("2"));
        QVERIFY(env_.pastes.isEmpty());
        // Shift extends from the row selected before the click.
        window->selection_manager->clear_marks();
        window->selection_manager->select_item_id(ids[3]);
        click(ids[1], Qt::LeftButton, Qt::ShiftModifier);
        QCOMPARE(window->selection_manager->marked_ids(), (QList<qint64>{ids[3], ids[2], ids[1]}));
        // A click on a selected row pastes the whole selection and clears it.
        click(ids[2]);
        QTRY_COMPARE(env_.pastes.size(), 1);
        QVERIFY(!window->selection_manager->has_marks());
        QVERIFY(!window->mark_count_label->isVisible());
        // A click elsewhere pastes only that row and drops the selection.
        window->toggle_item_mark(ids[0]);
        window->toggle_item_mark(ids[1]);
        click(ids[3]);
        QVERIFY(!window->selection_manager->has_marks());
        // A Ctrl+click bound to an action runs that action instead of selecting.
        env_.config->set_app_setting("mouse_clipboard_menu", "ctrl+left");
        MenuPicker picker;
        click(ids[0], Qt::LeftButton, Qt::ControlModifier);
        QVERIFY(picker.shown);
        QVERIFY(!window->selection_manager->has_marks());
        env_.config->set_app_setting("mouse_clipboard_menu", "right");
    }

    void keysPasteSelectEditAndClose() {
        const auto ids = add_texts({"one", "two", "three"});
        auto *window = open_window();
        // Direct pick keys paste the n-th row; with Ctrl they are left alone.
        QVERIFY(key(Qt::Key_2));
        QTRY_COMPARE(env_.pastes.size(), 1);
        QVERIFY(!key(Qt::Key_2, Qt::ControlModifier));
        QVERIFY(!key(Qt::Key_9));
        // Enter pastes the current row, Shift+Enter as plain text.
        window->selection_manager->select_item_id(ids[0]);
        QVERIFY(key(Qt::Key_Return));
        QTRY_COMPARE(env_.pastes.size(), 2);
        window->selection_manager->select_item_id(ids[0]);
        QVERIFY(key(Qt::Key_Return, Qt::ShiftModifier));
        QTRY_COMPARE(env_.pastes.size(), 3);
        // Enter with a selection of several pastes them together.
        window->toggle_item_mark(ids[0]);
        window->toggle_item_mark(ids[1]);
        QVERIFY(key(Qt::Key_Enter));
        QTRY_COMPARE(env_.pastes.size(), 4);
        // Delete never removes; Ctrl+F goes to the search box.
        QVERIFY(key(Qt::Key_Delete));
        QCOMPARE(window->list_widget->count(), 3);
        QVERIFY(key(Qt::Key_F, Qt::ControlModifier));
        // Esc: first the selection, then the current row, then the window.
        window->toggle_item_mark(ids[2]);
        QVERIFY(key(Qt::Key_Escape));
        QVERIFY(!window->selection_manager->has_marks());
        window->selection_manager->select_item_id(ids[2]);
        QVERIFY(key(Qt::Key_Escape));
        QVERIFY(!window->selection_manager->has_selection());
        QVERIFY(key(Qt::Key_Escape));
        QVERIFY(!window->isVisible());
    }

    void quickEditSavesAndPastes() {
        const auto ids = add_texts({"original"});
        auto *window = open_window();
        // The quick edit key opens the editor on the current text row.
        window->selection_manager->select_item_id(ids[0]);
        QVERIFY(key(Qt::Key_Tab));
        QVERIFY(window->quick_edit_popup && window->quick_edit_popup->is_open());
        // Keys belong to the editor while it is open.
        QVERIFY(!key(Qt::Key_Escape));
        window->quick_edit_popup->editor->setPlainText("edited");
        window->quick_edit_popup->commit();
        QVERIFY(!window->quick_edit_popup->is_open());
        QCOMPARE(manager().get_item(ids[0])->content, QString("edited"));
        QCOMPARE(window->get_item_data(ids[0])->content, QString("edited"));
        // Middle click edits text; save and paste writes and pastes.
        click(ids[0], Qt::MiddleButton);
        QVERIFY(window->quick_edit_popup->is_open());
        window->quick_edit_popup->editor->setPlainText("edited twice");
        window->quick_edit_popup->commit(true);
        QCOMPARE(manager().get_item(ids[0])->content, QString("edited twice"));
        QTRY_COMPARE(env_.pastes.size(), 1);
        // Cancelling changes nothing.
        window->quick_edit_item(ids[0]);
        window->quick_edit_popup->editor->setPlainText("thrown away");
        window->quick_edit_popup->cancel();
        QCOMPARE(manager().get_item(ids[0])->content, QString("edited twice"));
        // Files are not quick-editable; in the history nothing else opens either.
        const auto file = manager().add_item(file_payload({"C:\\x.txt"}), "file");
        window->notify_new_content(manager().get_item(*file));
        window->selection_manager->select_item_id(*file);
        QVERIFY(!window->edit_selected());
    }

    void theItemMenuRunsItsEntries() {
        const auto group = manager().create_group("Work");
        const auto ids = add_texts({"lower text", "second"});
        auto *window = open_window();

        MenuPicker *picker = nullptr;
        right_click_pick(ids[0], "Pin", &picker);
        QVERIFY(picker->seen.contains("Paste"));
        QVERIFY(picker->seen.contains("Special Paste"));
        QVERIFY(picker->seen.contains("Quick Edit"));
        QVERIFY(manager().get_item(ids[0])->is_pinned);
        right_click_pick(ids[0], "Unpin");
        QVERIFY(!manager().get_item(ids[0])->is_pinned);

        // Special paste and explicit paste press keys even with auto-paste off.
        window->controller->auto_paste_enabled = false;
        right_click_pick(ids[0], "All Uppercase");
        QTRY_COMPARE(env_.pastes.size(), 1);
        right_click_pick(ids[0], "Paste");
        QTRY_COMPARE(env_.pastes.size(), 2);

        right_click_pick(ids[0], "Quick Edit");
        QVERIFY(window->quick_edit_popup && window->quick_edit_popup->is_open());
        window->quick_edit_popup->cancel();

        right_click_pick(ids[0], "Work");
        QCOMPARE(manager().get_by_group(*group).size(), 1);

        // Delete asks first; No keeps the row.
        {
            ModalAnswerer no(false);
            right_click_pick(ids[1], "Delete");
            QCOMPARE(no.answered(), 1);
            QVERIFY(manager().get_item(ids[1]));
        }
        {
            ModalAnswerer yes(true);
            right_click_pick(ids[1], "Delete");
            QVERIFY(!manager().get_item(ids[1]));
            QVERIFY(!row_ids().contains(ids[1]));
        }
    }

    void groupRowsCanBeReorderedEditedAndRemoved() {
        const auto group = manager().create_group("Work");
        const auto ids = add_texts({"a", "b"});
        for (qint64 id : ids) QVERIFY(manager().move_to_group(id, *group));
        auto *window = open_window();
        QTRY_VERIFY(!window->group_bar->group_buttons.isEmpty());
        window->group_bar->group_buttons.first()->click();
        QCOMPARE(window->controller->current_group_id, std::optional<qint64>(*group));
        QCOMPARE(row_ids(), ids);
        right_click_pick(ids[0], "Move Down");
        QCOMPARE(row_ids(), (QList<qint64>{ids[1], ids[0]}));
        right_click_pick(ids[0], "Move Up");
        QCOMPARE(row_ids(), ids);
        right_click_pick(ids[1], "Edit");
        auto *dialog = get_existing_manage_dialog();
        QVERIFY(dialog);
        QCOMPARE(dialog->editing_item_id, std::optional<qint64>(ids[1]));
        // In a group, the edit key opens the editor for rows quick edit does not take.
        right_click_pick(ids[1], "Remove from Group");
        QCOMPARE(row_ids(), QList<qint64>{ids[0]});
        QVERIFY(!manager().get_item(ids[1])->content.isEmpty());
        // Back to the history.
        window->group_bar->clipboard_btn->click();
        QVERIFY(!window->controller->current_group_id);
    }

    void theSelectionMenuActsOnAllSelectedRows() {
        const auto group = manager().create_group("Bin");
        const auto ids = add_texts({"x", "y", "z"});
        auto *window = open_window();
        window->toggle_item_mark(ids[0]);
        window->toggle_item_mark(ids[1]);
        MenuPicker *picker = nullptr;
        right_click_pick(ids[1], "Clear Selection", &picker);
        QVERIFY(picker->seen.contains("Paste Selected"));
        QVERIFY(picker->seen.contains("Paste Selected as Plain Text"));
        QVERIFY(picker->seen.contains("Delete Selected"));
        QVERIFY(!window->selection_manager->has_marks());

        window->toggle_item_mark(ids[0]);
        window->toggle_item_mark(ids[1]);
        right_click_pick(ids[0], "Paste Selected as Plain Text");
        QTRY_COMPARE(env_.pastes.size(), 1);

        window->toggle_item_mark(ids[0]);
        window->toggle_item_mark(ids[1]);
        right_click_pick(ids[0], "Bin");
        QCOMPARE(manager().get_by_group(*group).size(), 2);

        window->toggle_item_mark(ids[0]);
        window->toggle_item_mark(ids[2]);
        ModalAnswerer yes(true);
        right_click_pick(ids[2], "Delete Selected");
        QCOMPARE(yes.answered(), 1);
        QVERIFY(yes.texts.first().contains("2"));
        QCOMPARE(row_ids(), QList<qint64>{ids[1]});
    }

    void tooLargeAStitchIsRefusedBeforePasting() {
        const auto a = manager().add_item("[40000x30000]", "image", QString("a"));
        const auto b = manager().add_item("[40000x30000]", "image", QString("b"));
        auto *window = open_window();
        window->toggle_item_mark(*a);
        window->toggle_item_mark(*b);
        ModalAnswerer ok(true);
        window->paste_selected();
        QCOMPARE(ok.answered(), 1);
        QVERIFY(ok.texts.first().contains("too large"));
        QTest::qWait(80);
        QVERIFY(env_.pastes.isEmpty());
        // Pin and middle click on an image without stored pixels does nothing.
        click(*a, Qt::MiddleButton);
        window->create_pin_window(*a);
    }

    void newContentArrivesWithoutAReload() {
        const auto ids = add_texts({"a"});
        auto *window = open_window();
        window->selection_manager->select_item_id(ids[0]);
        const auto b = manager().add_item("b");
        window->notify_new_content(manager().get_item(*b));
        QCOMPARE(row_ids(), (QList<qint64>{*b, ids[0]}));
        // The selection stays on its item.
        QCOMPARE(window->selection_manager->get_current_item_id(), std::optional<qint64>(ids[0]));
        // A hidden window refreshes on its next show instead.
        window->hide();
        const auto c = manager().add_item("c");
        window->notify_new_content(manager().get_item(*c));
        window->request_data_refresh("test");
        window->show();
        QTRY_COMPARE(row_ids().first(), *c);
        // Visible: a refresh request reloads.
        QVERIFY(manager().delete_item(*c));
        window->request_data_refresh();
        QCOMPARE(row_ids().first(), *b);
        window->on_manage_data_changed();
    }

    void searchTypeAndTimeFilters() {
        add_texts({"apple", "banana"});
        QVERIFY(manager().add_item(file_payload({"C:\\f.txt"}), "file"));
        auto *window = open_window();
        QCOMPARE(window->list_widget->count(), 3);
        window->search_input->setText("apple");
        QTRY_COMPARE(window->list_widget->count(), 1);
        QVERIFY(window->clear_search_btn->isVisible());
        window->clear_search_btn->click();
        QTRY_COMPARE(window->list_widget->count(), 3);

        window->time_filter_toggle_btn->click();
        QVERIFY(window->time_filter_bar->isVisible());
        {
            MenuPicker picker("File");
            window->type_filter_btn->click();
            QVERIFY(picker.picked);
        }
        QCOMPARE(window->list_widget->count(), 1);
        QCOMPARE(window->type_filter_btn->text(), QString("File"));
        {
            MenuPicker picker("Link");
            window->type_filter_btn->click();
            QVERIFY(picker.picked);
        }
        QCOMPARE(window->type_filter_btn->text(), QString("Link"));
        {
            MenuPicker picker("File");
            window->type_filter_btn->click();
            QVERIFY(picker.picked);
        }
        window->start_date_edit->setDate(QDate(2000, 1, 1));
        window->end_date_edit->setDate(QDate(2000, 1, 2));
        window->apply_time_filter_btn->click();
        QCOMPARE(window->list_widget->count(), 0);
        // An end before the start is marked and not applied.
        window->start_date_edit->setDate(QDate(2001, 1, 1));
        window->apply_time_filter_btn->click();
        QVERIFY(window->start_date_edit->property("hasError").toBool());
        // Closing the bar drops both filters.
        window->clear_time_filter_btn->click();
        QVERIFY(!window->time_filter_bar->isVisible());
        QCOMPARE(window->list_widget->count(), 3);
        // The search is dropped on hide unless kept.
        window->search_input->setText("banana");
        window->hide();
        QVERIFY(window->search_input->text().isEmpty());
        env_.config->set_clipboard_preserve_search(true);
        window->show();
        window->search_input->setText("banana");
        window->hide();
        QCOMPARE(window->search_input->text(), QString("banana"));
    }

    void theGearMenuChangesTheWindow() {
        add_texts({"a", "b"});
        auto *window = open_window();
        auto pick = [this](const QString &entry) { return gear_pick(entry); };
        QVERIFY(pick("Show Time and Source"));
        QVERIFY(!env_.config->get_clipboard_show_metadata());
        QVERIFY(pick("Close After Paste"));
        QVERIFY(env_.config->get_clipboard_close_after_paste());
        QVERIFY(pick("Auto Paste After Selection"));
        QVERIFY(!env_.config->get_clipboard_auto_paste());
        QVERIFY(pick("Move to Top After Paste"));
        QVERIFY(!env_.config->get_clipboard_move_to_top_on_paste());
        QVERIFY(pick("Preserve Search on Reopen"));
        QVERIFY(env_.config->get_clipboard_preserve_search());
        QVERIFY(pick("Paste with Format"));
        QVERIFY(!window->controller->paste_with_html);
        QVERIFY(pick("Record Content"));
        QVERIFY(env_.config->get_clipboard_multi_paste_keep_merged());
        QVERIFY(pick("Last Selected First"));
        QCOMPARE(env_.config->get_clipboard_multi_paste_order(), QString("newest_first"));
        QVERIFY(pick("Comma"));
        QCOMPARE(env_.config->get_clipboard_multi_paste_separator(), QString(", "));

        const auto opacity = env_.config->get_clipboard_window_opacity_options().toList();
        QVERIFY(opacity.size() > 1);
        QVERIFY(pick(QString("%1%").arg(opacity[1].toInt())));
        QCOMPARE(env_.config->get_clipboard_window_opacity(), qint64(opacity[1].toInt()));
        QVERIFY(pick("Opaque"));
        const auto fonts = env_.config->get_clipboard_font_size_options().toList();
        QVERIFY(pick(QString("%1px").arg(fonts.first().toInt())));
        QCOMPARE(env_.config->get_clipboard_font_size(), qint64(fonts.first().toInt()));
        QVERIFY(pick("Large"));
        QCOMPARE(env_.config->get_clipboard_image_size(), QString("large"));
        QVERIFY(!window->grab().isNull());

        for (const char *position : {"◀ Left", "▲ Top", "▶ Right"}) {
            QVERIFY(pick(QString::fromUtf8(position)));
            QVERIFY(!window->grab().isNull());
        }
        QCOMPARE(env_.config->get_clipboard_group_bar_position(), QString("right"));

        QVERIFY(pick("Add Content"));
        QVERIFY(get_existing_manage_dialog());
        QVERIFY(get_existing_manage_dialog()->isVisible());

        // A second click on the button closes the open menu.
        QTest::qWait(350);
        window->menu_btn->click();
        QTRY_VERIFY(QApplication::activePopupWidget());
        window->menu_btn->click();
        QTRY_VERIFY(!QApplication::activePopupWidget());
    }

    void theGearMenuClearsLooseItemsAfterAsking() {
        const auto group = manager().create_group("Work");
        const auto ids = add_texts({"loose", "grouped", "pinned"});
        QVERIFY(manager().move_to_group(ids[1], *group));
        QVERIFY(manager().toggle_pin(ids[2]));
        open_window();
        QCOMPARE(row_ids().size(), 3);
        {
            ModalAnswerer no(false);
            QVERIFY(gear_pick("Clear History"));
            QCOMPARE(no.answered(), 1);
        }
        QVERIFY(manager().get_item(ids[0]));
        {
            ModalAnswerer yes(true);
            QVERIFY(gear_pick("Clear History"));
            QCOMPARE(yes.answered(), 1);
        }
        QVERIFY(!manager().get_item(ids[0]));
        QCOMPARE(row_ids(), (QList<qint64>{ids[2], ids[1]}));
    }

    void theGroupBarSwitchesGroupsByMouseAndKeyboard() {
        const auto work = manager().create_group("Work", std::nullopt, QString::fromUtf8("💼"));
        const auto apps = manager().create_group("Apps", std::nullopt, std::nullopt, static_cast<int>(GroupType::FILE));
        const auto ids = add_texts({"in work", "loose"});
        QVERIFY(manager().move_to_group(ids[0], *work));
        auto *window = open_window();
        QTRY_COMPARE(window->group_bar->group_buttons.size(), 2);
        QVERIFY(!window->grab().isNull());

        // Right in the list enters the side bar, arrows walk it, Left leaves it.
        window->selection_manager->select_item_id(ids[1]);
        QTest::keyClick(window->list_widget, Qt::Key_Right);
        QVERIFY(window->group_bar->sidebar_mode());
        QTest::keyClick(window->list_widget, Qt::Key_Down);
        QTest::keyClick(window->list_widget, Qt::Key_Down);
        QTest::keyClick(window->list_widget, Qt::Key_Up);
        QTest::keyClick(window->list_widget, Qt::Key_Left);
        QVERIFY(!window->group_bar->sidebar_mode());

        // The top layout switches groups with left and right.
        window->group_bar->switch_to_group(std::nullopt);
        QVERIFY(gear_pick(QString::fromUtf8("▲ Top")));
        QTest::keyClick(window->list_widget, Qt::Key_Right);
        QCOMPARE(window->controller->current_group_id, std::optional<qint64>(*work));
        QTest::keyClick(window->list_widget, Qt::Key_Left);
        QVERIFY(!window->controller->current_group_id);
        QVERIFY(gear_pick(QString::fromUtf8("▶ Right")));
        QTRY_COMPARE(window->group_bar->group_buttons.size(), 2);

        // The group menu edits, moves and deletes.
        auto group_menu = [window](int index, const QString &entry) {
            MenuPicker picker(entry);
            QPushButton *button = window->group_bar->group_buttons[index];
            emit button->customContextMenuRequested(QPoint(1, 1));
            return picker.picked;
        };
        QVERIFY(group_menu(0, "Move Down"));
        QCOMPARE(manager().get_groups().first().id, *apps);
        QVERIFY(group_menu(1, "Move Up"));
        QCOMPARE(manager().get_groups().first().id, *work);
        QVERIFY(group_menu(0, "Edit"));
        QVERIFY(get_existing_manage_dialog());
        QCOMPARE(get_existing_manage_dialog()->editing_group_id, std::optional<qint64>(*work));
        {
            ModalAnswerer yes(true);
            QVERIFY(group_menu(0, "Delete Group"));
            QCOMPARE(yes.answered(), 1);
        }
        QTRY_COMPARE(window->group_bar->group_buttons.size(), 1);
        // Its item went back to the history.
        QVERIFY(manager().get_item(ids[0]));

        // The add and close buttons.
        window->group_bar->add_group_btn->click();
        QVERIFY(get_existing_manage_dialog()->isVisible());
        window->group_bar->close_btn->click();
        QVERIFY(!window->isVisible());
    }

    void groupsThatDoNotFitGoIntoAMenu() {
        QList<qint64> groups;
        for (int i = 0; i < 20; ++i) groups << manager().create_group(QString("G%1").arg(i)).value_or(-1);
        QString last_label;
        for (const auto &group : manager().get_groups())
            if (group.id == groups.last()) last_label = (group.icon && !group.icon->isEmpty() ? *group.icon : QString::fromUtf8("📁")) + "  " + group.name;
        auto *window = open_window();
        auto overflow_button = [window]() -> QPushButton * {
            for (auto *button : window->group_bar->findChildren<QPushButton *>())
                if (button->text() == QString::fromUtf8("···") && button->isVisible()) return button;
            return nullptr;
        };
        QTRY_VERIFY(overflow_button());
        QVERIFY(window->group_bar->group_buttons.size() < 20);
        MenuPicker picker(last_label);
        overflow_button()->click();
        QVERIFY(picker.picked);
        QCOMPARE(window->controller->current_group_id, std::optional<qint64>(groups.last()));
        // The chosen group takes the last visible place at once.
        QVERIFY(std::any_of(window->group_bar->group_buttons.begin(), window->group_bar->group_buttons.end(),
                                [&groups](QPushButton *button) { return button->isChecked() && button->property("group_id").toLongLong() == groups.last(); }));
        QVERIFY(!window->grab().isNull());
    }

    void losingFocusHidesOnlyWhenClosingAfterPaste() {
        add_texts({"a"});
        env_.config->set_clipboard_close_after_paste(false);
        auto *window = open_window();
        window->activateWindow();
        QTRY_VERIFY(window->isActiveWindow());
        QWidget other;
        other.show();
        other.activateWindow();
        QTRY_VERIFY(!window->isActiveWindow());
        QTest::qWait(250);
        QVERIFY(window->isVisible());

        env_.config->set_clipboard_close_after_paste(true);
        window->activateWindow();
        QTRY_VERIFY(window->isActiveWindow());
        other.activateWindow();
        QTRY_VERIFY(!window->isVisible());
        // Arrow keys reach the list even when the search box has focus.
        window->show();
        window->search_input->setFocus();
        QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
        QApplication::sendEvent(window, &down);
        QVERIFY(window->selection_manager->has_selection());
    }

    void aFileGroupOpensFilesInsteadOfPasting() {
        const QString path = QDir::toNativeSeparators(env_.dir.filePath("doc.txt"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
        file.close();
        const auto apps = manager().create_group("Apps", std::nullopt, std::nullopt, static_cast<int>(GroupType::FILE));
        const auto first = manager().add_item(file_payload({path}), "file");
        const auto second = manager().add_item(file_payload({QDir::toNativeSeparators(env_.dir.filePath("missing.txt"))}), "file");
        QVERIFY(manager().move_to_group(*first, *apps) && manager().move_to_group(*second, *apps));
        auto *window = open_window();
        QTRY_VERIFY(!window->group_bar->group_buttons.isEmpty());
        window->group_bar->group_buttons.first()->click();
        QCOMPARE(window->list_widget->count(), 2);
        QVERIFY(!window->grab().isNull());
        click(*first);
        QTRY_COMPARE(urls_.urls.size(), 1);
        QCOMPARE(QDir::toNativeSeparators(urls_.urls.first().toLocalFile()), path);
        QVERIFY(env_.pastes.isEmpty());
        // A selection opens each existing file; an explicit paste still pastes.
        window->show();
        window->toggle_item_mark(*first);
        window->toggle_item_mark(*second);
        window->paste_selected();
        QTRY_COMPARE(urls_.urls.size(), 2);
        // Shift+Enter pastes the path as text.
        window->show();
        window->selection_manager->select_item_id(*first);
        QVERIFY(key(Qt::Key_Return, Qt::ShiftModifier));
        QTRY_COMPARE(env_.pastes.size(), 1);
        right_click_pick(*first, "File Name");
        QTRY_COMPARE(env_.pastes.size(), 2);
    }

    void pathAndLinkTextOpenWhatTheyName() {
        const QString path = QDir::toNativeSeparators(env_.dir.filePath("notes.txt"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        const auto apps = manager().create_group("Apps", std::nullopt, std::nullopt, static_cast<int>(GroupType::FILE));
        const auto ids = add_texts({"\"" + path + "\"", "www.example.com"});
        QVERIFY(apps && manager().move_to_group(ids[0], *apps));
        auto *window = open_window();
        QTRY_VERIFY(!window->group_bar->group_buttons.isEmpty());
        right_click_pick(ids[1], "Open in Browser");
        QTRY_COMPARE(urls_.urls.size(), 1);
        QCOMPARE(urls_.urls.first(), QUrl("https://www.example.com"));
        // In a quick launch group a click opens the path instead of pasting it.
        window->show();
        window->group_bar->group_buttons.first()->click();
        QCOMPARE(window->list_widget->count(), 1);
        click(ids[0]);
        QTRY_COMPARE(urls_.urls.size(), 2);
        QCOMPARE(QDir::toNativeSeparators(urls_.urls.last().toLocalFile()), path);
        QVERIFY(env_.pastes.isEmpty());
    }

    void aFileGroupPastesWhatItCannotOpen() {
        const auto apps = manager().create_group("Apps");
        QVERIFY(apps);
        const auto text = add_texts({"plain text"}).first();
        const auto file = manager().add_item(file_payload({QDir::toNativeSeparators(env_.dir.filePath("missing.txt"))}), "file");
        QVERIFY(file && manager().move_to_group(text, *apps) && manager().move_to_group(*file, *apps));
        // The type changes after the text went in: quick launch groups still hold it.
        QVERIFY(manager().update_group(*apps, "Apps", std::nullopt, std::nullopt, static_cast<int>(GroupType::FILE)));
        auto *window = open_window();
        QTRY_VERIFY(!window->group_bar->group_buttons.isEmpty());
        window->group_bar->group_buttons.first()->click();
        QCOMPARE(window->list_widget->count(), 2);
        click(text);
        QTRY_COMPARE(env_.pastes.size(), 1);
        QVERIFY(urls_.urls.isEmpty());
        // A selection with a text in it is pasted as one, nothing is opened.
        window->show();
        window->toggle_item_mark(text);
        window->toggle_item_mark(*file);
        window->paste_selected();
        QTRY_COMPARE(env_.pastes.size(), 2);
        QVERIFY(urls_.urls.isEmpty());
    }

    void quickLaunchOpensLinksByClickEnterAndMultipleSelection() {
        const auto apps = manager().create_group("Launch", std::nullopt, std::nullopt, static_cast<int>(GroupType::FILE));
        QVERIFY(apps);
        const auto ids = add_texts({"https://example.com/page", "www.example.com"});
        for (qint64 id : ids) QVERIFY(manager().move_to_group(id, *apps));
        auto *window = open_window();
        QTRY_VERIFY(!window->group_bar->group_buttons.isEmpty());
        window->group_bar->group_buttons.first()->click();
        click(ids[0]);
        QTRY_COMPARE(urls_.urls.size(), 1);
        QCOMPARE(urls_.urls.last(), QUrl("https://example.com/page"));
        window->show();
        window->selection_manager->select_item_id(ids[1]);
        QVERIFY(key(Qt::Key_Return));
        QTRY_COMPARE(urls_.urls.size(), 2);
        QCOMPARE(urls_.urls.last(), QUrl("https://www.example.com"));
        window->show();
        for (qint64 id : ids) window->toggle_item_mark(id);
        window->paste_selected();
        QTRY_COMPARE(urls_.urls.size(), 4);
        QVERIFY(env_.pastes.isEmpty());
        window->show();
        window->selection_manager->select_item_id(ids[0]);
        QVERIFY(key(Qt::Key_Return, Qt::ShiftModifier));
        QTRY_COMPARE(env_.pastes.size(), 1);
        QCOMPARE(urls_.urls.size(), 4);
    }

    void aFilterGroupFixesTheTypeButton() {
        add_texts({"apple", "banana"});
        QVERIFY(manager().add_item(file_payload({"C:\\f.txt"}), "file"));
        QVERIFY(manager().create_group("Text", std::nullopt, std::nullopt, static_cast<int>(GroupType::FILTER), FilterRule::of_kind("text")));
        auto *window = open_window();
        QTRY_VERIFY(!window->group_bar->group_buttons.isEmpty());
        window->time_filter_toggle_btn->click();
        QVERIFY(window->type_filter_btn->isEnabled());
        window->group_bar->group_buttons.first()->click();
        QCOMPARE(window->list_widget->count(), 2);
        QVERIFY(!window->type_filter_btn->isEnabled());
        QCOMPARE(window->type_filter_btn->text(), QString("Text"));
        // Back in the history the button is the user's again.
        window->group_bar->clipboard_btn->click();
        QCOMPARE(window->list_widget->count(), 3);
        QVERIFY(window->type_filter_btn->isEnabled());
        QCOMPARE(window->type_filter_btn->text(), QString("All"));
        QVERIFY(!window->grab().isNull());
    }

    void themeScaleAndLanguageChangesRestyle() {
        add_texts({"a"});
        auto *window = open_window();
        auto *themes = get_theme_manager();
        const QString original = themes->get_current_theme().name;
        for (const auto &[name, swatch] : PRESET_THEME_SWATCHES()) {
            QVERIFY(themes->set_theme(name));
            QCOMPARE(window->current_theme.name, name);
            QVERIFY(!window->grab().isNull());
        }
        QVERIFY(themes->set_theme(original));
        themes->notify_font_size_changed(20);
        themes->notify_opacity_changed(30);
        QCOMPARE(window->window_opacity, 30);
        themes->notify_opacity_changed(0);
        get_ui_scale().set_percent(150);
        QVERIFY(!window->grab().isNull());
        get_ui_scale().set_percent(100);
        I18nManager::load_language("zh");
        I18nManager::load_language("en");
        QVERIFY(!window->grab().isNull());
    }

    void geometrySurvivesReopening() {
        auto *window = open_window();
        window->resize(500, 650);
        window->close();
        window_.reset();
        QSettings stored(env_.dir.filePath("window.ini"), QSettings::IniFormat);
        QCOMPARE(stored.value("window/width").toInt(), 500);
        auto *again = open_window();
        QCOMPARE(again->width(), 500);
    }

    void displayLineMenuUpdatesRowsAndSurvivesReopening() {
        const auto ids = add_texts({"one\ntwo\nthree\nfour"});
        auto *window = open_window();
        const auto height = [&] { return window_->list_widget->visualItemRect(window_->list_widget->item(row_of(ids[0]))).height(); };
        const int single_height = height();
        const auto font = env_.config->get_clipboard_font_size();
        const auto image = env_.config->get_clipboard_image_size();
        QVERIFY(gear_pick(QCoreApplication::translate("ClipboardWindow", "%n line(s)", nullptr, 3)));
        QCOMPARE(env_.config->get_clipboard_display_lines(), 3);
        QTRY_VERIFY(height() > single_height);
        const int multiline_height = height();
        QCOMPARE(env_.config->get_clipboard_font_size(), font);
        QCOMPARE(env_.config->get_clipboard_image_size(), image);
        window->close();
        window_.reset();
        open_window();
        QCOMPARE(height(), multiline_height);
        QVERIFY(gear_pick(QCoreApplication::translate("ClipboardWindow", "%n line(s)", nullptr, 1)));
        QTRY_COMPARE(height(), single_height);
    }

    void multilineRowsFollowTheAvailableWidthAndSelectionMarks() {
        const auto ids = add_texts({QString("word ").repeated(20)});
        env_.config->set_clipboard_display_lines(5);
        auto *window = open_window();
        window->resize(1100, 600);
        QApplication::processEvents();
        const auto height = [&] { return window->list_widget->visualItemRect(window->list_widget->item(row_of(ids[0]))).height(); };
        const int wide = height();
        window->resize(400, 600);
        QTRY_VERIFY(height() > wide);
        window->toggle_item_mark(ids[0]);
        QVERIFY(window->selection_manager->has_marks());
        QVERIFY(height() > wide);
        QVERIFY(!window->grab().isNull());
    }

    void hoveringShowsThePreview() {
        const auto ids = add_texts({QString("long text ").repeated(40)});
        auto *window = open_window();
        QTest::mouseMove(window->list_widget->viewport(), row_center(ids[0]));
        QTRY_VERIFY(PreviewPopup::instance()->isVisible());
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(window->list_widget->viewport(), &leave);
        QTRY_VERIFY(!PreviewPopup::instance()->isVisible());
    }

    void movingIntoBlankSpaceClearsTheHoverPreview_data() {
        QTest::addColumn<bool>("selected");
        QTest::addColumn<bool>("pending");
        QTest::newRow("visible") << false << false;
        QTest::newRow("visible-with-selection") << true << false;
        QTest::newRow("pending") << false << true;
        QTest::newRow("pending-with-selection") << true << true;
    }

    void movingIntoBlankSpaceClearsTheHoverPreview() {
        QFETCH(bool, selected);
        QFETCH(bool, pending);
        const auto ids = add_texts({"first", "second"});
        auto *window = open_window();
        auto *list = window->list_widget;
        auto *preview = PreviewPopup::instance();
        if (selected) list->setCurrentRow(0);
        const auto selection = list->selectedItems();
        preview->hide_preview();
        const QPoint row_pos = row_center(ids[1]);
        const QPoint blank_pos(row_pos.x(), list->viewport()->height() - 10);
        QVERIFY(!list->itemAt(blank_pos));
        const auto move_mouse = [list](const QPoint &pos) {
            QMouseEvent event(QEvent::MouseMove, QPointF(pos), QPointF(list->viewport()->mapToGlobal(pos)),
                              Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(list->viewport(), &event);
        };
        move_mouse(row_pos);
        if (pending) QVERIFY(!preview->isVisible());
        else QTRY_VERIFY(preview->isVisible());
        QCOMPARE(window->item_delegate->highlighted_id(), std::optional<qint64>(ids[1]));

        move_mouse(blank_pos);
        QVERIFY(!preview->isVisible());
        QCOMPARE(list->selectedItems(), selection);
        const auto expected_highlight = selection.isEmpty() ? std::nullopt
            : std::optional<qint64>(selection.front()->data(Qt::UserRole).toLongLong());
        QCOMPARE(window->item_delegate->highlighted_id(), expected_highlight);
        QTest::qWait(30);
        QVERIFY(!preview->isVisible());

        move_mouse(row_pos);
        QTRY_VERIFY(preview->isVisible());
        QCOMPARE(window->item_delegate->highlighted_id(), std::optional<qint64>(ids[1]));
    }
};

QTEST_MAIN(ClipboardWindowTests)
#include "test_clipboard_window.moc"
