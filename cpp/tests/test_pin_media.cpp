#include "clipboard/ui/windows/pin_window.h"
#include "clipboard/core/manager.h"
#include "pin/pin_animation.h"
#include "canvas/items/background_item.h"
#include "canvas/scene.h"
#include "pin/pin_canvas.h"
#include "pin/pin_canvas_view.h"
#include "pin/pin_file_card.h"
#include "pin/pin_file_selection.h"
#include "core/i18n.h"
#include "pin/pin_manager.h"
#include "pin/pin_ocr_manager.h"
#include "pin/pin_text_selection.h"
#include "pin/pin_text_content.h"
#include "pin/pin_window.h"
#include "settings/tool_settings.h"
#include "ui/wait_bar.h"
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDrag>
#include <QElapsedTimer>
#include <QFile>
#include <QGraphicsRectItem>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
            // An exact match, the shortcut shown after it aside, wins over one that only starts the same.
            const auto exact = [&trigger](const QString &text) { return text == trigger || text.startsWith(trigger + " ("); };
            if (!trigger.isEmpty() && !action->isSeparator() && action->text().startsWith(trigger) && !(chosen && exact(chosen->text()))) chosen = action;
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

// Left-button events on the view's viewport, through the pin's event filter as real input goes.
void send_left(PinWindow *pin, QEvent::Type type, const QPoint &local, Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers) {
    auto *viewport = pin->view->viewport();
    QMouseEvent event(type, QPointF(local), QPointF(viewport->mapToGlobal(local)), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, modifiers);
    QApplication::sendEvent(viewport, &event);
}

// A click that stays where it was pressed.
void click_at(PinWindow *pin, const QPoint &local, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    send_left(pin, QEvent::MouseButtonPress, local, Qt::LeftButton, modifiers);
    send_left(pin, QEvent::MouseButtonRelease, local, Qt::NoButton, modifiers);
}

// As Windows delivers it: the second press arrives as a double click.
void double_click_at(PinWindow *pin, const QPoint &local) {
    click_at(pin, local);
    send_left(pin, QEvent::MouseButtonDblClick, local, Qt::LeftButton, Qt::NoModifier);
    send_left(pin, QEvent::MouseButtonRelease, local, Qt::NoButton, Qt::NoModifier);
}

// Presses at start with the modifiers held and moves 30 pixels right. The offscreen platform
// ends a drag at once; the drag object stays until deferred deletes run.
QDrag *drag_from(PinWindow *pin, const QPoint &start, Qt::KeyboardModifiers modifiers) {
    send_left(pin, QEvent::MouseButtonPress, start, Qt::LeftButton, modifiers);
    send_left(pin, QEvent::MouseMove, start + QPoint(30, 0), Qt::LeftButton, modifiers);
    send_left(pin, QEvent::MouseButtonRelease, start + QPoint(30, 0), Qt::NoButton, modifiers);
    const auto drags = pin->findChildren<QDrag *>();
    return drags.isEmpty() ? nullptr : drags.front();
}

QPoint viewport_global(PinWindow *pin, const QPoint &local) { return pin->view->viewport()->mapToGlobal(local); }

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

    QStringList opened_;
    // Answers the question about small pins, which most pictures here are; accepting pins them as
    // before.
    QTimer small_pin_answerer_;
    bool small_pin_answer_ = true;
    int small_pin_asked_ = 0;

public slots:
    // Stands in for the shell while a test opens files.
    void record_opened(const QUrl &url) { opened_.append(url.toLocalFile()); }

