#include "clipboard/ui/widgets/item_drag.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <cstring>
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>

using namespace jietuba::clipboard;

namespace {
ClipboardItem text_item(qint64 id, const QString &text) {
    ClipboardItem item;
    item.id = id;
    item.content = text;
    item.content_type = "text";
    return item;
}

ClipboardItem image_item(qint64 id, const QString &image_id) {
    ClipboardItem item;
    item.id = id;
    item.content = "[image]";
    item.content_type = "image";
    item.image_id = image_id;
    return item;
}

ClipboardItem file_item(qint64 id, const QStringList &paths) {
    ClipboardItem item;
    item.id = id;
    item.content_type = "file";
    item.content = QString::fromUtf8(QJsonDocument(QJsonObject{{"files", QJsonArray::fromStringList(paths)}}).toJson(QJsonDocument::Compact));
    return item;
}

QByteArray read_all(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QStringList file_names(const QStringList &paths) {
    QStringList names;
    for (const auto &path : paths) names.append(QFileInfo(path).fileName());
    return names;
}

QStringList entries(const QString &dir) { return QDir(dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot); }

bool age_folder(const QString &path, qint64 seconds) {
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()), FILE_WRITE_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const qint64 ticks = (QDateTime::currentSecsSinceEpoch() - seconds + 11644473600LL) * 10000000LL;
    FILETIME time{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    const bool ok = SetFileTime(handle, nullptr, nullptr, &time);
    CloseHandle(handle);
    return ok;
}
}

class ClipboardDragTests : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;

    QString images_dir() const { return dir_.filePath("images"); }
    QString temp_root() const { return dir_.filePath("drag"); }

private slots:
    void init() {
        QDir(temp_root()).removeRecursively();
        QDir(images_dir()).removeRecursively();
        QDir().mkpath(images_dir());
    }

    void textGoesToEveryTargetAsTextAndAFile() {
        ItemDragMimeData data({text_item(1, "你好 world")}, images_dir(), temp_root());
        QCOMPARE(data.formats(), (QStringList{ItemDragMimeData::FILES_MIME, "text/plain"}));
        QCOMPARE(data.text(), QString("你好 world"));
        QVERIFY(!data.hasImage());
        // Qt's own file list would add a URL format that Word inserts as a link.
        QVERIFY(!data.hasUrls());
        QVERIFY(!QDir(temp_root()).exists());
    }

    void filesAreWrittenOnlyWhenATargetAsks() {
        auto first = text_item(1, "你好 world");
        first.created_at = QDateTime(QDate(2026, 10, 20), QTime(9, 8, 7));
        ItemDragMimeData data({first, text_item(2, "b")}, images_dir(), temp_root());
        QCOMPARE(data.text(), QString("你好 world\nb"));
        QVERIFY(!QDir(temp_root()).exists());
        const QStringList paths = QString::fromUtf8(data.data(ItemDragMimeData::FILES_MIME)).split(u'\n');
        QCOMPARE(file_names(paths), (QStringList{"clipboard_text_20261020_090807_1.txt", "clipboard_text_2.txt"}));
        QCOMPARE(read_all(paths.front()), QString("你好 world").toUtf8());
        // Asking again hands out the same files.
        QCOMPARE(data.file_paths(), paths);
        QCOMPARE(data.written_files(), paths);
        QCOMPARE(entries(temp_root()).size(), 1);
    }

    void eachDragWritesIntoItsOwnFolder() {
        ItemDragMimeData first({text_item(1, "a")}, images_dir(), temp_root());
        ItemDragMimeData second({text_item(1, "b")}, images_dir(), temp_root());
        const QString a = first.file_paths().front();
        const QString b = second.file_paths().front();
        QCOMPARE(QFileInfo(a).fileName(), QFileInfo(b).fileName());
        QVERIFY(QFileInfo(a).absolutePath() != QFileInfo(b).absolutePath());
        QCOMPARE(read_all(a), QByteArray("a"));
        QCOMPARE(read_all(b), QByteArray("b"));
    }

    void imageIsDecodedAndCopiedNotHandedOut() {
        QImage source(40, 30, QImage::Format_ARGB32);
        source.fill(QColor(255, 0, 0, 100));
        const QString library = QDir(images_dir()).filePath("abc.png");
        QVERIFY(source.save(library));
        QFile old(library);
        QVERIFY(old.open(QIODevice::ReadWrite));
        QVERIFY(old.setFileTime(QDateTime::currentDateTime().addDays(-3), QFileDevice::FileModificationTime));
        old.close();

        ItemDragMimeData data({image_item(1, "abc")}, images_dir(), temp_root());
        QVERIFY(data.hasImage());
        const QImage image = qvariant_cast<QImage>(data.imageData());
        QCOMPARE(image.size(), QSize(40, 30));
        QVERIFY(image.hasAlphaChannel());
        QVERIFY(!QDir(temp_root()).exists());

        const QStringList paths = data.file_paths();
        QCOMPARE(file_names(paths), QStringList{"clipboard_image_1.png"});
        QVERIFY(paths.front() != library);
        QCOMPARE(read_all(paths.front()), read_all(library));
        QVERIFY(QFileInfo(paths.front()).lastModified() > QDateTime::currentDateTime().addSecs(-60));
    }

