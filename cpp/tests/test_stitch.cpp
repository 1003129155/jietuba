#include "stitch/incremental.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QTransform>
#include <QPainter>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <mutex>

using namespace jietuba;

namespace {
constexpr int FRAME_ROWS = 200;
QImage numbered_page() {
    QImage page(180, 1400, QImage::Format_RGB32);
    for (int y = 0; y < page.height(); ++y) {
        QRandomGenerator random(quint32(y + 13));
        auto *row = reinterpret_cast<QRgb *>(page.scanLine(y));
        for (int x = 0; x < page.width(); ++x) row[x] = qRgb(random.bounded(256), random.bounded(256), random.bounded(256));
    }
    QPainter painter(&page);
    painter.setPen(Qt::black);
    for (int y = 0; y < page.height(); y += 20) {
        painter.fillRect(0, y, 55, 18, Qt::white);
        painter.drawText(2, y + 13, QString::number(y));
    }
    return page;
}

QImage portion(const QImage &page, bool horizontal, int top, int rows) {
    return horizontal ? page.copy(top, 0, rows, page.height()) : page.copy(0, top, page.width(), rows);
}

StitchFrame view_frame(const QImage &page, bool horizontal, int top, int leading = 0, int trailing = 0) {
    const auto image = portion(page, horizontal, top - leading, FRAME_ROWS + leading + trailing);
    StitchFrame frame;
    frame.bgra = QByteArray(reinterpret_cast<const char *>(image.constBits()), image.sizeInBytes());
    frame.width = image.width();
    frame.height = image.height();
    frame.scroll_direction = horizontal ? "horizontal" : "vertical";
    frame.leading = leading;
    frame.trailing = trailing;
    return frame;
}

bool prepare(IncrementalStitcher &stitcher, const StitchFrame &frame, quint64 request) {
    QSignalSpy ready(&stitcher, &IncrementalStitcher::extension_ready);
    stitcher.prepare_extension(frame, request);
    // Also proves that preparation itself does not commit: export is an ordered barrier.
    stitcher.export_image();
    if (ready.isEmpty() && !ready.wait(3000)) return false;
    return ready.last()[0].toULongLong() == request && ready.last()[1].toBool();
}
}