private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        // A throwaway settings file, never the registry; the singleton binds once per process.
        if (!settings_) settings_ = new QSettings(dir_.filePath("settings.ini"), QSettings::IniFormat, this);
        config_ = get_tool_settings_manager(settings_);
        QCOMPARE(&config_->settings(), settings_);
        connect(&small_pin_answerer_, &QTimer::timeout, this, [this] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog || dialog->windowTitle() != i18n_tr("Pin Too Small", "PinWindow")) return;
            ++small_pin_asked_;
            if (!small_pin_answer_) return dialog->reject();
            // Closing is a no; only the yes button answers yes.
            auto *box = dialog->findChild<QDialogButtonBox *>();
            for (auto *button : box->buttons())
                if (box->buttonRole(button) == QDialogButtonBox::YesRole) button->click();
        });
        small_pin_answerer_.start(5);
    }

    void init() {
        small_pin_answer_ = true;
        small_pin_asked_ = 0;
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

    void smallPicturesAskOnceForTheWholeBatch() {
        const auto big = picture("big.png", QSize(40, 30), Qt::red);
        QStringList files;
        for (const auto color : {Qt::blue, Qt::cyan, Qt::magenta}) files.append(picture(QString("small%1.png").arg(int(color)), QSize(40, 29), color));
        files.insert(1, big);
        small_pin_answer_ = false;
        // Declined, the one picture large enough still pins.
        copy_files(files);
        QVERIFY(clipboard::pin_latest_clipboard_image(QPoint(400, 300)));
        QTRY_COMPARE(pins().size(), 1);
        QTest::qWait(200);
        QCOMPARE(pins().size(), 1);
        QCOMPARE(pins()[0]->orig_size(), QSize(40, 30));
        QCOMPARE(small_pin_asked_, 1);
        // Accepted, every one does; the large one is pinned already.
        small_pin_asked_ = 0;
        small_pin_answer_ = true;
        QVERIFY(clipboard::pin_latest_clipboard_image(QPoint(400, 300)));
        QTRY_COMPARE(pins().size(), 4);
        QCOMPARE(small_pin_asked_, 1);
    }

    void aSmallCopiedImageAsksBeforePinning() {
        QImage image(29, 60, QImage::Format_RGB32);
        image.fill(Qt::green);
        small_pin_answer_ = false;
        // Declining is an answer, not a failure to pin.
        QVERIFY(clipboard::create_pin_from_image(image));
        QCOMPARE(small_pin_asked_, 1);
        QVERIFY(pins().isEmpty());
        // A side of 30 is large enough.
        QVERIFY(clipboard::create_pin_from_image(image.copy(0, 0, 24, 30).scaled(30, 30)));
        QCOMPARE(pins()[0]->orig_size(), QSize(30, 30));
        QCOMPARE(small_pin_asked_, 1);
        QCOMPARE(pins().size(), 1);
    }

    void aRepeatShakesThePinShowingItInstead() {
        QImage image(40, 30, QImage::Format_RGB32);
        image.fill(Qt::green);
        // Pinned by a capture counts too.
        auto *picture = PinManager::instance()->create_pin(image, QPoint(100, 100), config_);
        // The same pixels in another format are the same picture.
        QVERIFY(clipboard::create_pin_from_image(image.convertToFormat(QImage::Format_ARGB32)));
        QCOMPARE(pins().size(), 1);
        QVERIFY(picture->shaking());
        QImage other = image;
        other.setPixel(5, 5, qRgb(0, 0, 0));
        QVERIFY(clipboard::create_pin_from_image(other));
        QCOMPARE(pins().size(), 2);
        // A text whatever its formatting.
        QVERIFY(clipboard::create_pin_from_text("same words", {}));
        QTRY_COMPARE(pins().size(), 3);
        auto *text = pins()[2];
        QVERIFY(clipboard::create_pin_from_text("same words", "<b>same words</b>"));
        QVERIFY(text->shaking());
        // The same files, and the same animation.
        copy_files({write("notes.txt", "notes")});
        QVERIFY(clipboard::pin_latest_clipboard_image(QPoint(400, 300)));
        QTRY_COMPARE(pins().size(), 4);
        auto *card = pins()[3];
        QVERIFY(clipboard::pin_latest_clipboard_image(QPoint(400, 300)));
        QTRY_VERIFY(card->shaking());
        copy_files({three_frames()});
        QVERIFY(clipboard::pin_latest_clipboard_image(QPoint(400, 300)));
        QTRY_COMPARE(pins().size(), 5);
        auto *animation = pins()[4];
        QVERIFY(clipboard::pin_latest_clipboard_image(QPoint(400, 300)));
        QTRY_VERIFY(animation->shaking());
        QTest::qWait(100);
        QCOMPARE(pins().size(), 5);
    }

    void withRepeatsPinnedAgainEveryPinOpens() {
        config_->set_pin_point_out_repeats(false);
        QImage image(40, 30, QImage::Format_RGB32);
        image.fill(Qt::green);
        QVERIFY(clipboard::create_pin_from_image(image));
        QVERIFY(clipboard::create_pin_from_image(image));
        QCOMPARE(pins().size(), 2);
        QVERIFY(!pins()[0]->shaking());
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
        QVERIFY(clipboard::pin_latest_clipboard_image(QPoint(100, 100)));
        QTRY_COMPARE(pins().size(), 1);
        auto *pin = pins()[0];
        QVERIFY(pin->width() < screen.width() && pin->height() < screen.height());
        // The pin scales it to the size it opens at, off the UI thread.
        QTRY_COMPARE(pin->canvas->scene->background->pixmap().size(), pin->size());
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

    void historyItemsPinInOrderWithTheirFilesAsCopied() {
        auto &manager = clipboard::ClipboardManager::instance();
        const QString database = QDir::toNativeSeparators(dir_.filePath("history/clipboard.db"));
        config_->set_clipboard_db_path(database);
        QVERIFY(manager.reset_storage(database));
        const auto images = manager.get_images_dir();
        QVERIFY(images && QDir().mkpath(*images));
        QImage stored(QSize(7, 5), QImage::Format_RGB32);
        stored.fill(Qt::red);
        QVERIFY(stored.save(QDir(*images).filePath("stored_one.png")));
        clipboard::ClipboardItem image, blank, files, text;
        image.content_type = "image";
        image.image_id = "stored_one";
        blank.content_type = text.content_type = "text";
        blank.content = "  ";
        text.content = "some words";
        const auto notes = write("notes.txt", "notes");
        const auto still = picture("still.png", QSize(3, 2), Qt::blue);
        files.content_type = "file";
        files.content = QString::fromUtf8(QJsonDocument(QJsonObject{{"files", QJsonArray{notes, still}}}).toJson());
        QVERIFY(clipboard::create_pins_from_clipboard_items({image, blank, files, text}, nullptr, QPoint(400, 300)));
        // The other files on a card first, then the stored image, the picture file and the text in their order.
        QTRY_COMPARE(pins().size(), 4);
        QVERIFY(pins()[0]->file_card());
        QCOMPARE(pins()[0]->file_card()->files(), QStringList{notes});
        QCOMPARE(pins()[1]->orig_size(), QSize(7, 5));
        QCOMPARE(pins()[1]->canvas->scene->background->pixmap().toImage().pixelColor(1, 1), QColor(Qt::red));
        QCOMPARE(pins()[2]->orig_size(), QSize(3, 2));
        QVERIFY(pins()[3]->text_only());
        QCOMPARE(pins()[3]->text_content()->text(), QString("some words"));
        QTest::qWait(100);
        QCOMPARE(pins().size(), 4);
        QVERIFY(!clipboard::create_pins_from_clipboard_items({blank}, nullptr));
    }

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
        for (const char *entry : {"Open", "Copy this file", "Copy this file's path", "Show in Explorer", "Copy image", "Save as", "Toolbar", "Hide annotations"})
            QVERIFY2(has_entry(entries, entry), entry);
        for (const char *entry : {"Copy all files", "Copy plain text", "Translate", "Image transform", "Text selection", "Text mode"})
            QVERIFY2(!has_entry(entries, entry), entry);
        QCOMPARE(QApplication::clipboard()->text(), QDir::toNativeSeparators(b));
        // The right-click selects nothing and puts nothing into the scene; the copy key still copies the image.
        QCOMPARE(open_highlights, 0);
        QVERIFY(pin->file_selection()->selected_files().isEmpty());
        QVERIFY(entries.contains(QString("Copy this file")));
        // On a selected file the menu acts on the selection, which the copy key takes.
        const auto ctrl_click = [&](int index) { click_at(pin, pin->view->mapFromScene(QRectF(card.cell_rect(index)).center()), Qt::ControlModifier); };
        ctrl_click(0);
        ctrl_click(1);
        entries = run_menu(pin, global(card.cell_rect(1).center()), "Copy selected file paths");
        for (const char *entry : {"Open", "Copy selected files", "Show in Explorer"}) QVERIFY2(has_entry(entries, entry), entry);
        QVERIFY(!has_entry(entries, "Copy this file"));
        QVERIFY(entries.contains(QString("Copy image")));
        QCOMPARE(QApplication::clipboard()->text(), QDir::toNativeSeparators(QStringList({a, b}).join('\n')));
        // On another file, that file alone, and the selection stays.
        entries = run_menu(pin, global(card.cell_rect(2).center()), "Copy this file's path");
        QVERIFY(entries.contains(QString("Copy this file")));
        QCOMPARE(QApplication::clipboard()->text(), QDir::toNativeSeparators(c));
        QCOMPARE(pin->file_selection()->selected_files(), (QStringList{a, b}));
        // The empty cell beside the last card: all the files, and nothing stays selected.
        entries = run_menu(pin, global(card.cell_rect(3).center()), "Copy all file paths");
        QVERIFY(has_entry(entries, "Copy all files"));
        QVERIFY(!has_entry(entries, "Open"));
        QVERIFY(pin->file_selection()->selected_files().isEmpty());
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

    void filesOnACardAreSelectedAndOpenedAsInAFolder() {
        QStringList files;
        for (const char *name : {"a.txt", "b.txt", "c.txt", "d.txt"}) files.append(write(name, name));
        const QString a = files[0], b = files[1], c = files[2], d = files[3];
        // Three columns, two rows: the last two cells are empty.
        const PinFileCard card(files, QSize(3 * PinFileCard::CELL_WIDTH, 2 * PinFileCard::CELL_HEIGHT));
        auto *pin = PinManager::instance()->create_file_pin(card, QPoint(100, 100), config_);
        auto *selection = pin->file_selection();
        const auto at = [pin, &card](int index) { return pin->view->mapFromScene(QRectF(card.cell_rect(index)).center()); };
        const QImage clean = pin->get_current_image();
        // A plain click selects nothing; Ctrl adds and drops; Shift takes the run from the last one Ctrl+clicked.
        click_at(pin, at(1));
        QVERIFY(selection->selected_files().isEmpty());
        click_at(pin, at(1), Qt::ControlModifier);
        QCOMPARE(selection->selected_files(), QStringList{b});
        click_at(pin, at(3), Qt::ControlModifier);
        QCOMPARE(selection->selected_files(), (QStringList{b, d}));
        click_at(pin, at(1), Qt::ControlModifier);
        QCOMPARE(selection->selected_files(), QStringList{d});
        // The run starts at the file Ctrl+clicked last, and replaces what was selected.
        click_at(pin, at(0), Qt::ShiftModifier);
        QCOMPARE(selection->selected_files(), (QStringList{a, b}));
        click_at(pin, at(3), Qt::ShiftModifier | Qt::ControlModifier);
        QCOMPARE(selection->selected_files(), (QStringList{a, b, c, d}));
        // A plain click on a file clears the selection.
        click_at(pin, at(2));
        QVERIFY(selection->selected_files().isEmpty());
        click_at(pin, at(2), Qt::ControlModifier);
        click_at(pin, at(0), Qt::ShiftModifier);
        QCOMPARE(selection->selected_files(), (QStringList{a, b, c}));
        // The highlight stays out of what is copied and saved.
        QCOMPARE(pin->get_current_image(), clean);
        // Moving the pin keeps the selection.
        const QPoint before = pin->pos();
        QVERIFY(!drag_from(pin, at(3), Qt::NoModifier));
        QCOMPARE(pin->pos(), before + QPoint(30, 0));
        QCOMPARE(selection->selected_files(), (QStringList{a, b, c}));
        // The copy keys take the selection.
        pin->copy_to_clipboard();
        QCOMPARE(QApplication::clipboard()->mimeData()->urls(), QList<QUrl>({QUrl::fromLocalFile(a), QUrl::fromLocalFile(b), QUrl::fromLocalFile(c)}));
        pin->copy_all_text();
        QCOMPARE(QApplication::clipboard()->text(), QDir::toNativeSeparators(QStringList({a, b, c}).join('\n')));
        // Enter opens them, a double click the file under the pointer.
        QDesktopServices::setUrlHandler("file", this, "record_opened");
        opened_.clear();
        QTest::keyClick(pin->view, Qt::Key_Return);
        QCOMPARE(opened_, (QStringList{a, b, c}));
        opened_.clear();
        double_click_at(pin, at(3));
        QCOMPARE(opened_, QStringList{d});
        QCOMPARE(pin->pos(), before + QPoint(30, 0));
        // A plain click off the cards clears too; a locked pin still selects.
        click_at(pin, at(5));
        QVERIFY(selection->selected_files().isEmpty());
        pin->set_locked(true);
        click_at(pin, at(0), Qt::ControlModifier);
        QCOMPARE(selection->selected_files(), QStringList{a});
        pin->set_locked(false);
        // Ctrl+A selects every file; while drawing the selection is hidden and has no effect.
        QTest::keyClick(pin->view, Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(selection->selected_files(), files);
        QVERIFY(pin->canvas->activate_tool("pen"));
        QVERIFY(selection->selected_files().isEmpty());
        opened_.clear();
        QTest::keyClick(pin->view, Qt::Key_Return);
        QVERIFY(opened_.isEmpty());
        pin->canvas->deactivate_tool();
        QCOMPARE(selection->selected_files(), files);
        QDesktopServices::unsetUrlHandler("file");
    }

    void filesTheCardOnlyCountsCanBeSelectedTooAndManyAskBeforeOpening() {
        QStringList files;
        for (int i = 0; i < 20; ++i) files.append(write(QString("f%1.txt").arg(i), "x"));
        // One row of two cells: the first file and the count of all twenty.
        const PinFileCard card(files, QSize(2 * PinFileCard::CELL_WIDTH, PinFileCard::CELL_HEIGHT));
        QCOMPARE(card.shown(), 1);
        auto *pin = PinManager::instance()->create_file_pin(card, QPoint(100, 100), config_);
        auto *selection = pin->file_selection();
        // The count cell is no file.
        click_at(pin, pin->view->mapFromScene(QRectF(card.cell_rect(1)).center()));
        QVERIFY(selection->selected_files().isEmpty());
        QTest::keyClick(pin->view, Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(selection->selected_files(), files);
        QCOMPARE(selection->selected_bounds(), QRectF(QRect(QPoint(0, 0), card.size())));
        QDesktopServices::setUrlHandler("file", this, "record_opened");
        opened_.clear();
        bool asked = false;
        QTimer::singleShot(0, this, [&asked] {
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                asked = true;
                dialog->reject();
            }
        });
        QTest::keyClick(pin->view, Qt::Key_Return);
        QDesktopServices::unsetUrlHandler("file");
        QVERIFY(asked);
        QVERIFY(opened_.isEmpty());
    }

    // ---- Dragging content out ----

    void ctrlDraggingAPinDragsItsContentOutInsteadOfMovingIt() {
        QCOMPARE(get_pin_mouse_binding(config_, "drag_out"), QString("ctrl+dragleft"));
        QImage image(QSize(300, 200), QImage::Format_RGB32);
        image.fill(Qt::darkGreen);
        auto *pin = PinManager::instance()->create_pin(image, QPoint(100, 100), config_);
        const QPoint before = pin->pos();
        QDrag *drag = drag_from(pin, QPoint(40, 50), Qt::ControlModifier);
        QVERIFY(drag);
        QCOMPARE(pin->pos(), before);
        QCOMPARE(drag->supportedActions(), Qt::CopyAction);
        QVERIFY(drag->mimeData()->hasImage());
        QVERIFY(drag->mimeData()->hasFormat(DragOutMimeData::FILES_MIME));
        // A URL list would make Word insert a link instead of the picture.
        QVERIFY(!drag->mimeData()->hasUrls());
        // The pin shrunk to fit the preview, held at the point it was grabbed.
        const QSize shown = pin->view->viewport()->size();
        const double factor = 240.0 / std::max(shown.width(), shown.height());
        QCOMPARE(drag->pixmap().deviceIndependentSize().toSize(), QSize(qRound(shown.width() * factor), qRound(shown.height() * factor)));
        QCOMPARE(drag->hotSpot(), QPoint(qRound(40 * factor), qRound(50 * factor)));
        // At 80% opacity, so where it will land shows through.
        QVERIFY(std::abs(max_alpha(drag->pixmap()) - 204) <= 1);
        const QColor middle = drag->pixmap().toImage().pixelColor(drag->pixmap().rect().center());
        QVERIFY(middle.red() <= 1 && std::abs(middle.green() - 128) <= 1 && middle.blue() <= 1);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        // A short press is no drag, and does nothing else either.
        send_left(pin, QEvent::MouseButtonPress, QPoint(40, 50), Qt::LeftButton, Qt::ControlModifier);
        send_left(pin, QEvent::MouseButtonRelease, QPoint(40, 50), Qt::NoButton, Qt::ControlModifier);
        QVERIFY(pin->findChildren<QDrag *>().isEmpty());
        QVERIFY(!pin->is_closed());
        // Without Ctrl the pin moves as before.
        QVERIFY(!drag_from(pin, QPoint(40, 50), Qt::NoModifier));
        QCOMPARE(pin->pos(), before + QPoint(30, 0));
    }

    void theDragOutGestureCanBeChangedOrTurnedOff() {
        auto *pin = PinManager::instance()->create_pin(QImage(QSize(300, 200), QImage::Format_RGB32), QPoint(100, 100), config_);
        config_->set_app_setting("mouse_pin_drag_out", "shift+dragleft");
        const QPoint before = pin->pos();
        QVERIFY(!drag_from(pin, QPoint(40, 50), Qt::ControlModifier));
        QCOMPARE(pin->pos(), before + QPoint(30, 0));
        QVERIFY(drag_from(pin, QPoint(40, 50), Qt::ShiftModifier));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        config_->set_app_setting("mouse_pin_drag_out", "");
        QVERIFY(!drag_from(pin, QPoint(40, 50), Qt::ShiftModifier));
    }

    void ctrlDragDrawsWhileDrawingAndDragsOutOfALockedPin() {
        auto *pin = PinManager::instance()->create_pin(QImage(QSize(300, 200), QImage::Format_RGB32), QPoint(100, 100), config_);
        QVERIFY(pin->canvas->activate_tool("pen"));
        QVERIFY(!drag_from(pin, QPoint(40, 50), Qt::ControlModifier));
        pin->canvas->deactivate_tool();
        // Locking keeps the pin where it is; dragging its content out leaves it there.
        pin->set_locked(true);
        const QPoint before = pin->pos();
        QVERIFY(drag_from(pin, QPoint(40, 50), Qt::ControlModifier));
        QCOMPARE(pin->pos(), before);
    }

    void anImagePinGivesItsAnnotatedImageAndAFileOnlyWhenAsked() {
        QImage image(QSize(120, 80), QImage::Format_RGB32);
        image.fill(Qt::blue);
        auto *pin = PinManager::instance()->create_pin(image, QPoint(100, 100), config_);
        pin->canvas->scene->addRect(QRectF(10, 10, 20, 20), Qt::NoPen, QBrush(Qt::red));
        const QString root = dir_.filePath("image_drag");
        DragOutMimeData data(pin->prepare_drag_out(viewport_global(pin, QPoint(60, 40))).content, root);
        QCOMPARE(data.formats(), (QStringList{DragOutMimeData::FILES_MIME, "application/x-qt-image"}));
        const QImage dragged = qvariant_cast<QImage>(data.imageData());
        QCOMPARE(dragged.size(), QSize(120, 80));
        QCOMPARE(dragged.pixelColor(20, 20), QColor(Qt::red));
        QCOMPARE(dragged.pixelColor(90, 60), QColor(Qt::blue));
        // Written for Explorer only once it asks.
        QVERIFY(!QDir(root).exists());
        const QStringList files = data.file_paths();
        QCOMPARE(files.size(), 1);
        QVERIFY(QFileInfo(files[0]).fileName().startsWith("pinned_"));
        QCOMPARE(QFileInfo(files[0]).suffix(), QString("png"));
        const QImage written(files[0]);
        QCOMPARE(written.size(), QSize(120, 80));
        QCOMPARE(written.pixelColor(20, 20), QColor(Qt::red));
        // In the format auto-save uses.
        config_->set_screenshot_format("JPG");
        DragOutMimeData jpg(pin->prepare_drag_out(viewport_global(pin, QPoint(60, 40))).content, root);
        const QString jpg_file = jpg.file_paths().value(0);
        QCOMPARE(QFileInfo(jpg_file).suffix(), QString("jpg"));
        QCOMPARE(QImageReader(jpg_file).format(), QByteArray("jpeg"));
        QCOMPARE(QImage(jpg_file).size(), QSize(120, 80));
    }

    void aDragStartingOnHighlightedTextCarriesTheText() {
        auto *pin = PinManager::instance()->create_pin(QImage(QSize(200, 120), QImage::Format_RGB32), QPoint(100, 100), config_);
        OcrResult result;
        result.code = 100;
        result.data = {OcrLine{{QPointF(10, 10), QPointF(110, 10), QPointF(110, 30), QPointF(10, 30)}, "hello", 0.9}};
        pin->ocr_manager()->load_text(OcrTextLayout(result));
        pin->ocr_manager()->set_text_selection_enabled(true);
        send_left(pin, QEvent::MouseButtonPress, QPoint(12, 20), Qt::LeftButton, Qt::NoModifier);
        send_left(pin, QEvent::MouseMove, QPoint(108, 20), Qt::LeftButton, Qt::NoModifier);
        send_left(pin, QEvent::MouseButtonRelease, QPoint(108, 20), Qt::NoButton, Qt::NoModifier);
        QCOMPARE(pin->text_selection()->selected_text(), QString("hello"));
        const QString root = dir_.filePath("text_drag");
        auto on_selection = pin->prepare_drag_out(viewport_global(pin, QPoint(60, 20)));
        // The preview is the highlighted text alone: the box of (10, 10)-(110, 30), held where it was grabbed.
        const QSize shown = on_selection.preview.deviceIndependentSize().toSize();
        QVERIFY2(std::abs(shown.width() - 100) <= 1 && std::abs(shown.height() - 20) <= 1, qPrintable(QString("%1x%2").arg(shown.width()).arg(shown.height())));
        QVERIFY((on_selection.hot_spot - QPoint(50, 10)).manhattanLength() <= 2);
        DragOutMimeData on_text(std::move(on_selection.content), root);
        QCOMPARE(on_text.formats(), (QStringList{DragOutMimeData::FILES_MIME, "text/plain"}));
        QCOMPARE(on_text.text(), QString("hello"));
        QCOMPARE(read_all(on_text.file_paths().value(0)), QByteArray("hello"));
        // Elsewhere the drag takes the image, selection or not.
        DragOutMimeData off_text(pin->prepare_drag_out(viewport_global(pin, QPoint(60, 90))).content, root);
        QVERIFY(off_text.hasImage());
        QVERIFY(!off_text.hasText());
        QVERIFY(pin->text_selection()->has_selection());
        // A Ctrl+drag on the text leaves the selection as it was.
        QDrag *drag = drag_from(pin, QPoint(60, 20), Qt::ControlModifier);
        QVERIFY(drag);
        QCOMPARE(drag->mimeData()->text(), QString("hello"));
        QCOMPARE(pin->text_selection()->selected_text(), QString("hello"));
    }

    void aTextPinDragsItsSelectedTextWithThatPartAsThePreview() {
        auto *pin = PinManager::instance()->create_text_pin(std::make_unique<PinTextContent>("first line\nsecond line", QString(), false), QPoint(100, 100), config_);
        auto *selection = pin->text_selection();
        QVERIFY(selection);
        const auto &lines = selection->layout().lines();
        QCOMPARE(static_cast<int>(lines.size()), 2);
        // Across the first line, in the view's coordinates whatever the font.
        const QRectF first = lines[0].bounds;
        const auto at = [pin](const QPointF &scene_pos) { return pin->view->mapFromScene(scene_pos); };
        send_left(pin, QEvent::MouseButtonPress, at(QPointF(first.left() + 1, first.center().y())), Qt::LeftButton, Qt::NoModifier);
        send_left(pin, QEvent::MouseMove, at(QPointF(first.right() - 1, first.center().y())), Qt::LeftButton, Qt::NoModifier);
        send_left(pin, QEvent::MouseButtonRelease, at(QPointF(first.right() - 1, first.center().y())), Qt::NoButton, Qt::NoModifier);
        QCOMPARE(selection->selected_text(), QString("first line"));
        const auto drag = pin->prepare_drag_out(viewport_global(pin, at(first.center())));
        DragOutMimeData data(drag.content, dir_.filePath("text_selection_drag"));
        QCOMPARE(data.text(), QString("first line"));
        QVERIFY(!data.hasHtml());
        // The first line's highlight, not the page.
        const QSize shown = drag.preview.deviceIndependentSize().toSize();
        const QRect line = pin->view->mapFromScene(first).boundingRect();
        QVERIFY2(shown.width() <= line.width() + 1 && shown.width() >= line.width() / 2 && shown.height() <= line.height() + 1,
                 qPrintable(QString("%1x%2 in %3x%4").arg(shown.width()).arg(shown.height()).arg(line.width()).arg(line.height())));
        QVERIFY(shown.height() * 2 < pin->view->viewport()->height());
        // Elsewhere on the page the whole text goes, with the whole page as the preview.
        const auto whole = pin->prepare_drag_out(viewport_global(pin, at(QPointF(lines[1].bounds.center()))));
        QCOMPARE(DragOutMimeData(whole.content, dir_.filePath("text_selection_drag")).text(), QString("first line\nsecond line"));
        QVERIFY(whole.preview.deviceIndependentSize().height() > shown.height() * 2);
    }

    void aTextPinGivesItsTextItsFormattingAndATextFile() {
        const QString text = QString::fromUtf8("加粗 words");
        auto *pin = PinManager::instance()->create_text_pin(std::make_unique<PinTextContent>(text, QString::fromUtf8("<b>加粗</b> words"), true), QPoint(100, 100), config_);
        const QString root = dir_.filePath("text_pin_drag");
        DragOutMimeData data(pin->prepare_drag_out(viewport_global(pin, QPoint(5, 5))).content, root);
        QCOMPARE(data.formats(), (QStringList{DragOutMimeData::FILES_MIME, "text/plain", "text/html"}));
        QCOMPARE(data.text(), text);
        QVERIFY(data.html().contains("<b>") || data.html().contains("font-weight"));
        QVERIFY(data.html().contains(QString::fromUtf8("加粗")));
        const QStringList files = data.file_paths();
        QCOMPARE(files.size(), 1);
        QCOMPARE(QFileInfo(files[0]).suffix(), QString("txt"));
        QCOMPARE(read_all(files[0]), text.toUtf8());
        // Shown without its formatting, it gives none.
        auto *plain = PinManager::instance()->create_text_pin(std::make_unique<PinTextContent>(text, QString::fromUtf8("<b>加粗</b> words"), false), QPoint(100, 300), config_);
        DragOutMimeData unformatted(plain->prepare_drag_out(viewport_global(plain, QPoint(5, 5))).content, root);
        QCOMPARE(unformatted.formats(), (QStringList{DragOutMimeData::FILES_MIME, "text/plain"}));
    }

    void anAnimationGivesItsFileAndBytes() {
        const auto original = three_frames();
        const QByteArray bytes = read_all(original);
        auto source = load_animation(original);
        QVERIFY(source);
        auto *pin = PinManager::instance()->create_animation_pin(std::move(*source), QPoint(100, 100), config_);
        const QString root = dir_.filePath("animation_drag");
        auto data = std::make_unique<DragOutMimeData>(pin->prepare_drag_out(viewport_global(pin, QPoint(2, 2))).content, root);
        QCOMPARE(data->formats(), (QStringList{DragOutMimeData::FILES_MIME, "image/gif"}));
        QCOMPARE(data->data("image/gif"), bytes);
        QVERIFY(!QDir(root).exists());
        const QStringList files = data->file_paths();
        QCOMPARE(files.size(), 1);
        QCOMPARE(QFileInfo(files[0]).fileName(), QString("three.gif"));
        QVERIFY(files[0] != pin->animation()->source().path());
        QCOMPARE(read_all(files[0]), bytes);
        // A target copying after the drop still finds it once the pin and its own copy are gone.
        pin->close_window();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(read_all(files[0]), bytes);
    }

    void aFileCardDragsTheSelectedFilesTheFileUnderThePointerOrEveryFile() {
        const auto a = write("a.txt", "a");
        const auto b = write("b.txt", "b");
        const auto c = write("c.txt", "c");
        const auto d = write("d.txt", "d");
        // Three columns, two rows: the last two cells are empty.
        const PinFileCard card({a, b, c, d}, QSize(800, 400));
        auto *pin = PinManager::instance()->create_file_pin(card, QPoint(100, 100), config_);
        const auto global = [pin](const QPointF &scene_pos) { return pin->view->viewport()->mapToGlobal(pin->view->mapFromScene(scene_pos)); };
        const QString root = dir_.filePath("file_drag");
        const auto on_b = pin->prepare_drag_out(global(QRectF(card.cell_rect(1)).center()));
        DragOutMimeData one(on_b.content, root);
        QCOMPARE(one.formats(), QStringList{DragOutMimeData::FILES_MIME});
        QCOMPARE(one.file_paths(), QStringList{b});
        // The preview is that card alone, and the drag selects nothing.
        const QSize cell = pin->view->mapFromScene(QRectF(card.cell_rect(1))).boundingRect().size();
        QCOMPARE(on_b.preview.deviceIndependentSize().toSize().width(), std::min(cell.width(), 240));
        QVERIFY(pin->file_selection()->selected_files().isEmpty());
        // From a selected file, the whole selection goes.
        pin->file_selection()->click(1, Qt::ControlModifier);
        pin->file_selection()->click(3, Qt::ControlModifier);
        DragOutMimeData selected(pin->prepare_drag_out(global(QRectF(card.cell_rect(3)).center())).content, root);
        QCOMPARE(selected.file_paths(), (QStringList{b, d}));
        // From another file, that file alone, and the selection stays.
        DragOutMimeData other(pin->prepare_drag_out(global(QRectF(card.cell_rect(0)).center())).content, root);
        QCOMPARE(other.file_paths(), QStringList{a});
        QCOMPARE(pin->file_selection()->selected_files(), (QStringList{b, d}));
        // Off the cards, as the context menu does, every file.
        const QPointF blank = QRectF(card.cell_rect(5)).center();
        QCOMPARE(pin->file_at(global(blank)), -1);
        DragOutMimeData every(pin->prepare_drag_out(global(blank)).content, root);
        QCOMPARE(every.file_paths(), (QStringList{a, b, c, d}));
        // Nothing is written: the files are handed out where they are.
        QVERIFY(!QDir(root).exists());
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
            {"ClipboardWindow", "Pin to Screen"}, {"ClipboardWindow", "Pin to Top"}, {"ClipboardWindow", "Unpin from Top"},
            {"SettingsDialog", "Drag Out Content"}, {"PinWindow", "Copy selected files"}, {"PinWindow", "Copy selected file paths"},
            {"PinWindow", "Open Many Files"}, {"PinWindow", "Open %1 files at once?"},
            {"PinWindow", "Pin Too Small"}, {"PinWindow", "This pin is only %1 × %2 pixels. Pin it anyway?"},
        };
        for (const char *language : {"zh", "zh_TW", "en", "ja", "ko"}) {
            QTranslator translator;
            QVERIFY(translator.load(QString(":/translations/app_%1.qm").arg(language)));
            for (const auto &[context, text] : texts)
                QVERIFY2(!translator.translate(context, text).isEmpty(), qPrintable(QString("%1 %2: %3").arg(language, context, text)));
            // Side by side in the clipboard menu, putting an item on the screen and at the top of the list read apart.
            QVERIFY2(translator.translate("ClipboardWindow", "Pin to Screen") != translator.translate("ClipboardWindow", "Pin to Top"), language);
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