    void severalImagesOfferNoSingleImage() {
        QImage source(4, 4, QImage::Format_RGB32);
        source.fill(Qt::blue);
        QVERIFY(source.save(QDir(images_dir()).filePath("a.png")));
        QVERIFY(source.save(QDir(images_dir()).filePath("b.png")));
        ItemDragMimeData data({image_item(1, "a"), image_item(2, "b")}, images_dir(), temp_root());
        QVERIFY(!data.hasImage());
        QCOMPARE(data.file_paths().size(), 2);
    }

    void fileItemsOfferTheirOwnPaths() {
        const QString own = dir_.filePath("own.txt");
        QFile file(own);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        ItemDragMimeData data({file_item(1, {own})}, images_dir(), temp_root());
        QCOMPARE(data.formats(), QStringList{ItemDragMimeData::FILES_MIME});
        QCOMPARE(data.file_paths(), QStringList{own});
        QVERIFY(!QDir(temp_root()).exists());
    }

    void singleTextKeepsItsHtml() {
        auto item = text_item(1, "bold");
        item.html_content = "<b>bold</b>";
        ItemDragMimeData data({item}, images_dir(), temp_root());
        QCOMPARE(data.html(), QString("<b>bold</b>"));
        ItemDragMimeData two({item, text_item(2, "x")}, images_dir(), temp_root());
        QVERIFY(!two.hasHtml());
    }

    void canDragNeedsSomethingToOffer() {
        QVERIFY(ItemDragMimeData::can_drag(text_item(1, "x"), images_dir()));
        QVERIFY(!ItemDragMimeData::can_drag(text_item(1, ""), images_dir()));
        QVERIFY(!ItemDragMimeData::can_drag(image_item(1, "missing"), images_dir()));
        QVERIFY(!ItemDragMimeData::can_drag(file_item(1, {}), images_dir()));
        QVERIFY(ItemDragMimeData::can_drag(file_item(1, {"C:/x.txt"}), images_dir()));
    }

    void theFilesGoOutAsADropFilesList() {
        const QStringList paths{"C:/drag/a.txt", QString::fromUtf8("D:/有 空格/图.png")};
        const QByteArray bytes = drop_files_bytes(paths);
        DROPFILES header{};
        std::memcpy(&header, bytes.constData(), sizeof(header));
        QCOMPARE(header.pFiles, DWORD(sizeof(DROPFILES)));
        QVERIFY(header.fWide);
        // Read back the way a drop target does.
        HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(bytes.size()));
        QVERIFY(global);
        std::memcpy(GlobalLock(global), bytes.constData(), static_cast<size_t>(bytes.size()));
        GlobalUnlock(global);
        auto *drop = static_cast<HDROP>(global);
        QCOMPARE(DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0), UINT(2));
        for (UINT i = 0; i < 2; ++i) {
            wchar_t name[MAX_PATH]{};
            DragQueryFileW(drop, i, name, MAX_PATH);
            QCOMPARE(QString::fromWCharArray(name), QDir::toNativeSeparators(paths[int(i)]));
        }
        GlobalFree(global);
    }

    void writtenFilesAreRemovedAfterTheDrop() {
        QString folder;
        {
            ItemDragMimeData data({text_item(1, "x")}, images_dir(), temp_root());
            folder = QFileInfo(data.file_paths().front()).absolutePath();
        }
        QVERIFY(QDir(folder).exists());
        QTRY_VERIFY_WITH_TIMEOUT(!QDir(folder).exists(), ItemDragMimeData::removal_delay_ms(1) + 2000);
    }

    void staleFoldersGo() {
        const QString stale = QDir(temp_root()).filePath("old");
        const QString fresh = QDir(temp_root()).filePath("new");
        QVERIFY(QDir().mkpath(stale));
        QVERIFY(QDir().mkpath(fresh));
        QVERIFY(age_folder(stale, 7200));
        ItemDragMimeData::remove_stale_folders(temp_root());
        QVERIFY(!QDir(stale).exists());
        QVERIFY(QDir(fresh).exists());
        // At start-up every folder goes, however new.
        ItemDragMimeData::remove_stale_folders(temp_root(), 0);
        QVERIFY(!QDir(fresh).exists());
        QVERIFY(QDir(temp_root()).exists());
    }

    void filesStayLongerTheLargerTheyAre() {
        QCOMPARE(ItemDragMimeData::removal_delay_ms(0), 5000);
        QCOMPARE(ItemDragMimeData::removal_delay_ms(300000), 8000);
        QCOMPARE(ItemDragMimeData::removal_delay_ms(qint64(1) << 40), 600000);
    }
};

QTEST_MAIN(ClipboardDragTests)
#include "test_clipboard_drag.moc"
