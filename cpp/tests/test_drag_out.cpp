#include "core/drag_out.h"
#include "core/folder_drop.h"
#include <QDir>
#include <QDrag>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPointer>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <wrl/client.h>

using namespace jietuba;
using Microsoft::WRL::ComPtr;

namespace {
constexpr DWORD ANY_EFFECT = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;

// The data Explorer drags its files with.
ComPtr<IDataObject> shell_data(const QStringList &paths) {
    std::vector<PIDLIST_ABSOLUTE> items;
    for (const auto &path : paths) {
        PIDLIST_ABSOLUTE item = nullptr;
        if (SUCCEEDED(SHParseDisplayName(reinterpret_cast<PCWSTR>(QDir::toNativeSeparators(path).utf16()), nullptr, &item, 0, nullptr))) items.push_back(item);
    }
    ComPtr<IShellItemArray> array;
    ComPtr<IDataObject> data;
    if (SUCCEEDED(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(items.size()), const_cast<PCIDLIST_ABSOLUTE_ARRAY>(items.data()), &array)))
        array->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
    for (auto *item : items) CoTaskMemFree(item);
    return data;
}

POINTL at(int x) { return POINTL{x, 10}; }

// The most opaque pixel of a drag preview, 0-255.
int max_alpha(const QPixmap &pixmap) {
    const QImage image = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
    int alpha = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) alpha = std::max(alpha, qAlpha(image.pixel(x, y)));
    return alpha;
}

QByteArray read_all(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

DragOutFile counted_file(const QString &name, const QByteArray &bytes, std::shared_ptr<int> calls) {
    return {name, [bytes, calls](const QString &path) {
                ++*calls;
                QFile file(path);
                return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
            }};
}
}

class DragOutTests : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;

    QString temp_root() const { return dir_.filePath("drag"); }

    QString make_dir(const QString &name) const {
        const QString path = dir_.filePath(name);
        return QDir().mkpath(path) ? path : QString();
    }

    QString make_file(const QString &name) const {
        const QString path = dir_.filePath(name);
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(name.toUtf8()) > 0 ? path : QString();
    }

