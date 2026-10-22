#include "stitch/incremental.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QTransform>
#include <mutex>

using namespace jietuba;

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
};

QTEST_MAIN(StitchTests)
#include "test_stitch.moc"
