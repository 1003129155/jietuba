#include "clipboard/ui/windows/pin_window.h"
#include "pin/pin_animation.h"
#include "canvas/items/background_item.h"
#include "canvas/scene.h"
#include "pin/pin_canvas.h"
#include "pin/pin_canvas_view.h"
#include "pin/pin_file_card.h"
#include "pin/pin_manager.h"
#include "pin/pin_window.h"
#include "settings/tool_settings.h"
#include "ui/wait_bar.h"
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGraphicsRectItem>
#include <QImageWriter>
#include <QMenu>
#include <QMimeData>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTranslator>
#include <QUrl>
#include <optional>

using namespace jietuba;

namespace {
const QColor PALETTE[] = {Qt::red, Qt::green, Qt::blue, Qt::white};

// A GIF of solid frames in a four-colour palette. Each frame is LZW-coded as literals with a
// clear code every two pixels, so the code size never grows past three bits.
QByteArray make_gif(const QSize &size, const QList<int> &colors, const QList<int> &delays_cs, std::optional<int> loops) {
    QByteArray gif("GIF89a");
    const auto u16 = [&gif](int value) {
        gif.append(static_cast<char>(value & 0xff));
        gif.append(static_cast<char>((value >> 8) & 0xff));
    };
    u16(size.width());
    u16(size.height());
    // Global table of 2^(1+1) colours.
    gif.append(static_cast<char>(0x91));
    gif.append('\0');
    gif.append('\0');
    for (const auto &color : PALETTE) {
        gif.append(static_cast<char>(color.red()));
        gif.append(static_cast<char>(color.green()));
        gif.append(static_cast<char>(color.blue()));
    }
    if (loops) {
        gif.append("\x21\xff\x0bNETSCAPE2.0\x03\x01", 16);
        u16(*loops);
        gif.append('\0');
    }
    for (int f = 0; f < colors.size(); ++f) {
        gif.append("\x21\xf9\x04\x04", 4);
        u16(delays_cs.value(f));
        gif.append("\0\0", 2);
        gif.append('\x2c');
        u16(0);
        u16(0);
        u16(size.width());
        u16(size.height());
        gif.append('\0');
        // Minimum code size 2: clear is 4, end is 5.
        gif.append('\x02');
        QList<int> codes;
        const int pixels = size.width() * size.height();
        for (int p = 0; p < pixels; ++p) {
            if (p % 2 == 0) codes.append(4);
            codes.append(colors[f]);
        }
        codes.append(5);
        QByteArray data;
        quint32 bits = 0;
        int count = 0;
        for (const int code : codes) {
            bits |= static_cast<quint32>(code) << count;
            count += 3;
            while (count >= 8) {
                data.append(static_cast<char>(bits & 0xff));
                bits >>= 8;
                count -= 8;
            }
        }
        if (count > 0) data.append(static_cast<char>(bits & 0xff));
        for (int i = 0; i < data.size(); i += 255) {
            const auto block = data.mid(i, 255);
            gif.append(static_cast<char>(block.size()));
            gif.append(block);
        }
        gif.append('\0');
    }
    gif.append('\x3b');
    return gif;
}

struct Decoded {
    int index;
    QColor color;
    int delay;
};

// Opens the pin's context menu at global_pos and lists it (separators as "-", disabled entries
// in brackets); inspect runs while it is open; then the entry starting with trigger runs.
QStringList run_menu(PinWindow *pin, const QPoint &global_pos, const QString &trigger = {}, const std::function<void()> &inspect = {}) {
    QStringList entries;
    QTimer reader;
    reader.setSingleShot(true);
    QObject::connect(&reader, &QTimer::timeout, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) return;
        if (inspect) inspect();
        QAction *chosen = nullptr;
        for (auto *action : menu->actions()) {
            entries << (action->isSeparator() ? QString("-") : action->isEnabled() ? action->text() : "[" + action->text() + "]");
            // An exact match wins over one that only starts the same.
            if (!trigger.isEmpty() && !action->isSeparator() && action->text().startsWith(trigger) && !(chosen && chosen->text() == trigger)) chosen = action;
        }
        menu->close();
        if (chosen) chosen->trigger();
    });
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, [] {
        if (auto *popup = QApplication::activePopupWidget()) popup->close();
    });
    reader.start(0);
    guard.start(3000);
    pin->show_context_menu(global_pos);
    return entries;
}

int highlights(PinWindow *pin) {
    int count = 0;
    for (auto *item : pin->canvas->scene->items())
        if (item->type() == QGraphicsRectItem::Type) ++count;
    return count;
}

bool has_entry(const QStringList &entries, const QString &start) {
    return std::any_of(entries.begin(), entries.end(), [&start](const QString &entry) { return entry.startsWith(start); });
}
}

class PinMediaTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir dir_;
    QSettings *settings_ = nullptr;
    ToolSettingsManager *config_ = nullptr;

    QString picture(const QString &name, const QSize &size, const QColor &color) {
        QImage image(size, QImage::Format_RGB32);
        image.fill(color);
        const auto path = dir_.filePath(name);
        return image.save(path) ? path : QString();
    }

    // Only the in-process clipboard of the offscreen platform.
    static void copy_files(const QStringList &files) {
        QList<QUrl> urls;
        for (const auto &file : files) urls.append(QUrl::fromLocalFile(file));
        auto *mime = new QMimeData;
        mime->setUrls(urls);
        QApplication::clipboard()->setMimeData(mime);
    }

    static QList<PinWindow *> pins() { return PinManager::instance()->get_all_pins(); }

    QString write(const QString &name, const QByteArray &data) {
        const auto path = dir_.filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) return {};
        return path;
    }

    QString three_frames(std::optional<int> loops = 0) { return write("three.gif", make_gif(QSize(6, 4), {0, 1, 2}, {5, 12, 7}, loops)); }

