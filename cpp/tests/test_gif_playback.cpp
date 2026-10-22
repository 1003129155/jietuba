#include "core/native.h"
#include "gif/gifrecorder.h"
#include "gif/playback_engine.h"
#include <QSignalSpy>
#include <QTest>
#include <cstdlib>

using namespace jietuba;

namespace {
constexpr int W = 16;
constexpr int H = 12;
const QList<QColor> COLORS = {QColor(220, 30, 30), QColor(30, 200, 40), QColor(40, 60, 210)};
const QList<int> ELAPSED = {0, 125, 250};

std::shared_ptr<gifrecorder::FrameStore> solid_store() {
    auto store = std::make_shared<gifrecorder::FrameStore>(W, H, 8);
    for (int i = 0; i < COLORS.size(); ++i) {
        QByteArray rgb;
        for (int p = 0; p < W * H; ++p) rgb.append(static_cast<char>(COLORS[i].red())).append(static_cast<char>(COLORS[i].green())).append(static_cast<char>(COLORS[i].blue()));
        char error[256] = {};
        if (jb_frame_store_push_rgb(store->handle(), reinterpret_cast<const uint8_t *>(rgb.constData()), static_cast<size_t>(rgb.size()),
                                    static_cast<uint32_t>(ELAPSED[i]), error, sizeof error) != 1)
            qFatal("push_rgb: %s", error);
    }
    return store;
}

bool near(const QColor &a, const QColor &b) {
    return std::abs(a.red() - b.red()) <= 8 && std::abs(a.green() - b.green()) <= 8 && std::abs(a.blue() - b.blue()) <= 8;
}

std::pair<QImage, int> wait_for_frame(QSignalSpy &spy) {
    if (spy.isEmpty() && !spy.wait(3000)) return {QImage(), -1};
    const auto args = spy.takeFirst();
    return {args.at(0).value<QImage>(), args.at(1).toInt()};
}
}

class GifPlaybackTests : public QObject {
    Q_OBJECT
private slots:
    void theDecoderHandsOverEachFrameWithItsPixelsAndTime_data() {
        QTest::addColumn<int>("display_w");
        QTest::addColumn<int>("display_h");
        QTest::addColumn<bool>("blocking");
        QTest::newRow("original size") << 0 << 0 << true;
        QTest::newRow("scaled") << 8 << 6 << true;
        QTest::newRow("scaled, without waiting") << 8 << 6 << false;
    }

    void theDecoderHandsOverEachFrameWithItsPixelsAndTime() {
        QFETCH(int, display_w);
        QFETCH(int, display_h);
        QFETCH(bool, blocking);
        const auto store = solid_store();
        const auto decoder = store->start_decoder(display_w, display_h, 2);
        const int w = display_w ? display_w : W, h = display_h ? display_h : H;
        for (int i = 0; i < COLORS.size(); ++i) {
            std::optional<std::pair<QByteArray, int>> frame;
            if (blocking) {
                frame = decoder->next_frame();
            } else {
                QTest::qWaitFor([&] { return (frame = decoder->try_next_frame()).has_value(); }, 3000);
            }
            QVERIFY(frame);
            QCOMPARE(frame->first.size(), static_cast<qsizetype>(w) * h * 3);
            QCOMPARE(frame->first, store->get_frame_rgb(i, display_w, display_h));
            QCOMPARE(frame->second, ELAPSED[i]);
        }
        if (blocking) QVERIFY(!decoder->next_frame());
        else QVERIFY(QTest::qWaitFor([&] { return decoder->is_finished(); }, 3000) && !decoder->try_next_frame());
    }

    void playbackShowsEveryFrameAtTheDisplaySize() {
        auto store = solid_store();
        std::vector<FrameData> frames(static_cast<size_t>(COLORS.size()), FrameData{0, W, H, {}, std::nullopt});
        PlaybackEngine engine;
        engine.load(&frames, 30, store);
        engine.set_display_size(8, 6);
        QSignalSpy shown(&engine, &PlaybackEngine::frame_ready);
        QSignalSpy finished(&engine, &PlaybackEngine::playback_finished);
        engine.play();
        for (int i = 0; i < COLORS.size(); ++i) {
            const auto [image, index] = wait_for_frame(shown);
            QCOMPARE(index, i);
            QCOMPARE(image.size(), QSize(8, 6));
            QVERIFY2(near(image.pixelColor(4, 3), COLORS[i]), qPrintable(image.pixelColor(4, 3).name()));
        }
        QVERIFY(finished.count() == 1 || finished.wait(3000));
    }

    void resizingWhilePausedKeepsTheFramesWhole() {
        auto store = solid_store();
        std::vector<FrameData> frames(static_cast<size_t>(COLORS.size()), FrameData{0, W, H, {}, std::nullopt});
        PlaybackEngine engine;
        engine.load(&frames, 30, store);
        engine.set_display_size(W, H);
        QSignalSpy shown(&engine, &PlaybackEngine::frame_ready);
        engine.play();
        QCOMPARE(wait_for_frame(shown).first.size(), QSize(W, H));
        engine.pause();
        shown.clear();
        engine.set_display_size(8, 6);
        engine.play();
        const auto [image, index] = wait_for_frame(shown);
        QVERIFY(index > 0);
        QCOMPARE(image.size(), QSize(W, H));
        QVERIFY2(near(image.pixelColor(W - 1, H - 1), COLORS[index]), qPrintable(image.pixelColor(W - 1, H - 1).name()));
    }
};

QTEST_GUILESS_MAIN(GifPlaybackTests)
#include "test_gif_playback.moc"
