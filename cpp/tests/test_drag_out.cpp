#include "core/drag_out.h"
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

using namespace jietuba;

namespace {
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

private slots:
    void init() { QDir(temp_root()).removeRecursively(); }

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
