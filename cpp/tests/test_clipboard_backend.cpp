#include "clipboard_test_support.h"
#include "clipboard/controllers/clipboard_controller.h"
#include "clipboard/ui/dialogs/manage_dialog.h"
#include "clipboard/ui/windows/pin_window.h"
#include "core/clipboard_utils.h"
#include "core/constants.h"
#include "core/save.h"
#include "clipboard/ui/widgets/reorder_list.h"
#include "pin/pin_manager.h"
#include "pin/pin_window.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/segmented.h"
#include <QClipboard>
#include <QFile>
#include <QMimeData>
#include <QMutex>
#include <QSignalSpy>
#include <QTest>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdlib>
#include <memory>

using namespace clipboard_test;

namespace {
QImage solid(const QSize &size, const QColor &color) {
    QImage image(size, QImage::Format_RGB32);
    image.fill(color);
    return image;
}

std::optional<ClipboardItem> find_item(const QString &content_type, const std::function<bool(const ClipboardItem &)> &match = {}) {
    for (const auto &item : ClipboardManager::instance().get_history(0, 100))
        if (item.content_type == content_type && (!match || match(item))) return item;
    return std::nullopt;
}

// Read through Win32: under the offscreen platform QClipboard is a private one, not the system's.
QSize system_clipboard_image_size() {
    QSize size;
    if (!OpenClipboard(nullptr)) return size;
    if (HANDLE data = GetClipboardData(CF_DIB)) {
        if (const auto *header = static_cast<const BITMAPINFOHEADER *>(GlobalLock(data))) {
            size = QSize(header->biWidth, std::abs(header->biHeight));
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return size;
}
}

// The backend over a real database, and the watcher reading the real system clipboard:
// copied text, images and files are recorded, images keep their pixels for pins, previews,
// stitching and the manage window. Writes the system clipboard; needs no visible desktop.
class ClipboardBackendTests : public QObject {
    Q_OBJECT
    Environment env_;
    QMutex lock_;
    QList<ClipboardItem> received_;

    ClipboardManager &manager() { return ClipboardManager::instance(); }

    void start_watching() {
        manager().start_monitoring([this](const ClipboardItem &item) {
            QMutexLocker locker(&lock_);
            received_ << item;
        });
        // The watcher thread registers its listener shortly after starting; earlier changes are not seen.
        QTest::qWait(500);
    }

    int received_count() {
        QMutexLocker locker(&lock_);
        return static_cast<int>(received_.size());
    }

    // Copies an image and waits until the watcher records it.
    std::optional<ClipboardItem> record_image(const QImage &image) {
        const QString content = QString("[%1x%2]").arg(image.width()).arg(image.height());
        copy_image_to_clipboard(image);
        wait_for_clipboard_writes(5.0);
        std::optional<ClipboardItem> found;
        const bool recorded = QTest::qWaitFor([&] {
            found = find_item("image", [&content](const ClipboardItem &item) { return item.content == content && item.image_id; });
            return found.has_value();
        }, 10000);
        return recorded ? found : std::nullopt;
    }

private slots:
    void initTestCase() { QVERIFY(env_.init(this)); }

    void init() {
        QVERIFY(env_.fresh_database());
        env_.config->set_clipboard_enabled(true);
        QMutexLocker locker(&lock_);
        received_.clear();
    }

    void cleanup() {
        manager().clear_callback();
        manager().stop_monitoring();
        PinManager::instance()->close_all();
        if (auto *dialog = get_existing_manage_dialog()) {
            dialog->hide();
            destroy_manage_dialog();
        }
        QTest::qWait(80);
    }

    void storageIsReleasedAndReopened() {
        const QString path = manager().get_db_path().value_or(QString());
        QVERIFY(QFileInfo(path).isFile());
        QVERIFY(QFileInfo(manager().get_images_dir().value_or(QString())).isDir());
        // The backend's own default, shared with the Python version; the test sandbox moves %APPDATA%.
        QCOMPARE(ClipboardManager::default_db_path(), QDir::toNativeSeparators(QDir(qEnvironmentVariable("APPDATA")).filePath("pyclipboard/clipboard.db")));
        QVERIFY(ClipboardManager::default_db_path().startsWith(QDir::toNativeSeparators(test_sandbox_dir())));
        QCOMPARE(ClipboardManager::max_stitched_pixels(), quint64(60'000'000));
        QVERIFY(add_texts({"kept"}).first() > 0);
        QVERIFY(manager().release_storage());
        QVERIFY(!manager().is_available());
        QVERIFY(manager().get_history().isEmpty());
        QVERIFY(!manager().get_db_path());
        QVERIFY(manager().reset_storage(path));
        QCOMPARE(contents_of(manager().get_history()), QStringList{"kept"});
    }

    // A save folder that cannot be created (an unplugged drive) still puts the capture on
    // the clipboard, only without its file path, and both parts of the delivery finish.
    void anUnwritableSaveFolderStillCopies() {
        QVERIFY(OpenClipboard(nullptr));
        EmptyClipboard();
        CloseClipboard();
        QFile blocker(env_.dir.filePath("blocker"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        SaveKwargs kwargs;
        kwargs.directory = QDir::toNativeSeparators(env_.dir.filePath("blocker/shots"));
        const auto delivery = deliver_image_async(solid(QSize(12, 9), Qt::darkGreen), true, std::make_shared<SaveService>(*env_.config), kwargs, true);
        QVERIFY(delivery);
        QVERIFY(delivery->join(10.0));
        QVERIFY(wait_for_clipboard_writes(5.0));
        QTRY_COMPARE(system_clipboard_image_size(), QSize(12, 9));
        QVERIFY(!IsClipboardFormatAvailable(CF_HDROP));
    }

    void groupsAndOrderingThroughTheBackend() {
        const auto a = manager().create_group("A"), b = manager().create_group("B");
        QVERIFY(a && b);
        QVERIFY(manager().update_group(*a, "A2", QString("#ff0000"), QString::fromUtf8("⭐"), static_cast<int>(GroupType::FILE)));
        const auto updated = manager().get_groups().first();
        QCOMPARE(updated.name, QString("A2"));
        QCOMPARE(updated.color.value_or(QString()), QString("#ff0000"));
        QCOMPARE(updated.group_type, static_cast<int>(GroupType::FILE));
        QVERIFY(manager().move_group_between(*a, *b, std::nullopt));
        QCOMPARE(manager().get_groups().last().id, *a);
        // A file group refuses text.
        const auto text = add_texts({"text"}).first();
        QVERIFY(!manager().move_to_group(text, *a));
        const auto file = manager().add_item(file_payload({"C:\\x.txt"}), "file");
        QVERIFY(manager().move_to_group(*file, *a));
        // Items outside groups, ordering inside one.
        QCOMPARE(ids_of(manager().get_by_group(std::nullopt)), QList<qint64>{text});
        const auto more = add_texts({"m1", "m2"});
        for (qint64 id : more) QVERIFY(manager().move_to_group(id, *b));
        QVERIFY(manager().move_item_between(more[1], std::nullopt, more[0]));
        QCOMPARE(ids_of(manager().get_by_group(*b)), (QList<qint64>{more[1], more[0]}));
        QCOMPARE(contents_of(manager().search("m2")), QStringList{"m2"});
        QCOMPARE(manager().get_total_count(), qint64(4));
        // Clearing keeps grouped items when asked; deleting a group takes its items.
        QVERIFY(manager().clear_history(true));
        QVERIFY(!manager().get_item(text));
        QVERIFY(manager().get_item(more[0]));
        // Deleting a group returns its items to the history.
        QVERIFY(manager().delete_group(*b));
        QVERIFY(manager().get_item(more[0]));
        QVERIFY(ids_of(manager().get_by_group(std::nullopt)).contains(more[0]));
        QVERIFY(manager().delete_item(*file));
        QVERIFY(!manager().get_item(*file));
        QVERIFY(!manager().get_image_data("no-such-image"));
    }

    void groupTypesDecideWhatAGroupTakesAndBecomes() {
        // A filter group stores nothing; its view is the history of its type.
        const auto images = manager().create_group("Images", std::nullopt, std::nullopt, static_cast<int>(GroupType::FILTER_IMAGE));
        QVERIFY(images);
        const auto filter = manager().get_groups().first();
        QVERIFY(filter.is_filter());
        QCOMPARE(filter.filter_content_type(), std::optional<QString>("image"));
        const auto text = add_texts({"text"}).first();
        const auto file = manager().add_item(file_payload({"C:\\a.txt"}), "file");
        QVERIFY(file);
        QVERIFY(!manager().move_to_group(text, *images));
        QVERIFY(!manager().move_to_group(*file, *images));
        QVERIFY(manager().get_by_group(*images).isEmpty());
        QCOMPARE(ids_of(manager().get_history(0, 50, std::nullopt, filter.filter_content_type())), QList<qint64>{});
        QCOMPARE(ids_of(manager().get_history(0, 50, std::nullopt, QString("text"))), QList<qint64>{text});

        // A group with content cannot become a filter group; emptied, it can; a filter group changes type freely.
        const auto plain = manager().create_group("Plain");
        QVERIFY(plain);
        QVERIFY(manager().move_to_group(text, *plain));
        QVERIFY(!manager().update_group(*plain, "Plain", std::nullopt, std::nullopt, static_cast<int>(GroupType::FILTER_TEXT)));
        QCOMPARE(manager().get_groups().last().group_type, static_cast<int>(GroupType::NORMAL));
        // Quick launch with text inside is allowed: its clicks paste what they cannot open.
        QVERIFY(manager().update_group(*plain, "Plain", std::nullopt, std::nullopt, static_cast<int>(GroupType::FILE)));
        QVERIFY(manager().move_to_group(text, std::nullopt));
        for (const auto type : {GroupType::FILTER_TEXT, GroupType::FILTER_FILE, GroupType::NORMAL, GroupType::FILTER_IMAGE, GroupType::HIDDEN, GroupType::FILE})
            QVERIFY(manager().update_group(*plain, "Plain", std::nullopt, std::nullopt, static_cast<int>(type)));
        QVERIFY(!manager().create_group("Bad", std::nullopt, std::nullopt, 99));
    }

    void theHistoryLimitTrimsOldItems() {
        add_texts({"1", "2", "3", "4", "5"});
        env_.config->set_clipboard_history_limit(2);
        manager().apply_history_limit();
        QVERIFY(manager().get_total_count() <= 2);
        QCOMPARE(manager().get_history().first().content, QString("5"));
        env_.config->set_clipboard_history_limit(1000);
        manager().apply_history_limit();
    }

    void theWatcherRecordsCopiedTextImagesAndFiles() {
        start_watching();
        QVERIFY(manager().is_monitoring());
        // Starting again while running changes nothing.
        start_watching();
        QString error;
        QVERIFY2(ClipboardManager::set_clipboard_text("copied by the test", &error), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(find_item("text", [](const ClipboardItem &item) { return item.content == "copied by the test"; }), 10000);
        QTRY_VERIFY(received_count() >= 1);

        const auto image = record_image(solid(QSize(40, 30), Qt::red));
        QVERIFY(image);
        QCOMPARE(image->display_text(), QString("40x30"));
        const auto data = manager().get_image_data(*image->image_id);
        QVERIFY(data && !data->isEmpty());
        QCOMPARE(QImage::fromData(*data).size(), QSize(40, 30));

        // An image copied with a file reference is recorded as the image: image data comes before file paths.
        const QString path = QDir::toNativeSeparators(env_.dir.filePath("shot.png"));
        QVERIFY(solid(QSize(8, 8), Qt::blue).save(path));
        copy_image_to_clipboard(solid(QSize(8, 8), Qt::blue), path);
        wait_for_clipboard_writes(5.0);
        QTRY_VERIFY_WITH_TIMEOUT(find_item("image", [](const ClipboardItem &item) { return item.content == "[8x8]"; }), 10000);
        QVERIFY(!find_item("file"));

        // No update while images are being written; afterwards it may proceed.
        QTRY_COMPARE(manager().pending_writes(), size_t(0));
        QVERIFY(manager().pause_for_update());
        manager().resume_after_update();

        // Releasing the storage stops watching; reopening resumes it.
        const QString db = manager().get_db_path().value_or(QString());
        QVERIFY(manager().release_storage());
        QVERIFY(manager().reset_storage(db));
        QVERIFY(manager().is_monitoring());

        // Stopping before the watcher thread listens still returns.
        manager().stop_monitoring();
        QVERIFY(!manager().is_monitoring());
        const int before = received_count();
        QVERIFY(ClipboardManager::set_clipboard_text("after stopping", &error));
        QTest::qWait(500);
        QCOMPARE(received_count(), before);
        QVERIFY(!find_item("text", [](const ClipboardItem &item) { return item.content == "after stopping"; }));
    }

    void ownPastesAreNotRecordedUnlessKept() {
        const auto ids = add_texts({"left", "right"});
        start_watching();
        ClipboardController controller(manager());
        controller.load_history();
        env_.config->set_clipboard_multi_paste_separator(" + ");
        env_.config->set_clipboard_multi_paste_keep_merged(false);
        QVERIFY(controller.paste_items({ids[0], ids[1]}));
        QTest::qWait(500);
        QVERIFY(!find_item("text", [](const ClipboardItem &item) { return item.content == "left + right"; }));
        env_.config->set_clipboard_multi_paste_keep_merged(true);
        QVERIFY(controller.paste_items({ids[0], ids[1]}));
        QTRY_VERIFY_WITH_TIMEOUT(find_item("text", [](const ClipboardItem &item) { return item.content == "left + right"; }), 10000);
        env_.config->set_clipboard_multi_paste_keep_merged(false);
        env_.config->set_clipboard_multi_paste_separator("\n");
    }

    void recordedImagesPinPreviewAndStitch() {
        start_watching();
        const auto first = record_image(solid(QSize(40, 30), Qt::red));
        const auto second = record_image(solid(QSize(20, 50), Qt::green));
        QVERIFY(first && second);

        // The latest history image pins when the clipboard holds text.
        QApplication::clipboard()->setText("not an image");
        QVERIFY(pin_latest_clipboard_image(QPoint(200, 200)));
        QCOMPARE(PinManager::instance()->count(), 1);
        QCOMPARE(PinManager::instance()->get_all_pins().last()->orig_size(), QSize(20, 50));
        // The system clipboard's image comes first.
        QApplication::clipboard()->setImage(solid(QSize(11, 7), Qt::yellow));
        QVERIFY(pin_latest_clipboard_image());
        QCOMPARE(PinManager::instance()->get_all_pins().last()->orig_size(), QSize(11, 7));
        // With history off only the system clipboard counts.
        env_.config->set_clipboard_enabled(false);
        QApplication::clipboard()->setText("text again");
        QVERIFY(!pin_latest_clipboard_image());
        // A capture copied together with its file path is still an image.
        auto *capture = new QMimeData;
        capture->setImageData(solid(QSize(9, 6), Qt::cyan));
        capture->setUrls({QUrl::fromLocalFile(env_.dir.filePath("capture.png"))});
        QApplication::clipboard()->setMimeData(capture);
        QVERIFY(pin_latest_clipboard_image());
        QCOMPARE(PinManager::instance()->get_all_pins().last()->orig_size(), QSize(9, 6));
        env_.config->set_clipboard_enabled(true);
        QVERIFY(create_pin_from_clipboard_item(first->id, &manager(), nullptr));
        QCOMPARE(PinManager::instance()->count(), 4);
        QVERIFY(!create_pin_from_clipboard_item(999999, &manager(), nullptr));

        // The preview shows the stored image.
        auto *preview = PreviewPopup::instance();
        preview->set_manager(&manager());
        preview->set_display_enabled(true);
        preview->show_preview(*first, QPoint(100, 100), 0);
        QTRY_VERIFY(preview->isVisible());
        QVERIFY(preview->image_label->isVisible());
        preview->set_display_enabled(false);

        // Two images stitch into one clipboard image in the background.
        ClipboardController controller(manager());
        controller.auto_paste_enabled = true;
        controller.load_history();
        QVERIFY(controller.paste_items({first->id, second->id}));
        QTRY_VERIFY_WITH_TIMEOUT(!env_.pastes.isEmpty(), 10000);
        QVERIFY(controller.paste_items({first->id, second->id}, {}, false, false, "horizontal"));
        QTRY_VERIFY_WITH_TIMEOUT(env_.pastes.size() >= 2, 10000);
    }

    void theManageWindowAddsAndEditsRealImages() {
        start_watching();
        const auto group = manager().create_group("Pictures");
        auto *dialog = get_manage_dialog(&manager());
        dialog->show_and_activate();
        dialog->content_kind_switch->setCurrentItem("image");
        dialog->set_pending_image(solid(QSize(33, 22), Qt::magenta));
        dialog->title_input->setText("magenta");
        QSignalSpy added(dialog, &ManageDialog::content_added);
        dialog->save_btn->click();
        QTRY_VERIFY_WITH_TIMEOUT(!manager().get_by_group(*group).isEmpty(), 10000);
        const auto item = manager().get_by_group(*group).first();
        QCOMPARE(item.content, QString("[33x22]"));
        QCOMPARE(item.title.value_or(QString()), QString("magenta"));
        QVERIFY(!added.isEmpty());

        // Its editor knows the real size and rewrites a damaged content from it.
        QVERIFY(manager().update_item(item.id, "[1x1]", item.title));
        dialog->open_item_editor(item.id, *group);
        dialog->save_btn->click();
        QCOMPARE(manager().get_item(item.id)->content, QString("[33x22]"));
        QVERIFY(!dialog->grab().isNull());
        dialog->copy_image_item();
        dialog->pin_image_item();
        QCOMPARE(PinManager::instance()->count(), 1);
        const QString saved = QDir::toNativeSeparators(env_.dir.filePath("saved_image"));
        {
            FileDialogAnswerer answer(saved);
            dialog->save_image_item_as();
            QVERIFY(answer.answered);
        }
        QVERIFY(QFileInfo::exists(saved + ".png"));

        // The same image added again is already saved: moving it asks first. Records are told
        // apart by their creation second, so the second copy comes a second later.
        QTest::qWait(1100);
        const auto other = manager().create_group("Other");
        dialog->refresh_after_external_change();
        dialog->group_list->setCurrentRow(dialog->group_list->row_of_id(*other));
        dialog->content_kind_switch->setCurrentItem("image");
        dialog->set_pending_image(solid(QSize(33, 22), Qt::magenta));
        {
            ModalAnswerer yes(true);
            dialog->save_btn->click();
            QTRY_VERIFY_WITH_TIMEOUT(yes.answered() == 1, 10000);
            QVERIFY(yes.texts.first().contains("Pictures"));
        }
        QTRY_COMPARE(manager().get_by_group(*other).size(), 1);
        QVERIFY(manager().get_by_group(*group).isEmpty());

        // Without the watcher nothing is recorded: the window says so after a while.
        manager().stop_monitoring();
        dialog->set_pending_image(solid(QSize(9, 9), Qt::cyan));
        {
            ModalAnswerer ok(true);
            dialog->save_btn->click();
            QTRY_VERIFY_WITH_TIMEOUT(ok.answered() == 1, 10000);
            QVERIFY(ok.texts.first().contains("not recorded"));
        }
    }
};

QTEST_MAIN(ClipboardBackendTests)
#include "test_clipboard_backend.moc"