// Expected values come from the Python reference (scripts/generate_stitch_fixtures.py).
class StitchTests : public QObject {
    Q_OBJECT
    QJsonObject fixtures_;
    QImage page_;
    QImage wide_;
private slots:
    void initTestCase() {
        QFile file(JIETUBA_STITCH_FIXTURE_DIR "/stitch.json");
        QVERIFY(file.open(QIODevice::ReadOnly));
        fixtures_ = QJsonDocument::fromJson(file.readAll()).object();
        page_ = QImage(JIETUBA_STITCH_FIXTURE_DIR "/stitch_page.png").convertToFormat(QImage::Format_RGB32);
        QVERIFY(!page_.isNull());
        wide_ = page_.transformed(QTransform().rotate(-90));
    }
    void incrementalStitchMatchesReference() {
        const int width = fixtures_["width"].toInt();
        const int frame_height = fixtures_["frame_height"].toInt();
        for (const auto &case_value : fixtures_["cases"].toArray()) {
            const auto item = case_value.toObject();
            const auto name = item["name"].toString();
            const auto direction = item["direction"].toString();
            const auto tops = item["tops"].toArray();
            const auto headings = item["headings"].toArray();

            IncrementalStitcher stitcher(96, 20, 0);
            std::mutex mutex;
            std::vector<FrameResult> results;
            connect(&stitcher, &IncrementalStitcher::frame_done, &stitcher, [&](const FrameResult &result) {
                std::lock_guard lock(mutex);
                results.push_back(result);
            }, Qt::DirectConnection);
            for (int index = 0; index < tops.size(); ++index) {
                const int top = tops[index].toInt();
                const auto image = direction == "horizontal" ? wide_.copy(top, 0, frame_height, width) : page_.copy(0, top, width, frame_height);
                StitchFrame frame;
                frame.bgra = QByteArray(reinterpret_cast<const char *>(image.constBits()), static_cast<qsizetype>(image.width()) * image.height() * 4);
                frame.width = image.width();
                frame.height = image.height();
                frame.index = index;
                frame.scroll_direction = direction;
                if (headings[index].isString()) frame.heading = headings[index].toString();
                stitcher.submit(frame);
            }
            const auto exported = stitcher.export_image().convertToFormat(QImage::Format_RGB888);
            stitcher.close();

            const auto expected = item["results"].toArray();
            QCOMPARE(static_cast<qsizetype>(results.size()), expected.size());
            for (qsizetype i = 0; i < expected.size(); ++i) {
                const auto want = expected[i].toObject();
                const auto &got = results[i];
                QVERIFY2(got.ok == want["ok"].toBool(), qPrintable(name));
                QCOMPARE(got.first, want["first"].toBool());
                QCOMPARE(got.index, want["index"].toInt());
                QCOMPARE(got.width, want["width"].toInt());
                QCOMPARE(got.height, want["height"].toInt());
                QCOMPARE(got.top, want["top"].toInt());
                QCOMPARE(got.shift, want["shift"].toInt());
                QCOMPARE(got.box.has_value(), !want["box"].isNull());
                if (got.box) QCOMPARE(QPoint(got.box->first, got.box->second), QPoint(want["box"].toArray()[0].toInt(), want["box"].toArray()[1].toInt()));
                QCOMPARE(got.size.has_value(), !want["size"].isNull());
                if (got.size) QCOMPARE(QSize(got.size->first, got.size->second), QSize(want["size"].toArray()[0].toInt(), want["size"].toArray()[1].toInt()));
                QCOMPARE(!got.preview.isNull(), !want["preview"].isNull());
                if (!got.preview.isNull()) QCOMPARE(got.preview.size(), QSize(want["preview"].toArray()[0].toInt(), want["preview"].toArray()[1].toInt()));
            }

            const auto export_info = item["export"].toObject();
            QCOMPARE(exported.size(), QSize(export_info["size"].toArray()[0].toInt(), export_info["size"].toArray()[1].toInt()));
            QCryptographicHash hash(QCryptographicHash::Sha256);
            for (int y = 0; y < exported.height(); ++y) hash.addData(QByteArrayView(reinterpret_cast<const char *>(exported.constScanLine(y)), exported.width() * 3));
            QVERIFY2(QString::fromLatin1(hash.result().toHex()) == export_info["sha256"].toString(), qPrintable(name));
        }
    }

    // Restarting drops the long image so far; the next frame starts a new one in its own direction.
    void restartingStartsANewLongImage() {
        const int width = fixtures_["width"].toInt();
        const int frame_height = fixtures_["frame_height"].toInt();
        IncrementalStitcher stitcher(96, 20, 0);
        std::mutex mutex;
        std::vector<FrameResult> results;
        connect(&stitcher, &IncrementalStitcher::frame_done, &stitcher, [&](const FrameResult &result) {
            std::lock_guard lock(mutex);
            results.push_back(result);
        }, Qt::DirectConnection);
        const auto submit = [&](int index, const QImage &image, const QString &direction) {
            StitchFrame frame;
            frame.bgra = QByteArray(reinterpret_cast<const char *>(image.constBits()), static_cast<qsizetype>(image.width()) * image.height() * 4);
            frame.width = image.width();
            frame.height = image.height();
            frame.index = index;
            frame.scroll_direction = direction;
            stitcher.submit(frame);
        };
        submit(0, page_.copy(0, 0, width, frame_height), "vertical");
        submit(1, page_.copy(0, 120, width, frame_height), "vertical");
        stitcher.restart();
        submit(2, wide_.copy(0, 0, frame_height, width), "horizontal");
        submit(3, wide_.copy(130, 0, frame_height, width), "horizontal");
        const auto exported = stitcher.export_image();
        stitcher.close();

        // As the horizontal reference case's first two frames.
        QJsonObject horizontal;
        for (const auto &item : fixtures_["cases"].toArray())
            if (item.toObject()["name"].toString() == "horizontal") horizontal = item.toObject();
        const auto expected = horizontal["results"].toArray();
        QCOMPARE(results.size(), size_t(4));
        QVERIFY(results[1].ok && results[1].scroll_direction == "vertical");
        QVERIFY(results[2].ok && results[2].first);
        for (int i = 0; i < 2; ++i) {
            const auto &got = results[2 + i];
            const auto want = expected[i].toObject();
            QCOMPARE(got.scroll_direction, QString("horizontal"));
            QCOMPARE(got.preview.size(), QSize(want["preview"].toArray()[0].toInt(), want["preview"].toArray()[1].toInt()));
            QCOMPARE(QSize(got.size->first, got.size->second), QSize(want["size"].toArray()[0].toInt(), want["size"].toArray()[1].toInt()));
        }
        QCOMPARE(exported.size(), QSize(results[3].size->first, results[3].size->second));
    }