private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        // A throwaway settings file, never the registry; the singleton binds once per process.
        if (!settings_) settings_ = new QSettings(dir_.filePath("settings.ini"), QSettings::IniFormat, this);
        config_ = get_tool_settings_manager(settings_);
        QCOMPARE(&config_->settings(), settings_);
    }

    void init() {
        settings_->clear();
        config_->set_ocr_enabled(false);
        // History would stand in for an empty clipboard.
        config_->set_clipboard_enabled(false);
    }

    void cleanup() {
        PinManager::instance()->close_all();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QApplication::clipboard()->clear();
    }

    // ---- load_animation ----

    void anAnimationLoadsItsFirstFrameDelayAndCount() {
        auto source = load_animation(three_frames());
        QVERIFY(source);
        QCOMPARE(source->first_frame.size(), QSize(6, 4));
        QCOMPARE(source->first_frame.pixelColor(3, 2), QColor(Qt::red));
        QCOMPARE(source->first_delay, 50);
        QCOMPARE(source->frame_count, 3);
        QCOMPARE(source->format, QByteArray("gif"));
        // Played from a copy under the same name: the original can go.
        const QString copy = source->path();
        QVERIFY(copy != source->file_path);
        QCOMPARE(QFileInfo(copy).fileName(), QString("three.gif"));
        QVERIFY(QFile::remove(source->file_path));
        QVERIFY(QFile::exists(copy));
        // The copy goes with the last source holding it.
        auto kept = *source;
        source.reset();
        QVERIFY(QFile::exists(copy));
        kept = AnimationSource();
        QVERIFY(!QFile::exists(copy));
    }

    void theLoopCountFollowsTheFile() {
        // NETSCAPE 0 plays forever; no extension plays once; n repeats n times.
        QCOMPARE(load_animation(three_frames(0))->loop_count, -1);
        QCOMPARE(load_animation(three_frames(std::nullopt))->loop_count, 0);
        QCOMPARE(load_animation(three_frames(2))->loop_count, 2);
    }

    void stillImagesAreNotAnimations() {
        QVERIFY(!load_animation(write("one.gif", make_gif(QSize(4, 4), {1}, {10}, 0))));
        QImage png(5, 5, QImage::Format_RGB32);
        png.fill(Qt::red);
        const auto png_path = dir_.filePath("still.png");
        QVERIFY(png.save(png_path));
        QVERIFY(!load_animation(png_path));
        QVERIFY(!load_animation(write("notes.txt", "not an image")));
        QVERIFY(!load_animation(dir_.filePath("missing.gif")));
        QVERIFY(!load_animation(dir_.path()));
    }

    // ---- AnimationDecodeThread ----

    void theDecoderSendsFramesInOrderWithTheirOwnDelays() {
        const auto source = load_animation(three_frames(std::nullopt));
        QVERIFY(source);
        AnimationDecodeThread thread(*source, 0, 7, nullptr);
        QList<Decoded> frames;
        bool finished = false;
        connect(&thread, &AnimationDecodeThread::frame_decoded, &thread, [&](int generation, int index, const QImage &frame, int delay) {
            QCOMPARE(generation, 7);
            frames.append({index, frame.pixelColor(0, 0), delay});
            thread.release_frame();
        });
        connect(&thread, &AnimationDecodeThread::finished_playing, &thread, [&](int generation) {
            QCOMPARE(generation, 7);
            finished = true;
        });
        thread.start();
        QTRY_VERIFY(finished);
        thread.stop();
        QCOMPARE(frames.size(), 3);
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(frames[i].index, i);
            QCOMPARE(frames[i].color, PALETTE[i]);
        }
        // Each frame carries how long it stays, as written for it.
        QCOMPARE(frames[0].delay, 50);
        QCOMPARE(frames[1].delay, 120);
        QCOMPARE(frames[2].delay, 70);
    }

    void theDecoderRepeatsAsTheLoopCountSays() {
        const auto source = load_animation(three_frames(1));
        QVERIFY(source);
        AnimationDecodeThread thread(*source, 0, 1, nullptr);
        QList<int> indices;
        bool finished = false;
        connect(&thread, &AnimationDecodeThread::frame_decoded, &thread, [&](int, int index, const QImage &, int) {
            indices.append(index);
            thread.release_frame();
        });
        connect(&thread, &AnimationDecodeThread::finished_playing, &thread, [&] { finished = true; });
        thread.start();
        QTRY_VERIFY(finished);
        thread.stop();
        QCOMPARE(indices, QList<int>({0, 1, 2, 0, 1, 2}));
    }

    void theDecoderStaysAFewFramesAhead() {
        const auto source = load_animation(write("five.gif", make_gif(QSize(4, 4), {0, 1, 2, 3, 0}, {5, 5, 5, 5, 5}, 0)));
        QVERIFY(source);
        AnimationDecodeThread thread(*source, 0, 1, nullptr);
        QList<int> indices;
        connect(&thread, &AnimationDecodeThread::frame_decoded, &thread, [&](int, int index, const QImage &, int) { indices.append(index); });
        thread.start();
        QTRY_COMPARE(indices.size(), PinAnimation::BUFFERED_FRAMES);
        QTest::qWait(100);
        QCOMPARE(indices.size(), PinAnimation::BUFFERED_FRAMES);
        // One frame shown frees room for one more.
        thread.release_frame();
        QTRY_COMPARE(indices.size(), PinAnimation::BUFFERED_FRAMES + 1);
        QTest::qWait(50);
        QCOMPARE(indices.size(), PinAnimation::BUFFERED_FRAMES + 1);
        // Stopping does not wait for a free slot.
        QElapsedTimer clock;
        clock.start();
        thread.stop();
        QVERIFY(clock.elapsed() < 1000);
        QVERIFY(thread.isFinished());
    }

    void theDecoderStartsWhereItIsTold() {
        const auto source = load_animation(three_frames());
        QVERIFY(source);
        AnimationDecodeThread thread(*source, 2, 1, nullptr);
        QList<Decoded> frames;
        connect(&thread, &AnimationDecodeThread::frame_decoded, &thread, [&](int, int index, const QImage &frame, int delay) {
            frames.append({index, frame.pixelColor(0, 0), delay});
            thread.release_frame();
        });
        thread.start();
        // Frame 2, then around again from 0.
        QTRY_VERIFY(frames.size() >= 2);
        thread.stop();
        QCOMPARE(frames[0].index, 2);
        QCOMPARE(frames[0].color, PALETTE[2]);
        QCOMPARE(frames[1].index, 0);
        QCOMPARE(frames[1].color, PALETTE[0]);
    }

    // ---- PinAnimation ----

    void playbackShowsEachFrameAfterTheOneBeforeHasHadItsDelay() {
        auto source = load_animation(three_frames());
        QVERIFY(source);
        QElapsedTimer clock;
        QList<QPair<QColor, qint64>> shown;
        clock.start();
        PinAnimation player(*source, [&](const QImage &frame) { shown.append({frame.pixelColor(0, 0), clock.elapsed()}); }, nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(shown.size() >= 4, 5000);
        // Green after red's 50 ms, blue after green's 120, red again after blue's 70, and so on.
        const QColor order[] = {PALETTE[1], PALETTE[2], PALETTE[0], PALETTE[1]};
        const int due[] = {50, 170, 240, 290};
        for (int i = 0; i < 4; ++i) {
            QCOMPARE(shown[i].first, order[i]);
            // Never early; the deadlines add up, so lateness does not pile up either.
            QVERIFY2(shown[i].second >= due[i] - 2, qPrintable(QString("frame %1 at %2 ms").arg(i).arg(shown[i].second)));
            QVERIFY2(shown[i].second <= due[i] + 400, qPrintable(QString("frame %1 at %2 ms").arg(i).arg(shown[i].second)));
        }
        QCOMPARE(player.frame_count(), 3);
    }

    void framesAskingForAlmostNoTimeShowForATenthOfASecond() {
        auto source = load_animation(write("fast.gif", make_gif(QSize(4, 4), {0, 1}, {0, 1}, 0)));
        QVERIFY(source);
        QElapsedTimer clock;
        QList<qint64> shown;
        clock.start();
        PinAnimation player(*source, [&](const QImage &) { shown.append(clock.elapsed()); }, nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(shown.size() >= 2, 5000);
        QVERIFY(shown[0] >= 98);
        QVERIFY(shown[1] >= 198);
    }

    void pausingHoldsTheFrameAndPlayingGoesOnFromIt() {
        auto source = load_animation(three_frames());
        QVERIFY(source);
        QList<QColor> shown;
        PinAnimation player(*source, [&](const QImage &frame) { shown.append(frame.pixelColor(0, 0)); }, nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(!shown.isEmpty(), 5000);
        player.set_paused(true);
        QVERIFY(player.paused());
        const int held = player.frame_index();
        const auto count = shown.size();
        QTest::qWait(300);
        QCOMPARE(shown.size(), count);
        QCOMPARE(player.frame_index(), held);
        player.set_paused(false);
        QTRY_VERIFY_WITH_TIMEOUT(shown.size() > count, 5000);
        QCOMPARE(shown[count], PALETTE[(held + 1) % 3]);
    }

    void steppingPausesAndWrapsAtBothEnds() {
        auto source = load_animation(three_frames());
        QVERIFY(source);
        QList<QColor> shown;
        PinAnimation player(*source, [&](const QImage &frame) { shown.append(frame.pixelColor(0, 0)); }, nullptr);
        player.set_paused(true);
        // Back from the first frame: the decoder reads up to the last one, and it shows when ready.
        player.step(-1);
        QVERIFY(player.paused());
        QCOMPARE(player.frame_index(), 0);
        QTRY_COMPARE(player.frame_index(), 2);
        QCOMPARE(shown.last(), PALETTE[2]);
        // Forward from there wraps to the first frame once the decoder has it.
        player.step(1);
        QTRY_COMPARE(player.frame_index(), 0);
        QCOMPARE(shown.last(), PALETTE[0]);
        // The decoder runs a few frames ahead, so stepping on finds the next one ready.
        QTest::qWait(100);
        player.step(1);
        QCOMPARE(player.frame_index(), 1);
        QCOMPARE(player.current_frame().pixelColor(0, 0), PALETTE[1]);
        // Back over a frame already shown is read again, off the UI thread.
        player.step(-1);
        QCOMPARE(player.frame_index(), 1);
        QTRY_COMPARE(player.frame_index(), 0);
        QCOMPARE(shown.last(), PALETTE[0]);
        player.step(1);
        QTRY_COMPARE(player.frame_index(), 1);
        // Playing goes on from there.
        const auto count = shown.size();
        player.set_paused(false);
        QTRY_VERIFY_WITH_TIMEOUT(shown.size() > count, 5000);
        QCOMPARE(shown[count], PALETTE[2]);
    }

    void steppingFarForwardWaitsForTheDecoder() {
        auto source = load_animation(write("seven.gif", make_gif(QSize(4, 4), {0, 1, 2, 3, 0, 1, 2}, {5, 5, 5, 5, 5, 5, 5}, 0)));
        QVERIFY(source);
        PinAnimation player(*source, {}, nullptr);
        player.set_paused(true);
        QTest::qWait(100);
        // Past the frames decoded ahead: through those, then the rest as the decoder reads them.
        player.step(6);
        QTRY_COMPARE(player.frame_index(), 6);
        QCOMPARE(player.current_frame().pixelColor(0, 0), PALETTE[2]);
        QTest::qWait(50);
        QCOMPARE(player.frame_index(), 6);
    }

    // A frame read up to from the start tells how far reading is, at once and as it goes on.
    void steppingBackTellsHowFarTheDecoderIs() {
        QList<int> colors, delays;
        for (int i = 0; i < 40; ++i) {
            colors << i % 4;
            delays << 5;
        }
        auto source = load_animation(write("forty.gif", make_gif(QSize(4, 4), colors, delays, 0)));
        QVERIFY(source);
        PinAnimation player(*source, {}, nullptr);
        player.set_paused(true);
        QSignalSpy progress(&player, &PinAnimation::seek_progress);
        player.step(-1);
        QVERIFY(player.seeking());
        QCOMPARE(progress.size(), 1);
        QCOMPARE(progress[0][0].toInt(), 0);
        QCOMPARE(progress[0][1].toInt(), 39);
        QTRY_COMPARE(player.frame_index(), 39);
        QVERIFY(!player.seeking());
        int done = 0;
        for (const auto &arguments : progress) {
            QCOMPARE(arguments[1].toInt(), 39);
            QVERIFY(arguments[0].toInt() >= done);
            done = arguments[0].toInt();
        }
        QVERIFY(progress.size() > 2);
        QVERIFY(done < 39);
    }

    // The bar waits a moment, so a frame read quickly shows none, and goes when the frame shows.
    void anAnimationPinBarsAFrameBeingReadAgain() {
        auto source = load_animation(write("wide.gif", make_gif(QSize(200, 100), {0, 1, 2, 3, 0, 1}, {5, 5, 5, 5, 5, 5}, 0)));
        QVERIFY(source);
        auto *pin = PinManager::instance()->create_animation_pin(std::move(*source), QPoint(100, 100), config_);
        auto *animation = pin->animation();
        animation->set_paused(true);
        QVERIFY(!pin->findChild<WaitBar *>());
        animation->step(-1);
        auto *bar = pin->findChild<WaitBar *>();
        QVERIFY(bar);
        QVERIFY(bar->show_pending());
        QVERIFY(!bar->isVisible());
        QVERIFY(bar->width() < pin->width());
        QVERIFY(std::abs(bar->geometry().center().x() - pin->rect().center().x()) <= 1);
        QVERIFY(std::abs(bar->geometry().center().y() - pin->rect().center().y()) <= 1);
        QTRY_VERIFY(!animation->seeking());
        QVERIFY(!bar->show_pending());
        QVERIFY(!bar->isVisible());
        // Only telling: clicks reach the pin.
        QVERIFY(bar->testAttribute(Qt::WA_TransparentForMouseEvents));
    }

    void aFinitePlayStopsOnTheLastFrameAndStartsOver() {
        auto source = load_animation(three_frames(std::nullopt));
        QVERIFY(source);
        QList<QColor> shown;
        PinAnimation player(*source, [&](const QImage &frame) { shown.append(frame.pixelColor(0, 0)); }, nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(player.paused(), 5000);
        QCOMPARE(shown, QList<QColor>({PALETTE[1], PALETTE[2]}));
        QCOMPARE(player.frame_index(), 2);
        player.set_paused(false);
        QTRY_COMPARE_WITH_TIMEOUT(shown.size(), 3, 5000);
        QCOMPARE(shown[2], PALETTE[0]);
        QTRY_VERIFY_WITH_TIMEOUT(player.paused(), 5000);
        QCOMPARE(shown.size(), 5);
    }

    void speedStepsThroughFixedValues() {
        auto source = load_animation(three_frames());
        QVERIFY(source);
        PinAnimation player(*source, {}, nullptr);
        QCOMPARE(player.speed(), 1.0);
        player.change_speed(1);
        QCOMPARE(player.speed(), 1.25);
        for (int i = 0; i < 20; ++i) player.change_speed(1);
        QCOMPARE(player.speed(), 4.0);
        player.change_speed(0);
        QCOMPARE(player.speed(), 1.0);
        player.change_speed(-1);
        QCOMPARE(player.speed(), 0.75);
        for (int i = 0; i < 20; ++i) player.change_speed(-1);
        QCOMPARE(player.speed(), 0.25);
    }

    void doubleSpeedHalvesTheDelays() {
        auto source = load_animation(write("slow.gif", make_gif(QSize(4, 4), {0, 1, 2}, {40, 40, 40}, 0)));
        QVERIFY(source);
        QElapsedTimer clock;
        QList<qint64> shown;
        PinAnimation player(*source, [&](const QImage &) { shown.append(clock.elapsed()); }, nullptr);
        for (int i = 0; i < 3; ++i) player.change_speed(1);
        QCOMPARE(player.speed(), 2.0);
        clock.start();
        QTRY_VERIFY_WITH_TIMEOUT(shown.size() >= 2, 5000);
        // 400 ms frames at twice the speed.
        QVERIFY2(shown[0] >= 198 && shown[0] < 380, qPrintable(QString::number(shown[0])));
        // Deadlines add up, so a late frame shortens the wait for the next; the second is due at 400 ms.
        QVERIFY2(shown[1] >= 398 && shown[1] < 580, qPrintable(QString::number(shown[1])));
    }

    // ---- The pin hotkey ----

    void copiedFilesPinEachPictureAndTheRestAsOneCard() {
        const auto still = picture("still.png", QSize(30, 20), Qt::red);
        const auto moving = three_frames();
        const auto notes = write("notes.txt", "notes");
        const auto folder = dir_.filePath("folder");
        QVERIFY(QDir().mkpath(folder));
        copy_files({notes, still, folder, moving});
        QVERIFY(clipboard::pin_latest_clipboard_image(QPoint(400, 300)));
        // Drawn and decoded off the UI thread: the hotkey returns before any pin opens.
        QCOMPARE(pins().size(), 0);
        QTRY_COMPARE(pins().size(), 3);
        const auto all = pins();
        QVERIFY(all[0]->file_card());
        QCOMPARE(all[0]->file_card()->files(), QStringList({notes, folder}));
        QCOMPARE(all[0]->orig_size(), all[0]->file_card()->size());
        // The pictures in the order copied, each a step down and right of the one before.
        QCOMPARE(all[1]->orig_size(), QSize(30, 20));
        QVERIFY(!all[1]->animation() && !all[1]->file_card());
        QVERIFY(all[2]->animation());
        QCOMPARE(all[2]->orig_size(), QSize(6, 4));
        QCOMPARE(all[2]->geometry().center() - all[1]->geometry().center(), QPoint(24, 24));
    }

    void picturesAreToldByNameAndThoseThatDoNotDecodeGoOnACard() {
        // A picture's name with no picture inside, and a picture without a picture's name.
        const auto fake = write("fake.png", "not a picture");
        const auto unnamed = dir_.filePath("photo_without_suffix");
        QVERIFY(QFile::copy(picture("real.png", QSize(8, 8), Qt::red), unnamed));
        copy_files({fake, unnamed});
        QVERIFY(clipboard::pin_latest_clipboard_image(QPoint(400, 300)));
        QTRY_COMPARE(pins().size(), 2);
        QCOMPARE(pins()[0]->file_card()->files(), QStringList({unnamed}));
        QCOMPARE(pins()[1]->file_card()->files(), QStringList({fake}));
    }

    void withPicturesOffEveryFileGoesOnTheCard() {
        const auto still = picture("still.png", QSize(30, 20), Qt::red);
        const auto notes = write("notes.txt", "notes");
        config_->set_pin_clipboard_image(false);
        copy_files({still, notes});
        QVERIFY(clipboard::pin_latest_clipboard_image(QPoint(400, 300)));
        QTRY_COMPARE(pins().size(), 1);
        QCOMPARE(pins()[0]->file_card()->files(), QStringList({still, notes}));
    }

    void withFilesOffOnlyThePicturesPin() {
        const auto still = picture("still.png", QSize(30, 20), Qt::red);
        const auto notes = write("notes.txt", "notes");
        config_->set_pin_clipboard_file(false);
        copy_files({still, notes});
        QVERIFY(clipboard::pin_latest_clipboard_image());
        QTRY_COMPARE(pins().size(), 1);
        QVERIFY(!pins()[0]->file_card());
        // Nothing left to pin: no card, and no older picture instead.
        copy_files({notes});
        QVERIFY(!clipboard::pin_latest_clipboard_image());
        QCOMPARE(pins().size(), 1);
    }

    void missingFilesAreLeftOut() {
        copy_files({dir_.filePath("gone.png")});
        clipboard::pin_latest_clipboard_image();
        QTest::qWait(300);
        QVERIFY(pins().isEmpty());
    }

    void aCopiedPhotoStandsAsTaken() {
        // A 4x2 JPEG whose orientation tag says it is turned a quarter: it pins 2x4.
        QImage wide(4, 2, QImage::Format_RGB32);
        wide.fill(Qt::blue);
        const auto path = dir_.filePath("photo.jpg");
        QImageWriter writer(path);
        writer.setTransformation(QImageIOHandler::TransformationRotate90);
        QVERIFY(writer.write(wide));
        copy_files({path});
        QVERIFY(clipboard::pin_latest_clipboard_image());
        QTRY_COMPARE(pins().size(), 1);
        QCOMPARE(pins().last()->orig_size(), QSize(2, 4));
    }

    void aPictureLargerThanTheScreenOpensShrunkAndSharp() {
        const QSize screen = QGuiApplication::primaryScreen()->geometry().size();
        const auto big = picture("big.png", screen + QSize(400, 300), Qt::darkCyan);
        copy_files({big});
        // What the pin shows on its first paint, before its own smooth scaling would run.
        QObject watch;
        QSize first_shown;
        connect(get_pin_manager(), &PinManager::pin_created, &watch, [&first_shown](PinWindow *pin) {
            QMetaObject::invokeMethod(pin, [pin, &first_shown] { first_shown = pin->canvas->scene->background->pixmap().size(); }, Qt::QueuedConnection);
        });
        QVERIFY(clipboard::pin_latest_clipboard_image(QPoint(100, 100)));
        QTRY_COMPARE(pins().size(), 1);
        auto *pin = pins()[0];
        QVERIFY(pin->width() < screen.width() && pin->height() < screen.height());
        // Scaled off the UI thread to exactly the size it opens at, and shown at that size from the start.
        QTRY_VERIFY(first_shown.isValid());
        QCOMPARE(first_shown, pin->size());
        QCOMPARE(pin->canvas->scene->background->pixmap().size(), pin->size());
        QCOMPARE(pin->canvas->scene->background->pixmap().toImage().pixelColor(5, 5), QColor(Qt::darkCyan));
    }

    void pngDataWinsOverTheFileBesideIt() {
        // A capture copied with a file reference: the picture, not whatever the file is.
        const auto other = picture("other.png", QSize(30, 20), Qt::red);
        QImage capture(9, 6, QImage::Format_RGB32);
        capture.fill(Qt::green);
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        QVERIFY(capture.save(&buffer, "PNG"));
        auto *mime = new QMimeData;
        mime->setData("image/png", png);
        mime->setUrls({QUrl::fromLocalFile(other)});
        QApplication::clipboard()->setMimeData(mime);
        QVERIFY(clipboard::pin_latest_clipboard_image());
        QCOMPARE(pins().size(), 1);
        QCOMPARE(pins()[0]->orig_size(), QSize(9, 6));
    }

    void aBitmapWinsOverTheFileBesideIt() {
        // A capture copied with the path it is still being saved to: that file may not be complete yet.
        const auto saving = write("capture.png", QByteArray());
        QImage capture(9, 6, QImage::Format_RGB32);
        capture.fill(Qt::green);
        auto *mime = new QMimeData;
        mime->setImageData(capture);
        mime->setUrls({QUrl::fromLocalFile(saving)});
        QApplication::clipboard()->setMimeData(mime);
        QVERIFY(clipboard::pin_latest_clipboard_image());
        QCOMPARE(pins().size(), 1);
        QCOMPARE(pins()[0]->orig_size(), QSize(9, 6));
    }

    void textStillPinsWhenNoFilesAreCopied() {
        QApplication::clipboard()->setText("just words");
        QVERIFY(clipboard::pin_latest_clipboard_image());
        // Laid out off the UI thread, it opens a moment later.
        QTRY_COMPARE(pins().size(), 1);
        QVERIFY(pins()[0]->text_only());
        QApplication::clipboard()->clear();
        QVERIFY(!clipboard::pin_latest_clipboard_image());
    }

    // ---- Animation and file pins ----

    void anAnimationPinPlaysWithoutTheImageTools() {
        auto source = load_animation(three_frames());
        QVERIFY(source);
        auto *pin = PinManager::instance()->create_animation_pin(std::move(*source), QPoint(100, 100), config_);
        QVERIFY(pin->animation());
        QCOMPARE(pin->base_pixmap().toImage().pixelColor(0, 0), PALETTE[0]);
        QTRY_COMPARE_WITH_TIMEOUT(pin->base_pixmap().toImage().pixelColor(0, 0), PALETTE[1], 5000);
        // What it shows is the frame it is on.
        pin->animation()->set_paused(true);
        QCOMPARE(pin->get_current_image().pixelColor(1, 1), pin->animation()->current_frame().pixelColor(1, 1));
        // No drawing over frames that change, no text to read.
        QVERIFY(!pin->annotatable());
        QVERIFY(!pin->recognizable());
        pin->toggle_toolbar();
        QVERIFY(!pin->toolbar);
        pin->toggle_text_mode();
        QVERIFY(!pin->text_mode());
    }

    void copyingAnAnimationGivesItsFileAndBytes() {
        const auto original = three_frames();
        QFile file(original);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        file.close();
        auto source = load_animation(original);
        QVERIFY(source);
        auto *pin = PinManager::instance()->create_animation_pin(std::move(*source), QPoint(100, 100), config_);
        // Gone from where it was: the pin's own copy, under the same name, goes on the clipboard.
        QVERIFY(QFile::remove(original));
        pin->copy_to_clipboard();
        const auto *mime = QApplication::clipboard()->mimeData();
        QCOMPARE(mime->data("image/gif"), bytes);
        const auto copied = mime->urls().value(0).toLocalFile();
        QCOMPARE(QFileInfo(copied).fileName(), QString("three.gif"));
        QFile copy(copied);
        QVERIFY(copy.open(QIODevice::ReadOnly));
        QCOMPARE(copy.readAll(), bytes);
        copy.close();
        // Closing the pin removes its own copy; what was put on the clipboard stays to be pasted.
        const QString own = pin->animation()->source().path();
        QVERIFY(own != copied);
        pin->close_window();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!QFile::exists(own));
        QVERIFY(QFile::exists(copied));
    }

    void theAnimationMenuControlsPlayback() {
        auto source = load_animation(three_frames());
        QVERIFY(source);
        auto *pin = PinManager::instance()->create_animation_pin(std::move(*source), QPoint(100, 100), config_);
        auto *animation = pin->animation();
        animation->set_paused(true);
        const auto at = pin->mapToGlobal(QPoint(2, 2));
        auto entries = run_menu(pin, at, "Next frame");
        for (const char *entry : {"Copy animation", "Copy current frame", "Save as", "Play", "Next frame", "Previous frame", "Faster", "Slower", "Normal speed",
                                  "[Speed: 1.0x]", "[Frame: 1/3]", "Always on top", "Thumbnail mode"})
            QVERIFY2(has_entry(entries, entry), entry);
        // Nothing that reads or draws on the image.
        for (const char *entry : {"Translate", "Image transform", "Toolbar", "Hide annotations", "Text selection", "Text mode"})
            QVERIFY2(!has_entry(entries, entry), entry);
        QCOMPARE(animation->frame_index(), 1);
        run_menu(pin, at, "Faster");
        QCOMPARE(animation->speed(), 1.25);
        entries = run_menu(pin, at, "Play");
        QVERIFY(has_entry(entries, "[Speed: 1.25x]"));
        QVERIFY(has_entry(entries, "[Frame: 2/3]"));
        QVERIFY(!animation->paused());
        entries = run_menu(pin, at, "Pause");
        QVERIFY(animation->paused());
    }

    void theFileMenuActsOnTheRightClickedFileOrAll() {
        const auto a = write("a.txt", "a"), b = write("b.txt", "b"), c = write("c.txt", "c");
        const PinFileCard card({a, b, c}, QSize(2 * PinFileCard::CELL_WIDTH, 600));
        auto *pin = PinManager::instance()->create_file_pin(card, QPoint(100, 100), config_);
        const auto global = [pin](const QPointF &scene_pos) { return pin->view->viewport()->mapToGlobal(pin->view->mapFromScene(scene_pos)); };
        int open_highlights = -1;
        auto entries = run_menu(pin, global(card.cell_rect(1).center()), "Copy this file's path", [&] { open_highlights = highlights(pin); });
        for (const char *entry : {"Open", "Copy this file", "Copy this file's path", "Show in Explorer", "Copy image", "Copy plain text", "Save as", "Toolbar",
                                  "Hide annotations"})
            QVERIFY2(has_entry(entries, entry), entry);
        for (const char *entry : {"Copy all files", "Translate", "Image transform", "Text selection", "Text mode"}) QVERIFY2(!has_entry(entries, entry), entry);
        QCOMPARE(QApplication::clipboard()->text(), QDir::toNativeSeparators(b));
        // The card was marked while the menu was open, and only then.
        QCOMPARE(open_highlights, 1);
        QCOMPARE(highlights(pin), 0);
        // The empty cell beside the last card: all the files.
        entries = run_menu(pin, global(card.cell_rect(3).center()), "Copy all file paths", [&] { open_highlights = highlights(pin); });
        QVERIFY(has_entry(entries, "Copy all files"));
        QVERIFY(!has_entry(entries, "Open"));
        QCOMPARE(open_highlights, 0);
        QCOMPARE(QApplication::clipboard()->text(), QDir::toNativeSeparators(QStringList({a, b, c}).join('\n')));
        // So does Copy plain text.
        QApplication::clipboard()->clear();
        run_menu(pin, global(card.cell_rect(0).center()), "Copy plain text");
        QCOMPARE(QApplication::clipboard()->text(), QDir::toNativeSeparators(QStringList({a, b, c}).join('\n')));
        // One file: its card fills the pin, so the menu is always about it.
        auto *single = PinManager::instance()->create_file_pin(PinFileCard({a}, QSize(800, 600)), QPoint(300, 300), config_);
        entries = run_menu(single, single->mapToGlobal(QPoint(single->width() - 2, single->height() - 2)), "Copy this file");
        QVERIFY(!has_entry(entries, "Copy all"));
        QCOMPARE(QApplication::clipboard()->mimeData()->urls(), QList<QUrl>({QUrl::fromLocalFile(a)}));
    }

    void aFilePinFindsTheFileUnderThePointerAndCopiesPaths() {
        const auto a = write("a.txt", "a"), b = write("b.txt", "b");
        const PinFileCard card({a, b}, QSize(800, 400));
        auto *pin = PinManager::instance()->create_file_pin(card, QPoint(100, 100), config_);
        QVERIFY(!pin->recognizable());
        QVERIFY(pin->annotatable());
        const auto global = [pin, &card](int index) {
            return pin->view->viewport()->mapToGlobal(pin->view->mapFromScene(QPointF(card.cell_rect(index).center())));
        };
        QCOMPARE(pin->file_at(global(0)), 0);
        QCOMPARE(pin->file_at(global(1)), 1);
        QCOMPARE(pin->file_at(pin->mapToGlobal(QPoint(-5, -5))), -1);
        // The copy-text key copies the paths.
        pin->copy_all_text();
        QCOMPARE(QApplication::clipboard()->text(), QDir::toNativeSeparators(a) + "\n" + QDir::toNativeSeparators(b));
        PinFileCard::copy_files({a, b});
        QCOMPARE(QApplication::clipboard()->mimeData()->urls(), QList<QUrl>({QUrl::fromLocalFile(a), QUrl::fromLocalFile(b)}));
    }

    // ---- Interface text ----

    void everyNewTextIsTranslated() {
        const QList<QPair<const char *, const char *>> texts = {
            {"PinWindow", "Folder"}, {"PinWindow", "...%1 items in all"}, {"PinWindow", "Open"}, {"PinWindow", "Copy this file"},
            {"PinWindow", "Copy this file's path"}, {"PinWindow", "Show in Explorer"}, {"PinWindow", "Copy all files"},
            {"PinWindow", "Copy all file paths"}, {"PinWindow", "Copy animation"}, {"PinWindow", "Copy current frame"},
            {"PinWindow", "Play"}, {"PinWindow", "Pause"}, {"PinWindow", "Next frame"}, {"PinWindow", "Previous frame"}, {"PinWindow", "Faster"},
            {"PinWindow", "Slower"}, {"PinWindow", "Normal speed"}, {"PinWindow", "Speed: %1"}, {"PinWindow", "Frame: %1"},
            {"PinWindow", "Save Animation"}, {"PinWindow", "Pin Many Images"},
            {"PinWindow", "Pin %1 images at once? So many windows may slow the computer down."}, {"MainApp", "Nothing on the clipboard to pin"},
            {"SettingsDialog", "File"}, {"SettingsDialog", "Copied files and folders, shown with their icons, names and sizes."},
            {"SettingsDialog", "Screenshots, copied pictures and picture files. Animated GIF and WebP files play."},
            {"SettingsDialog", "What this hotkey takes from the clipboard. When it holds both a picture and text, such as copied spreadsheet cells, the picture is pinned."},
        };
        for (const char *language : {"zh", "zh_TW", "en", "ja", "ko"}) {
            QTranslator translator;
            QVERIFY(translator.load(QString(":/translations/app_%1.qm").arg(language)));
            for (const auto &[context, text] : texts)
                QVERIFY2(!translator.translate(context, text).isEmpty(), qPrintable(QString("%1 %2: %3").arg(language, context, text)));
        }
    }

    // ---- PinFileCard ----

    void cardsFillRowsUpToTheLimit() {
        const QStringList four = {"a", "b", "c", "d"};
        const PinFileCard card(four, QSize(800, 500));
        QCOMPARE(card.columns(), 3);
        QCOMPARE(card.rows(), 2);
        QCOMPARE(card.shown(), 4);
        QVERIFY(!card.overflows());
        QCOMPARE(card.size(), QSize(3 * PinFileCard::CELL_WIDTH, 2 * PinFileCard::CELL_HEIGHT));
        // Fewer files than columns: only as wide as they need.
        const PinFileCard two({"a", "b"}, QSize(800, 500));
        QCOMPARE(two.size(), QSize(2 * PinFileCard::CELL_WIDTH, PinFileCard::CELL_HEIGHT));
        // A limit smaller than one card still shows one.
        const PinFileCard tight({"a", "b"}, QSize(10, 10));
        QCOMPARE(tight.size(), QSize(PinFileCard::CELL_WIDTH, PinFileCard::CELL_HEIGHT));
        QCOMPARE(tight.shown(), 0);
        QVERIFY(tight.overflows());
    }

    void filesBeyondTheGridAreCountedInItsLastCell() {
        QStringList ten;
        for (int i = 0; i < 10; ++i) ten.append(QString::number(i));
        const PinFileCard card(ten, QSize(2 * PinFileCard::CELL_WIDTH + 20, 2 * PinFileCard::CELL_HEIGHT + 5));
        QCOMPARE(card.columns(), 2);
        QCOMPARE(card.rows(), 2);
        QCOMPARE(card.shown(), 3);
        QVERIFY(card.overflows());
        // Exactly as many files as cells need no count.
        const PinFileCard full(ten.mid(0, 4), QSize(2 * PinFileCard::CELL_WIDTH, 2 * PinFileCard::CELL_HEIGHT));
        QCOMPARE(full.shown(), 4);
        QVERIFY(!full.overflows());
    }

    void aPointFindsTheFileOfItsCard() {
        QStringList ten;
        for (int i = 0; i < 10; ++i) ten.append(QString::number(i));
        const PinFileCard card(ten, QSize(2 * PinFileCard::CELL_WIDTH, 2 * PinFileCard::CELL_HEIGHT));
        const auto centre = [&card](int index) { return QPointF(card.cell_rect(index).center()); };
        QCOMPARE(card.file_at(centre(0)), 0);
        QCOMPARE(card.file_at(centre(1)), 1);
        QCOMPARE(card.file_at(centre(2)), 2);
        // The count cell and points off the grid.
        QCOMPARE(card.file_at(centre(3)), -1);
        QCOMPARE(card.file_at(QPointF(-1, 5)), -1);
        QCOMPARE(card.file_at(QPointF(2 * PinFileCard::CELL_WIDTH + 1, 5)), -1);
        QCOMPARE(card.file_at(QPointF(5, 2 * PinFileCard::CELL_HEIGHT + 1)), -1);
    }

    void sizesReadInTheLargestWholeUnit() {
        QCOMPARE(PinFileCard::format_size(0), QString("0 B"));
        QCOMPARE(PinFileCard::format_size(1023), QString("1023 B"));
        QCOMPARE(PinFileCard::format_size(1024), QString("1.00 KB"));
        QCOMPARE(PinFileCard::format_size(6574571), QString("6.27 MB"));
        QCOMPARE(PinFileCard::format_size(qint64(5) << 30), QString("5.00 GB"));
        QCOMPARE(PinFileCard::format_size(qint64(3) << 40), QString("3.00 TB"));
    }

    void theCardImageIsWhiteWithAnIconPerFile() {
        const auto file = write("report.txt", "hello");
        const PinFileCard card({file, dir_.path()}, QSize(800, 500));
        const QImage image = card.render(card.size());
        QCOMPARE(image.size(), card.size());
        // Twice as large, at twice the pixels.
        const QImage large = card.render(card.size() * 2, 1.5);
        QCOMPARE(large.size(), card.size() * 3);
        QCOMPARE(large.devicePixelRatio(), 1.5);
        // The margin around the cards.
        QCOMPARE(image.pixelColor(1, 1), QColor(Qt::white));
        QCOMPARE(image.pixelColor(image.width() - 2, image.height() - 2), QColor(Qt::white));
        // Each icon left something on the white.
        for (int i = 0; i < 2; ++i) {
            const auto cell = card.cell_rect(i);
            const QRect icon(cell.x() + PinFileCard::MARGIN, cell.y() + (PinFileCard::CELL_HEIGHT - PinFileCard::ICON_SIZE) / 2, PinFileCard::ICON_SIZE,
                             PinFileCard::ICON_SIZE);
            bool drawn = false;
            for (int y = icon.top(); y <= icon.bottom() && !drawn; ++y)
                for (int x = icon.left(); x <= icon.right() && !drawn; ++x) drawn = image.pixelColor(x, y) != QColor(Qt::white);
            QVERIFY2(drawn, qPrintable(QString("no icon in card %1").arg(i)));
        }
    }
};

QTEST_MAIN(PinMediaTests)
#include "test_pin_media.moc"