private slots:
    // The shell's drop targets live in a single-threaded apartment.
    void initTestCase() { QVERIFY(SUCCEEDED(OleInitialize(nullptr))); }
    void cleanupTestCase() { OleUninitialize(); }

    void init() { QDir(temp_root()).removeRecursively(); }

    // ---- Dropping onto folders ----

    void aFileDroppedOnAFolderMovesInOnTheSameDriveAndCopiesWithCtrl() {
        const QString moved = make_file("moved.txt"), copied = make_file("copied.txt"), box = make_dir("box");
        QStringList hovered;
        // Left of x = 100 is the folder.
        FolderDrop drop([&box](const QPoint &screen) { return screen.x() < 100 ? box : QString(); }, [&hovered](const QString &folder) { hovered.append(folder); });
        auto *target = drop.target();
        auto data = shell_data({moved});
        QVERIFY(data);
        // Beside the folder nothing may drop, and nothing is outlined.
        DWORD effect = ANY_EFFECT;
        QCOMPARE(target->DragEnter(data.Get(), MK_LBUTTON, at(200), &effect), S_OK);
        QCOMPARE(effect, DWORD(DROPEFFECT_NONE));
        QVERIFY(hovered.isEmpty());
        // On it the effect is Explorer's: move on the same drive, copy with Ctrl.
        effect = ANY_EFFECT;
        QCOMPARE(target->DragOver(MK_LBUTTON, at(10), &effect), S_OK);
        QCOMPARE(effect, DWORD(DROPEFFECT_MOVE));
        QCOMPARE(hovered, QStringList{box});
        effect = ANY_EFFECT;
        target->DragOver(MK_LBUTTON | MK_CONTROL, at(10), &effect);
        QCOMPARE(effect, DWORD(DROPEFFECT_COPY));
        effect = ANY_EFFECT;
        QCOMPARE(target->Drop(data.Get(), MK_LBUTTON, at(10), &effect), S_OK);
        // Having moved the file itself, the folder tells the source there is nothing left to delete.
        QCOMPARE(effect, DWORD(DROPEFFECT_NONE));
        QCOMPARE(hovered, (QStringList{box, QString()}));
        QTRY_VERIFY(QFileInfo::exists(QDir(box).filePath("moved.txt")));
        QTRY_VERIFY(!QFileInfo::exists(moved));
        // A drag whose source allows copying only, like one out of a pin, copies.
        data = shell_data({copied});
        effect = DROPEFFECT_COPY;
        target->DragEnter(data.Get(), MK_LBUTTON, at(10), &effect);
        QCOMPARE(effect, DWORD(DROPEFFECT_COPY));
        effect = DROPEFFECT_COPY;
        QCOMPARE(target->Drop(data.Get(), MK_LBUTTON, at(10), &effect), S_OK);
        QCOMPARE(effect, DWORD(DROPEFFECT_COPY));
        QTRY_VERIFY(QFileInfo::exists(QDir(box).filePath("copied.txt")));
        QVERIFY(QFileInfo::exists(copied));
    }

    void aFolderNeverTakesItselfOrWhatHoldsIt() {
        const QString box = make_dir("outer/box"), inner = make_dir("outer/box/inner"), other = make_dir("other");
        QString under;
        QStringList hovered;
        FolderDrop drop([&under](const QPoint &) { return under; }, [&hovered](const QString &folder) { hovered.append(folder); });
        auto *target = drop.target();
        auto data = shell_data({box});
        DWORD effect = ANY_EFFECT;
        under = box;
        target->DragEnter(data.Get(), MK_LBUTTON, at(10), &effect);
        QCOMPARE(effect, DWORD(DROPEFFECT_NONE));
        under = inner;
        effect = ANY_EFFECT;
        target->DragOver(MK_LBUTTON, at(10), &effect);
        QCOMPARE(effect, DWORD(DROPEFFECT_NONE));
        QVERIFY(hovered.isEmpty());
        // A name that only starts the same is another folder.
        const QString sibling = make_dir("outer/box2");
        under = sibling;
        effect = ANY_EFFECT;
        target->DragOver(MK_LBUTTON, at(10), &effect);
        QCOMPARE(effect, DWORD(DROPEFFECT_MOVE));
        under = other;
        effect = ANY_EFFECT;
        target->DragOver(MK_LBUTTON, at(10), &effect);
        QCOMPARE(effect, DWORD(DROPEFFECT_MOVE));
        QCOMPARE(hovered, (QStringList{sibling, other}));
        QCOMPARE(target->DragLeave(), S_OK);
        QCOMPARE(hovered, (QStringList{sibling, other, QString()}));
        QVERIFY(QFileInfo(box).isDir());
        QVERIFY(QDir(other).isEmpty());
    }

    void aDragOutlivingTheOwnerFindsNoFolders() {
        const QString file = make_file("kept.txt"), box = make_dir("box");
        int hovered = 0;
        auto drop = std::make_unique<FolderDrop>([&box](const QPoint &) { return box; }, [&hovered](const QString &) { ++hovered; });
        ComPtr<IDropTarget> target(drop->target());
        drop.reset();
        auto data = shell_data({file});
        DWORD effect = ANY_EFFECT;
        QCOMPARE(target->DragEnter(data.Get(), MK_LBUTTON, at(10), &effect), S_OK);
        QCOMPARE(effect, DWORD(DROPEFFECT_NONE));
        effect = ANY_EFFECT;
        QCOMPARE(target->Drop(data.Get(), MK_LBUTTON, at(10), &effect), S_OK);
        QCOMPARE(effect, DWORD(DROPEFFECT_NONE));
        QCOMPARE(hovered, 0);
        QVERIFY(QFileInfo::exists(file));
    }

    void aWindowHandleIsNeededToTakeDrops() {
        FolderDrop drop([](const QPoint &) { return QString(); }, [](const QString &) {});
        QVERIFY(!drop.attach(0));
    }

    void textAndHtmlAloneOfferNoFiles() {
        DragOutContent content;
        content.text = QString::fromUtf8("你好");
        content.html = ready_value(QString("<b>你好</b>"));
        DragOutMimeData data(content, temp_root());
        QCOMPARE(data.formats(), (QStringList{"text/plain", "text/html"}));
        QCOMPARE(data.text(), QString::fromUtf8("你好"));
        QCOMPARE(data.html(), QString("<b>你好</b>"));
        QVERIFY(!data.hasUrls());
        QVERIFY(data.file_paths().isEmpty());
        QVERIFY(!QDir(temp_root()).exists());
    }

    void madeFilesAreWrittenOnceWhenATargetAsks() {
        auto calls = std::make_shared<int>(0);
        DragOutContent content;
        content.made_files = {counted_file("a.txt", "a", calls), counted_file("b.png", "b", calls)};
        DragOutMimeData data(content, temp_root());
        // Explorer only sees formats offered from the start.
        QCOMPARE(data.formats(), QStringList{DragOutMimeData::FILES_MIME});
        QCOMPARE(*calls, 0);
        QVERIFY(!QDir(temp_root()).exists());
        const QStringList paths = QString::fromUtf8(data.data(DragOutMimeData::FILES_MIME)).split(u'\n');
        QCOMPARE(paths.size(), 2);
        QCOMPARE(QFileInfo(paths[0]).fileName(), QString("a.txt"));
        QCOMPARE(QFileInfo(paths[1]).fileName(), QString("b.png"));
        QCOMPARE(QFileInfo(paths[0]).absolutePath(), QFileInfo(paths[1]).absolutePath());
        QCOMPARE(QFileInfo(QFileInfo(paths[0]).absolutePath()).absolutePath(), QFileInfo(temp_root()).absoluteFilePath());
        QCOMPARE(read_all(paths[1]), QByteArray("b"));
        QCOMPARE(data.file_paths(), paths);
        QCOMPARE(data.written_files(), paths);
        QCOMPARE(*calls, 2);
    }

    void existingFilesComeFirstAndStayWhereTheyAre() {
        const QString own = dir_.filePath("own.txt");
        QFile file(own);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        auto calls = std::make_shared<int>(0);
        DragOutContent content;
        content.files = {own};
        content.made_files = {counted_file("made.txt", "m", calls)};
        DragOutMimeData data(content, temp_root());
        const QStringList paths = data.file_paths();
        QCOMPARE(paths.size(), 2);
        QCOMPARE(paths[0], own);
        QCOMPARE(QFileInfo(paths[1]).fileName(), QString("made.txt"));
        QCOMPARE(data.written_files(), QStringList{paths[1]});
    }

    void existingFilesAloneWriteNothing() {
        DragOutContent content;
        content.files = {"C:/x.txt"};
        DragOutMimeData data(content, temp_root());
        QCOMPARE(data.file_paths(), QStringList{"C:/x.txt"});
        QVERIFY(!QDir(temp_root()).exists());
    }

    void aFileThatCouldNotBeWrittenIsLeftOut() {
        DragOutContent content;
        content.made_files = {{"bad.txt", [](const QString &) { return false; }}, counted_file("good.txt", "g", std::make_shared<int>(0))};
        DragOutMimeData data(content, temp_root());
        const QStringList paths = data.file_paths();
        QCOMPARE(paths.size(), 1);
        QCOMPARE(QFileInfo(paths[0]).fileName(), QString("good.txt"));
    }

    void aDeferredImageIsMadeOnlyWhenATargetAsks() {
        auto calls = std::make_shared<std::atomic<int>>(0);
        DragOutContent content;
        content.image = std::async(std::launch::deferred, [calls] {
                            ++*calls;
                            QImage image(6, 4, QImage::Format_ARGB32);
                            image.fill(Qt::red);
                            return image;
                        }).share();
        DragOutMimeData data(content, temp_root());
        QCOMPARE(data.formats(), QStringList{"application/x-qt-image"});
        QVERIFY(data.hasImage());
        QCOMPARE(calls->load(), 0);
        QCOMPARE(qvariant_cast<QImage>(data.imageData()).size(), QSize(6, 4));
        QCOMPARE(qvariant_cast<QImage>(data.imageData()).pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(calls->load(), 1);
    }

    void deferredHtmlIsWorkedOutOnce() {
        auto calls = std::make_shared<int>(0);
        DragOutContent content;
        content.text = "a";
        content.html = std::async(std::launch::deferred, [calls] {
                           ++*calls;
                           return QString("<i>a</i>");
                       }).share();
        DragOutMimeData data(content, temp_root());
        QVERIFY(data.hasHtml());
        QCOMPARE(*calls, 0);
        QCOMPARE(data.html(), QString("<i>a</i>"));
        QCOMPARE(data.html(), QString("<i>a</i>"));
        QCOMPARE(*calls, 1);
    }

    void otherFormatsGoUnderTheirOwnType() {
        DragOutContent content;
        content.data = {{"image/gif", ready_value(QByteArray("GIF89a"))}};
        DragOutMimeData data(content, temp_root());
        QCOMPARE(data.formats(), QStringList{"image/gif"});
        QCOMPARE(data.data("image/gif"), QByteArray("GIF89a"));
        QVERIFY(!data.hasImage());
    }

    void lettingGoOfABackgroundPartNeverWaitsForIt() {
        std::promise<void> release;
        const auto gate = release.get_future().share();
        QElapsedTimer timer;
        timer.start();
        {
            const auto part = prepare_in_background([gate] {
                gate.wait();
                return QByteArray("late");
            });
            QVERIFY(part.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
        }
        QVERIFY(timer.elapsed() < 1000);
        release.set_value();
        QCOMPARE(prepare_in_background([] { return QByteArray("x"); }).get(), QByteArray("x"));
        const auto failed = prepare_in_background([]() -> QByteArray { throw std::runtime_error("no"); });
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, failed.get());
        QThreadPool::globalInstance()->waitForDone();
    }

    void aNullImageGivesNothing() {
        DragOutContent content;
        content.image = std::async(std::launch::deferred, [] { return QImage(); }).share();
        DragOutMimeData data(content, temp_root());
        QVERIFY(qvariant_cast<QImage>(data.imageData()).isNull());
    }

    void withoutATempFolderNothingIsWritten() {
        auto calls = std::make_shared<int>(0);
        DragOutContent content;
        content.made_files = {counted_file("a.txt", "a", calls)};
        DragOutMimeData data(content, QString());
        QVERIFY(data.file_paths().isEmpty());
        QCOMPARE(*calls, 0);
    }

    void theDragOnlyCopies() {
        if (QGuiApplication::platformName() != "offscreen") QSKIP("Requires the offscreen drag platform");
        QObject source;
        QPixmap preview(20, 10);
        preview.fill(Qt::blue);
        DragOutContent content;
        content.text = "x";
        auto *mime = new DragOutMimeData(content, temp_root());
        const QPointer<QMimeData> data = mime;
        exec_drag_out(&source, mime, preview, QPoint(3, 4));
        const auto drags = source.findChildren<QDrag *>();
        QCOMPARE(drags.size(), 1);
        QCOMPARE(drags.front()->supportedActions(), Qt::CopyAction);
        QCOMPARE(drags.front()->defaultAction(), Qt::CopyAction);
        QCOMPARE(drags.front()->pixmap().size(), QSize(20, 10));
        QCOMPARE(drags.front()->hotSpot(), QPoint(3, 4));
        // At 80% opacity, so where it will land shows through.
        QVERIFY(std::abs(max_alpha(drags.front()->pixmap()) - 204) <= 1);
        QCOMPARE(qRed(drags.front()->pixmap().toImage().pixel(10, 5)), 0);
        QCOMPARE(drags.front()->mimeData(), data.data());
        // Released with the drag once the drop is over.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(source.findChildren<QDrag *>().isEmpty());
        QVERIFY(!data);
    }
};

QTEST_MAIN(DragOutTests)
#include "test_drag_out.moc"