    void supplementEndsAndContinueScrolling_data() {
        QTest::addColumn<bool>("horizontal");
        QTest::addColumn<bool>("at_start");
        QTest::newRow("top") << false << true;
        QTest::newRow("bottom") << false << false;
        QTest::newRow("left") << true << true;
        QTest::newRow("right") << true << false;
    }

    void supplementEndsAndContinueScrolling() {
        QFETCH(bool, horizontal);
        QFETCH(bool, at_start);
        const auto page = horizontal ? numbered_page().transformed(QTransform().rotate(-90)) : numbered_page();
        IncrementalStitcher stitcher(90, 20, 0);
        const int top = at_start ? 300 : 500;
        stitcher.submit(view_frame(page, horizontal, 400));
        stitcher.submit(view_frame(page, horizontal, top));
        int begin = std::min(400, top), end = std::max(400, top) + FRAME_ROWS;
        QCOMPARE(stitcher.export_image().convertToFormat(QImage::Format_RGB32), portion(page, horizontal, begin, end - begin));
        for (int extra : {20, 40}) {
            const auto before = stitcher.export_image();
            QVERIFY(prepare(stitcher, view_frame(page, horizontal, top, at_start ? extra : 0, at_start ? 0 : extra), extra));
            QCOMPARE(stitcher.export_image(), before);
            const auto result = stitcher.commit_extension(extra, 0);
            QVERIFY(result.ok);
            QVERIFY(at_start ? result.can_extend_start : result.can_extend_end);
            const int first = begin - (at_start ? extra : 0), last = end + (at_start ? 0 : extra);
            QCOMPARE(stitcher.export_image().convertToFormat(QImage::Format_RGB32), portion(page, horizontal, first, last - first));
            QCOMPARE(QSize(result.size->first, result.size->second), stitcher.export_image().size());
        }
        const int next = at_start ? 250 : 550;
        stitcher.submit(view_frame(page, horizontal, next, at_start ? 40 : 0, at_start ? 0 : 40));
        begin = at_start ? next - 40 : begin;
        end = at_start ? end : next + FRAME_ROWS + 40;
        // A normal frame covers the old strip: every numbered row/column occurs exactly once.
        QCOMPARE(stitcher.export_image().convertToFormat(QImage::Format_RGB32), portion(page, horizontal, begin, end - begin));
        const int middle = at_start ? 330 : 450;
        stitcher.submit(view_frame(page, horizontal, middle, at_start ? 40 : 0, at_start ? 0 : 40));
        const auto unchanged = stitcher.export_image();
        QVERIFY(!prepare(stitcher, view_frame(page, horizontal, middle, at_start ? 60 : 0, at_start ? 0 : 60), 60));
        QCOMPARE(stitcher.export_image(), unchanged);
        QCOMPARE(unchanged.convertToFormat(QImage::Format_RGB32), portion(page, horizontal, begin, end - begin));
    }

    void singleFrameSupplementCropAndRestart_data() {
        QTest::addColumn<bool>("horizontal");
        QTest::addColumn<QString>("side");
        for (bool horizontal : {false, true})
            for (const auto &side : {QString("top"), QString("bottom")})
                QTest::newRow(qPrintable(QString("%1-%2").arg(horizontal ? "horizontal" : "vertical", side))) << horizontal << side;
    }

    void singleFrameSupplementCropAndRestart() {
        QFETCH(bool, horizontal);
        QFETCH(QString, side);
        const auto page = horizontal ? numbered_page().transformed(QTransform().rotate(-90)) : numbered_page();
        IncrementalStitcher stitcher(90, 20, 0);
        stitcher.submit(view_frame(page, horizontal, 400));
        stitcher.export_image();
        QVERIFY(prepare(stitcher, view_frame(page, horizontal, 400, 20, 0), 1));
        QVERIFY(stitcher.commit_extension(1, 0).ok);
        QVERIFY(prepare(stitcher, view_frame(page, horizontal, 400, 20, 30), 2));
        const auto result = stitcher.commit_extension(2, 0);
        QVERIFY(result.ok && result.can_extend_start && result.can_extend_end);
        QCOMPARE(stitcher.export_image().convertToFormat(QImage::Format_RGB32), portion(page, horizontal, 380, 250));
        stitcher.submit(view_frame(page, horizontal, 450, 20, 30));
        QCOMPARE(stitcher.export_image().convertToFormat(QImage::Format_RGB32), portion(page, horizontal, 380, 300));
        stitcher.submit(view_frame(page, horizontal, 425, 20, 30));
        QCOMPARE(stitcher.export_image().convertToFormat(QImage::Format_RGB32), portion(page, horizontal, 380, 300));
        stitcher.crop(side);
        const int begin = side == "top" ? 405 : 380, end = side == "bottom" ? 655 : 680;
        QCOMPARE(stitcher.export_image().convertToFormat(QImage::Format_RGB32), portion(page, horizontal, begin, end - begin));
        stitcher.restart();
        stitcher.submit(view_frame(page, horizontal, 100));
        QCOMPARE(stitcher.export_image().convertToFormat(QImage::Format_RGB32), portion(page, horizontal, 100, FRAME_ROWS));
    }

    void cancelledMisalignedAndSupersededSupplementsKeepTheValidResult() {
        const auto page = numbered_page();
        IncrementalStitcher stitcher(90, 20, 0);
        stitcher.submit(view_frame(page, false, 400));
        stitcher.submit(view_frame(page, false, 500));
        const auto before = stitcher.export_image();
        QVERIFY(prepare(stitcher, view_frame(page, false, 500, 0, 30), 1));
        stitcher.discard_extension(1);
        QVERIFY(!stitcher.commit_extension(1, 0).ok);
        QCOMPARE(stitcher.export_image(), before);
        QVERIFY(!prepare(stitcher, view_frame(page, false, 511, 0, 30), 2));
        QCOMPARE(stitcher.export_image(), before);
        QVERIFY(!prepare(stitcher, view_frame(page, false, 500, 0, -10), 3));
        QCOMPARE(stitcher.export_image(), before);
        auto broken = view_frame(page, false, 500, 0, 30);
        broken.bgra.clear(); // A failed grab must not consume or replace the last valid pixels.
        QVERIFY(!prepare(stitcher, broken, 3));
        QCOMPARE(stitcher.export_image(), before);
        QVERIFY(prepare(stitcher, view_frame(page, false, 500, 0, 30), 4));
        stitcher.submit(view_frame(page, false, 550));
        QVERIFY(!stitcher.commit_extension(4, 0).ok);
        QCOMPARE(stitcher.export_image().convertToFormat(QImage::Format_RGB32), portion(page, false, 400, 350));
        QVERIFY(prepare(stitcher, view_frame(page, false, 550, 0, 30), 5));
        QVERIFY(!stitcher.commit_extension(4, 0).ok);
        const auto result = stitcher.commit_extension(5, 37);
        QVERIFY(result.ok);
        QCOMPARE(result.generation, quint64(37));
        QCOMPARE(stitcher.export_image().convertToFormat(QImage::Format_RGB32), portion(page, false, 400, 380));
    }
};

QTEST_MAIN(StitchTests)
#include "test_stitch.moc"
