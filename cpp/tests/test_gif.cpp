#include "capture/capture_service.h"
#include "canvas/items/drawing_items.h"
#include "canvas/scene.h"
#include "gif/drawing_view.h"
#include "gif/_widgets.h"
#include "gif/cursor_overlay.h"
#include "gif/frame_recorder.h"
#include "gif/gifrecorder.h"
#include "gif/playback_controller.h"
#include "gif/playback_toolbar.h"
#include "gif/record_toolbar.h"
#include "gif/record_window.h"
#include "gif/video_recorder.h"
#include "core/ui_scale.h"
#include "settings/tool_settings.h"
#include "tools/controller.h"
#include "canvas/items/arrow_item.h"
#include "canvas/smart_edit_controller.h"
#include "gif/overlay.h"
#include "ui/arrow_settings_panel.h"
#include "ui/paint_settings_panel.h"
#include "ui/shape_settings_panel.h"
#include "ui/text_settings_panel.h"
#include "video/worker.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QInputDialog>
#include <QMenu>
#include <QSpinBox>
#include <QImageReader>
#include <QPainter>
#include <QPointer>
#include <QScopeGuard>
#include <QPushButton>
#include <QTimer>
#include <QClipboard>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QMimeData>
#include <QScreen>
#include <QFileInfo>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

using namespace jietuba;

namespace {
RecordingOptions silent_options() {
    RecordingOptions options;
    options.fps = 30;
    options.system_audio = false;
    return options;
}

QStringList real_worker() { return {QStringLiteral(JIETUBA_APP_PATH), "--video-recorder-worker"}; }

// A topmost window with a bar sweeping across it, so every recorded frame differs.
class AnimatedWindow : public QWidget {
public:
    AnimatedWindow() : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool) {
        setAttribute(Qt::WA_ShowWithoutActivating);
        connect(&timer_, &QTimer::timeout, this, [this] {
            step_ = (step_ + 7) % std::max(1, width());
            repaint();
        });
        timer_.start(30);
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(240, 240, 240));
        painter.fillRect(step_, 0, 24, height(), QColor(30, 90, 200));
    }
private:
    QTimer timer_;
    int step_ = 0;
};

// The visible top-level widget of type T.
template <typename T>
T *visible_top_level() {
    for (auto *widget : QApplication::topLevelWidgets())
        if (auto *typed = qobject_cast<T *>(widget); typed && typed->isVisible()) return typed;
    return nullptr;
}

QPushButton *button_with_tooltip(QWidget *parent, const char *tooltip) {
    for (auto *button : parent->findChildren<QPushButton *>())
        if (button->toolTip() == QString::fromUtf8(tooltip) && button->isVisible()) return button;
    return nullptr;
}

QPushButton *button_with_text(QWidget *parent, const char *text) {
    for (auto *button : parent->findChildren<QPushButton *>())
        if (button->text() == QString::fromUtf8(text) && button->isVisible()) return button;
    return nullptr;
}

// The delay button shows the time or countdown; its tooltip names the delay.
QPushButton *time_button(QWidget *toolbar) {
    for (auto *button : toolbar->findChildren<QPushButton *>())
        if (button->toolTip().startsWith(QString::fromUtf8("录制延迟")) && button->isVisible()) return button;
    return nullptr;
}

QRect centred_rect(const QSize &size) {
    const auto available = QGuiApplication::primaryScreen()->availableGeometry();
    return QRect(available.center() - QPoint(size.width() / 2, size.height() / 2), size);
}

// Clicks a button that pops up a menu and triggers the entry with this text.
bool pick_from_menu(QPushButton *button, const QString &text) {
    if (!button) return false;
    button->click();
    auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if (!menu) return false;
    for (auto *action : menu->actions()) {
        if (action->text() == text) {
            action->trigger();
            menu->close();
            return true;
        }
    }
    menu->close();
    return false;
}

template <typename T>
T *visible_child(QWidget *parent) {
    for (auto *child : parent->findChildren<T *>())
        if (child->isVisible()) return child;
    return nullptr;
}

// The visible dialog showing this message.
QDialog *message_dialog(const QString &part) {
    for (auto *widget : QApplication::topLevelWidgets()) {
        auto *dialog = qobject_cast<QDialog *>(widget);
        if (!dialog || !dialog->isVisible()) continue;
        for (auto *label : dialog->findChildren<QLabel *>())
            if (label->text().contains(part)) return dialog;
    }
    return nullptr;
}

void send_mouse(QWidget *target, QEvent::Type type, const QPoint &global, Qt::MouseButtons buttons) {
    QMouseEvent event(type, QPointF(target->mapFromGlobal(global)), QPointF(global), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, Qt::NoModifier);
    QApplication::sendEvent(target, &event);
}

GifDrawingView *drawing_view() {
    for (auto *widget : QApplication::topLevelWidgets())
        if (auto *view = qobject_cast<GifDrawingView *>(widget); view && view->isVisible()) return view;
    return nullptr;
}
}

// Records the URLs the application asks the desktop to open.
class UrlCatcher : public QObject {
    Q_OBJECT
public:
    QList<QUrl> urls;
public slots:
    void open(const QUrl &url) { urls << url; }
};

class GifTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir settings_dir_;
    QSettings *settings_ = nullptr;
private slots:
    void initTestCase() {
        QVERIFY(settings_dir_.isValid());
        // The settings singleton reads a throwaway file, never the registry.
        settings_ = new QSettings(settings_dir_.filePath("settings.ini"), QSettings::IniFormat, this);
        QCOMPARE(&get_tool_settings_manager(settings_)->settings(), settings_);
        get_tool_settings_manager()->set_screenshot_save_path(settings_dir_.path());
    }
    void cleanupTestCase() { shutdown_hdr_session(); }

    // A failed test can leave its window open; the next one must not find its toolbar.
    void cleanup() {
        if (auto window = active_gif_window()) window->close_all();
        QTRY_VERIFY_WITH_TIMEOUT(active_gif_window().isNull(), 30000);
        for (auto *widget : QApplication::topLevelWidgets())
            if (qobject_cast<QDialog *>(widget) && widget->isVisible()) widget->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    void failureMessagesMapCodes() {
        QCOMPARE(failure_message("disk_full", "raw"), QString::fromUtf8("保存位置空间不足，请释放空间或选择其他磁盘后重试。"));
        QCOMPARE(failure_message("release_timeout", "detail text"), QString("detail text"));
        QCOMPARE(failure_message("something_else", "raw"), QString::fromUtf8("视频录制失败，请重试。若问题持续，请查看日志中的详细错误。"));
    }

    void startRejectsMissingHelperExistingOutputAndTinyRegion() {
        QTemporaryDir dir;
        VideoRecorder missing(nullptr, {dir.filePath("missing.exe")});
        QSignalSpy missing_failed(&missing, &VideoRecorder::failed);
        QVERIFY(!missing.start(QRect(0, 0, 100, 100), dir.filePath("a.mp4"), silent_options()));
        QCOMPARE(missing_failed.at(0).at(0).toString(), QString("helper_missing"));

        VideoRecorder recorder(nullptr, real_worker());
        QSignalSpy failed(&recorder, &VideoRecorder::failed);
        QFile existing(dir.filePath("exists.mp4"));
        QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.close();
        QVERIFY(!recorder.start(QRect(0, 0, 100, 100), existing.fileName(), silent_options()));
        QCOMPARE(failed.at(0).at(0).toString(), QString("output_exists"));
        QVERIFY(!recorder.start(QRect(0, 0, 3, 3), dir.filePath("tiny.mp4"), silent_options()));
        QCOMPARE(failed.at(1).at(0).toString(), QString("invalid_region"));
        QVERIFY(!recorder.active());
    }

    void malformedStatusFailsAsProtocol() {
        QTemporaryDir dir;
        // Prints one line that is not JSON, then exits.
        VideoRecorder recorder(nullptr, {"C:/Windows/System32/cmd.exe", "/c", "echo", "not-json"});
        QSignalSpy failed(&recorder, &VideoRecorder::failed);
        QSignalSpy finished(&recorder, &VideoRecorder::finished);
        QVERIFY(recorder.start(QRect(0, 0, 100, 100), dir.filePath("p.mp4"), silent_options()));
        QVERIFY(finished.wait(10000));
        QCOMPARE(failed.at(0).at(0).toString(), QString("protocol"));
        QVERIFY(!recorder.active());
    }

    // Records through the real worker process: ready, pause, resume, stop, complete.
    void workerRecordsPausesAndCompletes() {
        QTemporaryDir dir;
        const QString output = dir.filePath(QString::fromUtf8("录制 测试.mp4"));
        VideoRecorder recorder(nullptr, real_worker());
        QSignalSpy ready(&recorder, &VideoRecorder::ready);
        QSignalSpy paused(&recorder, &VideoRecorder::paused_changed);
        QSignalSpy completed(&recorder, &VideoRecorder::completed);
        QSignalSpy failed(&recorder, &VideoRecorder::failed);
        QSignalSpy finished(&recorder, &VideoRecorder::finished);
        QVERIFY(recorder.start(QRect(40, 40, 322, 242), output, silent_options()));
        QVERIFY(ready.wait(30000) || !failed.isEmpty());
        if (!failed.isEmpty() && failed.at(0).at(0).toString() == "capture") QSKIP("No capture device here.");
        QVERIFY2(failed.isEmpty(), qPrintable(failed.isEmpty() ? QString() : failed.at(0).at(1).toString()));
        QTest::qWait(400);
        QVERIFY(recorder.pause(true));
        QVERIFY(paused.wait(5000));
        QCOMPARE(paused.at(0).at(0).toBool(), true);
        QVERIFY(recorder.pause(false));
        QVERIFY(paused.wait(5000));
        QCOMPARE(paused.at(1).at(0).toBool(), false);
        QTest::qWait(300);
        recorder.stop();
        QVERIFY(finished.wait(30000));
        QVERIFY2(failed.isEmpty(), qPrintable(failed.isEmpty() ? QString() : failed.at(0).at(1).toString()));
        QCOMPARE(completed.size(), 1);
        QCOMPARE(QFileInfo(completed.at(0).at(0).toString()).absoluteFilePath(), QFileInfo(output).absoluteFilePath());
        QVERIFY(QFileInfo(output).size() > 0);
        QVERIFY(!recorder.active());
    }

    // The frame rate comes from the GIF setting or the MP4 options, by the saved format.
    void toolbarFrameRateFollowsSavedFormat() {
        auto *config = get_tool_settings_manager();
        config->set_app_setting("recording_format", "gif");
        config->set_gif_fps(24);
        {
            RecordToolbar toolbar;
            QCOMPARE(toolbar.get_recording_mode(), QString("gif"));
            QCOMPARE(toolbar.get_current_fps(), 24);
        }
        config->set_app_setting("recording_format", "mp4");
        config->set_app_setting("video_record_fps", 48);
        // A damaged delay falls back to none.
        config->set_app_setting("recording_delay", 7);
        RecordToolbar toolbar;
        QCOMPARE(toolbar.get_recording_mode(), QString("mp4"));
        QCOMPARE(toolbar.get_current_fps(), 48);
        QCOMPARE(toolbar.get_start_delay(), 0);
        config->set_app_setting("recording_format", "gif");
    }

    // The settings panels are windows of their own, without a parent; they go with the toolbar.
    void settingsPanelsGoWithTheToolbar() {
        QList<QPointer<QWidget>> panels;
        {
            RecordToolbar toolbar;
            for (QWidget *panel : std::initializer_list<QWidget *>{toolbar.paint_panel, toolbar.shape_panel, toolbar.arrow_panel, toolbar.text_panel})
                panels.append(panel);
        }
        QCOMPARE(panels.size(), 4);
        for (const auto &panel : panels) QVERIFY(!panel);
    }

    // Only windows over the recording area are left out, and only with the setting on; moving
    // the toolbar or changing the area decides again.
    void toolbarWindowsLeaveCapturesOnlyOverTheRecordingArea() {
        auto *config = get_tool_settings_manager();
        const bool saved = config->get_recording_hide_toolbar_in_captures();
        const auto restore = qScopeGuard([config, saved] { config->set_recording_hide_toolbar_in_captures(saved); });
        const auto affinity = [](QWidget *window) {
            DWORD value = 0xFFFF;
            GetWindowDisplayAffinity(reinterpret_cast<HWND>(window->winId()), &value);
            return value;
        };
        const DWORD left_out = WDA_EXCLUDEFROMCAPTURE, captured = WDA_NONE;
        const QRect screen = QGuiApplication::primaryScreen()->availableGeometry();
        const QRect small_area(screen.center(), QSize(200, 150));
        config->set_recording_hide_toolbar_in_captures(true);
        {
            RecordToolbar toolbar;
            toolbar.move(screen.topLeft() + QPoint(40, 40));
            toolbar.set_record_rect(small_area);
            toolbar.show();
            QVERIFY(QTest::qWaitForWindowExposed(&toolbar));
            QCOMPARE(affinity(&toolbar), captured);
            toolbar.move(small_area.topLeft());
            QCOMPARE(affinity(&toolbar), left_out);
            toolbar.move(screen.topLeft() + QPoint(40, 40));
            QCOMPARE(affinity(&toolbar), captured);
            // A full-screen area covers the toolbar where it is.
            toolbar.set_record_rect(screen);
            QCOMPARE(affinity(&toolbar), left_out);
            toolbar.paint_panel->move(screen.topLeft() + QPoint(40, 120));
            toolbar.paint_panel->show();
            QCOMPARE(affinity(toolbar.paint_panel), left_out);
            QMenu menu;
            menu.addAction("x");
            popup_menu(&menu, &toolbar);
            QCOMPARE(affinity(&menu), left_out);
            menu.close();
        }
        config->set_recording_hide_toolbar_in_captures(false);
        RecordToolbar toolbar;
        toolbar.set_record_rect(screen);
        toolbar.show();
        QVERIFY(QTest::qWaitForWindowExposed(&toolbar));
        QCOMPARE(affinity(&toolbar), captured);
        QMenu menu;
        menu.addAction("x");
        popup_menu(&menu, &toolbar);
        QCOMPARE(affinity(&menu), captured);
        menu.close();
    }

    // Over a solid backdrop in a full-screen area, both capture engines see the toolbar while the setting is off and
    // only the backdrop once it is on. The off case runs first, so a stale duplication frame
    // can only fail the on case, never pass it.
    void toolbarStaysOutOfScreenCaptures() {
        auto *config = get_tool_settings_manager();
        const bool saved = config->get_recording_hide_toolbar_in_captures();
        const auto restore = qScopeGuard([config, saved] { config->set_recording_hide_toolbar_in_captures(saved); });
        const QColor backdrop_color(255, 0, 255);
        const auto foreign_share = [&backdrop_color](const QImage &image) {
            const QImage x = image.convertToFormat(QImage::Format_RGB32);
            if (x.isNull()) return 1.0;
            qint64 foreign = 0;
            for (int row = 0; row < x.height(); ++row) {
                const auto *p = reinterpret_cast<const QRgb *>(x.constScanLine(row));
                for (int col = 0; col < x.width(); ++col)
                    foreign += std::abs(qRed(p[col]) - backdrop_color.red()) + std::abs(qGreen(p[col]) - backdrop_color.green()) +
                                   std::abs(qBlue(p[col]) - backdrop_color.blue()) > 24;
            }
            return double(foreign) / (qint64(x.width()) * x.height());
        };
        QScreen *screen = QGuiApplication::primaryScreen();
        QWidget backdrop;
        backdrop.setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
        backdrop.setAttribute(Qt::WA_ShowWithoutActivating);
        backdrop.setAutoFillBackground(true);
        QPalette palette = backdrop.palette();
        palette.setColor(QPalette::Window, backdrop_color);
        backdrop.setPalette(palette);
        for (bool hidden : {false, true}) {
            config->set_recording_hide_toolbar_in_captures(hidden);
            RecordToolbar toolbar;
            toolbar.move(screen->geometry().center() - QPoint(toolbar.width() / 2, toolbar.height() / 2));
            backdrop.setGeometry(toolbar.frameGeometry().adjusted(-20, -20, 20, 20));
            toolbar.set_record_rect(screen->geometry());
            backdrop.show();
            QVERIFY(QTest::qWaitForWindowExposed(&backdrop));
            toolbar.show();
            QVERIFY(QTest::qWaitForWindowExposed(&toolbar));
            toolbar.raise();
            const QRect logical = toolbar.frameGeometry();
            const qreal dpr = screen->devicePixelRatio();
            const QRect physical(QPoint(qRound(logical.x() * dpr), qRound(logical.y() * dpr)), QSize(qRound(logical.width() * dpr), qRound(logical.height() * dpr)));
            const QPoint origin = logical.topLeft() - screen->geometry().topLeft();
            const auto gdi = [&] { return foreign_share(screen->grabWindow(0, origin.x(), origin.y(), logical.width(), logical.height()).toImage()); };
            const auto dxgi = [&] { return foreign_share(grab_region_hdr(physical)); };
            if (!hidden) {
                QTRY_VERIFY_WITH_TIMEOUT(gdi() > 0.2, 3000);
                QTRY_VERIFY_WITH_TIMEOUT(dxgi() > 0.2, 3000);
                continue;
            }
            QTest::qWait(300);
            for (int sample = 0; sample < 3; ++sample) {
                const double g = gdi(), d = dxgi();
                QVERIFY2(g < 0.02, qPrintable(QString("GDI saw the toolbar: %1").arg(g)));
                QVERIFY2(d < 0.02, qPrintable(QString("DXGI saw the toolbar: %1").arg(d)));
                QTest::qWait(100);
            }
        }
    }

    // The window hands its capture session back when closed, and clears the global pointer.
    void recordWindowClosesAndClearsItself() {
        get_tool_settings_manager()->set_app_setting("recording_format", "gif");
        auto *window = new GifRecordWindow(QRect(60, 60, 240, 160));
        active_gif_window() = window;
        QVERIFY(window->overlay_visible());
        QVERIFY(!window->video_busy());
        QSignalSpy closed(window, &GifRecordWindow::closed);
        QSignalSpy destroyed(window, &QObject::destroyed);
        window->close_all();
        QCOMPARE(closed.size(), 1);
        QVERIFY(active_gif_window().isNull());
        QVERIFY(destroyed.wait(5000));
        // The session comes back in the background; captures work again once it has.
        QString last_error;
        const auto session_back = [&last_error] {
            try {
                return grab_region_hdr(QRect(0, 0, 32, 32)).size() == QSize(32, 32);
            } catch (const std::exception &e) {
                last_error = QString::fromUtf8(e.what());
                return false;
            }
        };
        QTRY_VERIFY2_WITH_TIMEOUT(session_back(), qPrintable(last_error), 10000);
    }

    // Recorded frames enter playback: the first frame is shown, the toolbar knows the length.
    void playbackShowsFirstFrameAndTearsDown() {
        FrameRecorder recorder;
        recorder.set_fps(15);
        recorder.set_rect(QRect(100, 100, 202, 122));
        recorder.start();
        QTest::qWait(500);
        auto &frames = recorder.stop();
        QVERIFY(!frames.empty());
        RecordToolbar toolbar;
        PlaybackController playback;
        QRect placed;
        playback.set_context(QRect(100, 100, 202, 122), &recorder, &toolbar, [&placed](QWidget *widget) { placed = widget->geometry(); });
        playback.start_playback(frames);
        QVERIFY(playback.is_active());
        QVERIFY(placed.isValid());
        QVERIFY(playback.preview_label());
        QVERIFY(!playback.preview_label()->pixmap().isNull());
        QCOMPARE(playback.playback_toolbar()->get_trim_range(), std::make_pair(0, static_cast<int>(frames.size()) - 1));
        playback.teardown();
        QVERIFY(!playback.is_active());
        QVERIFY(!playback.preview_label());
        toolbar.hide();
        const auto released = recorder.release();
        if (released.valid()) released.wait();
    }

    // Recording, stopping and releasing hands the session back as well.
    void recorderHandsSessionBackAfterRecording() {
        FrameRecorder recorder;
        recorder.set_fps(15);
        recorder.set_rect(QRect(100, 100, 322, 202));
        recorder.prepare();
        QTest::qWait(300);
        recorder.start();
        QTest::qWait(300);
        QSignalSpy stopped(&recorder, &FrameRecorder::stop_finished);
        recorder.stop_async();
        QVERIFY(stopped.wait(5000));
        QVERIFY(recorder.store());
        QVERIFY(recorder.store()->frame_count() > 0);
        const auto released = recorder.release();
        if (released.valid()) released.wait();
        QTRY_VERIFY_WITH_TIMEOUT([] {
            try {
                return !grab_region_hdr(QRect(0, 0, 16, 16)).isNull();
            } catch (const std::exception &) {
                return false;
            }
        }(), 10000);
    }

    // The background stop outlives the recorder: the window may close and delete it meanwhile.
    void backgroundStopOutlivesTheRecorder() {
        auto *recorder = new FrameRecorder;
        recorder->set_fps(15);
        recorder->set_rect(QRect(100, 100, 202, 122));
        recorder->start();
        QTest::qWait(300);
        const auto store = recorder->store();
        QVERIFY(store);
        recorder->stop_async();
        const auto released = recorder->release();
        delete recorder;
        if (released.valid()) released.wait();
        QTRY_COMPARE_WITH_TIMEOUT(store->state(), gifrecorder::STATE_STOPPED, 5000);
        QVERIFY(store->frame_count() > 0);
    }

    // The whole GIF path: record a moving picture, stop into playback, export and copy.
    void recordingEndsInPlaybackAndExportsAGif() {
        auto *config = get_tool_settings_manager();
        config->set_app_setting("recording_format", "gif");
        config->set_app_setting("recording_delay", 0);
        const auto available = QGuiApplication::primaryScreen()->availableGeometry();
        const QRect rect(available.center() - QPoint(160, 100), QSize(320, 200));
        AnimatedWindow page;
        page.setGeometry(rect);
        page.show();
        QTest::qWait(100);
        auto *window = new GifRecordWindow(rect);
        active_gif_window() = window;
        QSignalSpy closed(window, &GifRecordWindow::closed);
        RecordToolbar *toolbar = nullptr;
        QTRY_VERIFY((toolbar = visible_top_level<RecordToolbar>()));
        auto *record = button_with_tooltip(toolbar, "开始录制");
        QVERIFY(record);
        record->click();
        QTRY_VERIFY_WITH_TIMEOUT(button_with_tooltip(toolbar, "停止录制"), 3000);
        QTest::qWait(1000);
        button_with_tooltip(toolbar, "停止录制")->click();
        PlaybackToolbar *playback = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((playback = visible_top_level<PlaybackToolbar>()), 10000);
        const auto [first, last] = playback->get_trim_range();
        QCOMPARE(first, 0);
        QVERIFY2(last >= 5, qPrintable(QString::number(last)));
        const auto before = QDir(settings_dir_.path()).entryList({"*.gif"});
        auto *copy = button_with_tooltip(playback, "复制到剪贴板");
        QVERIFY(copy);
        copy->click();
        QTRY_COMPARE_WITH_TIMEOUT(closed.size(), 1, 30000);
        const auto after = QDir(settings_dir_.path()).entryList({"*.gif"});
        QCOMPARE(after.size(), before.size() + 1);
        QString gif;
        for (const auto &name : after)
            if (!before.contains(name)) gif = QDir(settings_dir_.path()).filePath(name);
        QImageReader reader(gif);
        QVERIFY2(reader.canRead(), qPrintable(reader.errorString()));
        // The recorder insets 1 px on every side to leave the selection border out.
        QCOMPARE(reader.size(), rect.size() - QSize(2, 2));
        QVERIFY2(reader.imageCount() >= 5, qPrintable(QString::number(reader.imageCount())));
        // Frames differ: the bar moved while recording.
        const auto frame_a = reader.read();
        reader.jumpToImage(reader.imageCount() - 1);
        const auto frame_b = reader.read();
        QVERIFY(frame_a != frame_b);
        // The clipboard holds the file and its GIF bytes.
        const auto *mime = QApplication::clipboard()->mimeData();
        QVERIFY(mime->hasFormat("image/gif") || mime->hasUrls());
        QTRY_VERIFY(active_gif_window().isNull());
    }

    // Drawing over the recording area goes through the toolbar's tools and undo.
    void drawingToolsDrawOnTheRecordingLayer() {
        get_tool_settings_manager()->set_app_setting("recording_format", "gif");
        const auto available = QGuiApplication::primaryScreen()->availableGeometry();
        const QRect rect(available.center() - QPoint(160, 100), QSize(320, 200));
        auto *window = new GifRecordWindow(rect);
        active_gif_window() = window;
        RecordToolbar *toolbar = nullptr;
        QTRY_VERIFY((toolbar = visible_top_level<RecordToolbar>()));
        GifDrawingView *view = nullptr;
        for (auto *widget : QApplication::allWidgets())
            if (auto *found = qobject_cast<GifDrawingView *>(widget)) view = found;
        QVERIFY(view);
        auto *rect_button = button_with_tooltip(toolbar, "矩形");
        QVERIFY(rect_button);
        rect_button->click();
        QVERIFY(rect_button->isChecked());
        QCOMPARE(view->canvas_scene->tool_controller->current_tool_id(), QString("rect"));
        const auto rects = [&] {
            int count = 0;
            for (auto *item : view->canvas_scene->items()) count += dynamic_cast<RectItem *>(item) != nullptr;
            return count;
        };
        auto *viewport = view->viewport();
        const auto send = [&](QEvent::Type type, const QPoint &pos, Qt::MouseButtons buttons) {
            QMouseEvent event(type, QPointF(pos), QPointF(viewport->mapToGlobal(pos)), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, Qt::NoModifier);
            QApplication::sendEvent(viewport, &event);
        };
        send(QEvent::MouseButtonPress, QPoint(40, 40), Qt::LeftButton);
        send(QEvent::MouseMove, QPoint(90, 80), Qt::LeftButton);
        send(QEvent::MouseMove, QPoint(140, 120), Qt::LeftButton);
        send(QEvent::MouseButtonRelease, QPoint(140, 120), Qt::NoButton);
        QCOMPARE(rects(), 1);
        button_with_tooltip(toolbar, "撤销")->click();
        QCOMPARE(rects(), 0);
        button_with_tooltip(toolbar, "重做")->click();
        QCOMPARE(rects(), 1);
        // Clicking the active tool again leaves drawing; the layer lets clicks through again.
        rect_button->click();
        QVERIFY(!rect_button->isChecked());
        QCOMPARE(view->canvas_scene->tool_controller->current_tool_id(), QString("cursor"));
        QSignalSpy closed(window, &GifRecordWindow::closed);
        button_with_tooltip(toolbar, "关闭")->click();
        QCOMPARE(closed.size(), 1);
        QTRY_VERIFY(active_gif_window().isNull());
    }

    // MP4 from the window: the save dialog, the worker started by this executable, pause,
    // stop, and the result row's actions.
    void mp4FromTheWindowSavesAndOffersTheResult() {
        auto *config = get_tool_settings_manager();
        config->set_app_setting("recording_format", "gif");
        config->set_app_setting("recording_delay", 0);
        silent_options().save_to_config(config);
        const QRect rect = centred_rect(QSize(320, 200));
        AnimatedWindow page;
        page.setGeometry(rect);
        page.show();
        auto *window = new GifRecordWindow(rect);
        active_gif_window() = window;
        QSignalSpy closed(window, &GifRecordWindow::closed);
        RecordToolbar *toolbar = nullptr;
        QTRY_VERIFY((toolbar = visible_top_level<RecordToolbar>()));
        QVERIFY(!button_with_tooltip(toolbar, "视频设置"));
        // MP4 shows the video options; back to GIF waits for the session to come back first.
        QVERIFY(pick_from_menu(button_with_tooltip(toolbar, "录制格式"), "MP4"));
        QCOMPARE(config->get_app_setting("recording_format").toString(), QString("mp4"));
        QVERIFY(button_with_tooltip(toolbar, "视频设置"));
        QVERIFY(pick_from_menu(button_with_tooltip(toolbar, "录制格式"), "GIF"));
        QTRY_COMPARE_WITH_TIMEOUT(time_button(toolbar)->text(), QString("00:00"), 10000);
        QVERIFY(button_with_tooltip(toolbar, "开始录制")->isEnabled());
        QVERIFY(pick_from_menu(button_with_tooltip(toolbar, "录制格式"), "MP4"));

        QTemporaryDir out;
        const auto open_dialog = [&]() -> QFileDialog * {
            button_with_tooltip(toolbar, "开始录制")->click();
            return toolbar->findChild<QFileDialog *>();
        };
        // Cancelling the save dialog gives the toolbar back.
        auto *dialog = open_dialog();
        QVERIFY(dialog && dialog->isVisible());
        QVERIFY(window->video_busy());
        QVERIFY(QFileInfo(dialog->selectedFiles().value(0)).fileName().startsWith("recording_"));
        dialog->reject();
        QVERIFY(!window->video_busy());
        QVERIFY(button_with_tooltip(toolbar, "开始录制")->isEnabled());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        // An existing file is refused.
        QFile existing(out.filePath("taken.mp4"));
        QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.close();
        dialog = open_dialog();
        QVERIFY(dialog);
        dialog->selectFile(existing.fileName());
        static_cast<QDialog *>(dialog)->accept();
        QTRY_VERIFY(message_dialog(QString::fromUtf8("目标文件已存在")));
        message_dialog(QString::fromUtf8("目标文件已存在"))->close();
        QVERIFY(button_with_tooltip(toolbar, "开始录制")->isEnabled());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

        const QString output = out.filePath("clip.mp4");
        dialog = open_dialog();
        QVERIFY(dialog);
        dialog->selectFile(output);
        static_cast<QDialog *>(dialog)->accept();
        // Ready: pause becomes available once the worker records.
        QPushButton *pause = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((pause = button_with_tooltip(toolbar, "暂停录制")) && pause->isEnabled(), 30000);
        QVERIFY(window->video_busy());
        QTest::qWait(400);
        pause->click();
        QTRY_VERIFY_WITH_TIMEOUT(button_with_tooltip(toolbar, "恢复录制") && button_with_tooltip(toolbar, "恢复录制")->isEnabled(), 5000);
        button_with_tooltip(toolbar, "恢复录制")->click();
        QTRY_VERIFY_WITH_TIMEOUT(button_with_tooltip(toolbar, "暂停录制") && button_with_tooltip(toolbar, "暂停录制")->isEnabled(), 5000);
        QTest::qWait(300);
        button_with_tooltip(toolbar, "停止录制")->click();
        QTRY_VERIFY_WITH_TIMEOUT(button_with_text(toolbar, "复制文件"), 30000);
        QVERIFY(QFileInfo(output).size() > 0);
        QVERIFY(!window->video_busy());

        // The result row: copy puts the file on the clipboard, open and folder go to the desktop.
        button_with_text(toolbar, "复制文件")->click();
        QVERIFY(button_with_text(toolbar, "已复制"));
        const auto urls = QApplication::clipboard()->mimeData()->urls();
        QCOMPARE(urls.size(), 1);
        QCOMPARE(QFileInfo(urls.first().toLocalFile()), QFileInfo(output));
        UrlCatcher catcher;
        QDesktopServices::setUrlHandler("file", &catcher, "open");
        button_with_text(toolbar, "播放视频")->click();
        button_with_text(toolbar, "打开文件夹")->click();
        QDesktopServices::unsetUrlHandler("file");
        QCOMPARE(catcher.urls.size(), 2);
        QCOMPARE(QFileInfo(catcher.urls.at(0).toLocalFile()), QFileInfo(output));
        QCOMPARE(QFileInfo(catcher.urls.at(1).toLocalFile()), QFileInfo(out.path()));
        // A file moved away since is reported.
        QVERIFY(QFile::rename(output, out.filePath("moved.mp4")));
        button_with_text(toolbar, "播放视频")->click();
        auto *missing = message_dialog(QString::fromUtf8("视频文件不存在"));
        QVERIFY(missing);
        missing->close();
        // Re-recording returns to the recording row.
        button_with_text(toolbar, "重新录制")->click();
        QVERIFY(!button_with_text(toolbar, "复制文件"));
        QVERIFY(button_with_tooltip(toolbar, "开始录制")->isEnabled());
        button_with_tooltip(toolbar, "关闭")->click();
        QCOMPARE(closed.size(), 1);
        QTRY_VERIFY(active_gif_window().isNull());
        config->set_app_setting("recording_format", "gif");
    }

    // Closing while an MP4 records saves the video first, then closes.
    void closingDuringMp4FinishesTheFile() {
        auto *config = get_tool_settings_manager();
        config->set_app_setting("recording_format", "mp4");
        config->set_app_setting("recording_delay", 0);
        silent_options().save_to_config(config);
        auto *window = new GifRecordWindow(centred_rect(QSize(320, 200)));
        active_gif_window() = window;
        QSignalSpy closed(window, &GifRecordWindow::closed);
        RecordToolbar *toolbar = nullptr;
        QTRY_VERIFY((toolbar = visible_top_level<RecordToolbar>()));
        QTemporaryDir out;
        const QString output = out.filePath("closing.mp4");
        button_with_tooltip(toolbar, "开始录制")->click();
        auto *dialog = toolbar->findChild<QFileDialog *>();
        QVERIFY(dialog);
        dialog->selectFile(output);
        static_cast<QDialog *>(dialog)->accept();
        QTRY_VERIFY_WITH_TIMEOUT(button_with_tooltip(toolbar, "暂停录制") && button_with_tooltip(toolbar, "暂停录制")->isEnabled(), 30000);
        QTest::qWait(400);
        button_with_tooltip(toolbar, "关闭")->click();
        // The window stays until the worker has written the file.
        QCOMPARE(closed.size(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(closed.size(), 1, 30000);
        QVERIFY(QFileInfo(output).size() > 0);
        QTRY_VERIFY(active_gif_window().isNull());
        config->set_app_setting("recording_format", "gif");
    }

    // Closing with the save dialog open drops it and closes at once.
    void closingWithTheSaveDialogOpenCloses() {
        get_tool_settings_manager()->set_app_setting("recording_format", "mp4");
        auto *window = new GifRecordWindow(centred_rect(QSize(240, 160)));
        active_gif_window() = window;
        QSignalSpy closed(window, &GifRecordWindow::closed);
        RecordToolbar *toolbar = nullptr;
        QTRY_VERIFY((toolbar = visible_top_level<RecordToolbar>()));
        button_with_tooltip(toolbar, "开始录制")->click();
        QPointer<QFileDialog> dialog = toolbar->findChild<QFileDialog *>();
        QVERIFY(dialog && dialog->isVisible());
        window->close_all();
        QCOMPARE(closed.size(), 1);
        QVERIFY(!dialog || !dialog->isVisible());
        QTRY_VERIFY(active_gif_window().isNull());
        get_tool_settings_manager()->set_app_setting("recording_format", "gif");
    }

    // The start delay counts down on the toolbar; stopping during it cancels, waiting it out
    // records. GIF pause and resume, then re-recording from playback.
    void delayedGifRecordingPausesAndRerecords() {
        auto *config = get_tool_settings_manager();
        config->set_app_setting("recording_format", "gif");
        config->set_app_setting("recording_delay", 0);
        const QRect rect = centred_rect(QSize(320, 200));
        AnimatedWindow page;
        page.setGeometry(rect);
        page.show();
        auto *window = new GifRecordWindow(rect);
        active_gif_window() = window;
        RecordToolbar *toolbar = nullptr;
        QTRY_VERIFY((toolbar = visible_top_level<RecordToolbar>()));
        QVERIFY(pick_from_menu(time_button(toolbar), QString::fromUtf8("2 秒")));
        QCOMPARE(config->get_app_setting("recording_delay").toInt(), 2);
        QCOMPARE(time_button(toolbar)->toolTip(), QString::fromUtf8("录制延迟: 2 秒"));
        button_with_tooltip(toolbar, "开始录制")->click();
        auto *countdown = button_with_text(toolbar, "倒计时 2");
        QVERIFY(countdown);
        QVERIFY(!countdown->isEnabled());
        // Stop is the only way out of the countdown.
        button_with_tooltip(toolbar, "停止录制")->click();
        QCOMPARE(time_button(toolbar)->text(), QString("00:00"));
        QVERIFY(button_with_tooltip(toolbar, "开始录制")->isEnabled());

        QVERIFY(pick_from_menu(time_button(toolbar), QString::fromUtf8("1 秒")));
        button_with_tooltip(toolbar, "开始录制")->click();
        QVERIFY(button_with_text(toolbar, "倒计时 1"));
        QPushButton *pause = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((pause = button_with_tooltip(toolbar, "暂停录制")) && pause->isEnabled(), 5000);
        QTest::qWait(300);
        pause->click();
        QVERIFY(button_with_tooltip(toolbar, "恢复录制"));
        QTest::qWait(200);
        button_with_tooltip(toolbar, "恢复录制")->click();
        QVERIFY(button_with_tooltip(toolbar, "暂停录制"));
        QTest::qWait(500);
        button_with_tooltip(toolbar, "停止录制")->click();
        PlaybackToolbar *playback = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((playback = visible_top_level<PlaybackToolbar>()), 10000);
        QVERIFY(!toolbar->isVisible());
        button_with_tooltip(playback, "重新录制")->click();
        QVERIFY(toolbar->isVisible());
        QVERIFY(!playback->isVisible());
        QVERIFY(button_with_tooltip(toolbar, "开始录制")->isEnabled());
        QVERIFY(pick_from_menu(time_button(toolbar), QString::fromUtf8("无延迟")));
        QCOMPARE(config->get_app_setting("recording_delay").toInt(), 0);
        window->close_all();
        QTRY_VERIFY(active_gif_window().isNull());
    }

    // Resizing the frame and dragging the handle move the area, the layers and the toolbar.
    void theAreaResizesAndMovesWithItsToolbar() {
        get_tool_settings_manager()->set_app_setting("recording_format", "gif");
        const QRect rect = centred_rect(QSize(300, 180));
        auto *window = new GifRecordWindow(rect);
        active_gif_window() = window;
        RecordToolbar *toolbar = nullptr;
        QTRY_VERIFY((toolbar = visible_top_level<RecordToolbar>()));
        CaptureOverlay *overlay = visible_top_level<CaptureOverlay>();
        QVERIFY(overlay);
        // The toolbar sits below the area first.
        QVERIFY(toolbar->geometry().top() > rect.bottom());
        // Dragging the right edge widens the area.
        const QPoint edge = overlay->mapToGlobal(QPoint(overlay->width() - 2, overlay->height() / 2));
        send_mouse(overlay, QEvent::MouseButtonPress, edge, Qt::LeftButton);
        send_mouse(overlay, QEvent::MouseMove, edge + QPoint(20, 0), Qt::LeftButton);
        send_mouse(overlay, QEvent::MouseMove, edge + QPoint(40, 0), Qt::LeftButton);
        send_mouse(overlay, QEvent::MouseButtonRelease, edge + QPoint(40, 0), Qt::NoButton);
        QCOMPARE(overlay->capture_rect(), rect.adjusted(0, 0, 40, 0));
        QCOMPARE(drawing_view()->geometry(), rect.adjusted(0, 0, 40, 0));
        // The handle moves the area and the toolbar together.
        QLabel *handle = nullptr;
        for (auto *label : toolbar->findChildren<QLabel *>())
            if (label->cursor().shape() == Qt::SizeAllCursor && label->isVisible()) handle = label;
        QVERIFY(handle);
        const QPoint toolbar_before = toolbar->pos();
        const QPoint grip = handle->mapToGlobal(handle->rect().center());
        send_mouse(handle, QEvent::MouseButtonPress, grip, Qt::LeftButton);
        send_mouse(handle, QEvent::MouseMove, grip + QPoint(15, -10), Qt::LeftButton);
        send_mouse(handle, QEvent::MouseMove, grip + QPoint(30, -20), Qt::LeftButton);
        QCOMPARE(overlay->capture_rect(), rect.adjusted(0, 0, 40, 0).translated(30, -20));
        QCOMPARE(toolbar->pos(), toolbar_before + QPoint(30, -20));
        send_mouse(handle, QEvent::MouseButtonRelease, grip + QPoint(30, -20), Qt::NoButton);
        QVERIFY(toolbar->geometry().top() > overlay->capture_rect().bottom());
        window->close_all();
        QTRY_VERIFY(active_gif_window().isNull());
    }

    // Without room below, the toolbar goes above; without room above either, beside the area.
    void theToolbarFindsRoomAroundTheArea() {
        get_tool_settings_manager()->set_app_setting("recording_format", "gif");
        const auto available = QGuiApplication::primaryScreen()->availableGeometry();
        const QRect low(available.center().x() - 150, available.bottom() - 200, 300, 180);
        auto *window = new GifRecordWindow(low);
        active_gif_window() = window;
        RecordToolbar *toolbar = nullptr;
        QTRY_VERIFY((toolbar = visible_top_level<RecordToolbar>()));
        QVERIFY2(toolbar->geometry().bottom() < low.top(), qPrintable(QString("%1 vs %2").arg(toolbar->geometry().bottom()).arg(low.top())));
        window->close_all();
        QTRY_VERIFY(active_gif_window().isNull());
        const QRect tall(available.center().x(), available.top(), 300, available.height());
        window = new GifRecordWindow(tall);
        active_gif_window() = window;
        QTRY_VERIFY((toolbar = visible_top_level<RecordToolbar>()));
        QVERIFY(toolbar->geometry().right() < tall.left() || toolbar->geometry().left() > tall.right());
        window->close_all();
        QTRY_VERIFY(active_gif_window().isNull());
    }

    // The toolbar's styles reach the drawing layer and the selected arrow.
    void drawingStylesReachTheLayer() {
        get_tool_settings_manager()->set_app_setting("recording_format", "gif");
        auto *window = new GifRecordWindow(centred_rect(QSize(320, 200)));
        active_gif_window() = window;
        RecordToolbar *toolbar = nullptr;
        QTRY_VERIFY((toolbar = visible_top_level<RecordToolbar>()));
        auto *view = drawing_view();
        QVERIFY(view);
        auto *arrow_button = button_with_tooltip(toolbar, "箭头");
        QVERIFY(arrow_button);
        arrow_button->click();
        QVERIFY(toolbar->arrow_panel->isVisible());
        auto &ctx = view->tool_controller()->ctx;
        emit toolbar->color_changed(QColor(10, 200, 30));
        QCOMPARE(ctx.color.rgb(), QColor(10, 200, 30).rgb());
        emit toolbar->width_changed(9);
        QCOMPARE(ctx.stroke_width, 9.0);
        emit toolbar->opacity_changed(128);
        QVERIFY(std::abs(ctx.opacity - 128 / 255.0) < 0.01);
        auto *viewport = view->viewport();
        const auto at = [&](const QPoint &pos) { return viewport->mapToGlobal(pos); };
        send_mouse(viewport, QEvent::MouseButtonPress, at(QPoint(40, 40)), Qt::LeftButton);
        send_mouse(viewport, QEvent::MouseMove, at(QPoint(100, 90)), Qt::LeftButton);
        send_mouse(viewport, QEvent::MouseMove, at(QPoint(160, 140)), Qt::LeftButton);
        send_mouse(viewport, QEvent::MouseButtonRelease, at(QPoint(160, 140)), Qt::NoButton);
        ArrowItem *arrow = nullptr;
        for (auto *item : view->canvas_scene->items())
            if (auto *found = dynamic_cast<ArrowItem *>(item)) arrow = found;
        QVERIFY(arrow);
        view->smart_edit_controller->select_item(arrow);
        emit toolbar->arrow_panel->arrow_style_changed("double");
        QCOMPARE(arrow->arrow_style(), QString("double"));
        QCOMPARE(get_tool_settings_manager()->get_tool_settings("arrow")->get("arrow_style").toString(), QString("double"));
        // The rectangle's line style is saved for the rectangle.
        button_with_tooltip(toolbar, "矩形")->click();
        QVERIFY(toolbar->shape_panel->isVisible());
        emit toolbar->shape_panel->line_style_changed("dashed");
        QCOMPARE(get_tool_settings_manager()->get_tool_settings("rect")->get("line_style").toString(), QString("dashed"));
        emit toolbar->shape_panel->line_style_changed("solid");
        // Text styles are saved for the next text.
        QSignalSpy shadow(toolbar, &RecordToolbar::shadow_changed);
        emit toolbar->text_panel->background_changed(true, QColor(255, 255, 0), 120);
        emit toolbar->text_panel->outline_changed(true, QColor(0, 0, 255), 2.5);
        emit toolbar->text_panel->shadow_changed(true, QColor(0, 0, 0, 90));
        QCOMPARE(shadow.size(), 1);
        emit toolbar->text_panel->background_changed(false, QColor(255, 255, 0), 120);
        emit toolbar->text_panel->outline_changed(false, QColor(0, 0, 255), 2.5);
        emit toolbar->text_panel->shadow_changed(false, QColor(0, 0, 0, 90));
        toolbar->set_stroke_width(6);
        toolbar->set_opacity(200);
        toolbar->apply_scale();
        QVERIFY(toolbar->isVisible());
        window->close_all();
        QTRY_VERIFY(active_gif_window().isNull());
    }

    // The MP4 options: the settings dialog, the audio and cursor switches and a custom frame rate.
    void videoOptionsAreSavedFromTheToolbar() {
        auto *config = get_tool_settings_manager();
        config->set_app_setting("recording_format", "mp4");
        silent_options().save_to_config(config);
        RecordToolbar toolbar;
        toolbar.show();
        QSignalSpy fps_changed(&toolbar, &RecordToolbar::fps_changed);
        button_with_tooltip(&toolbar, "视频设置")->click();
        auto *dialog = visible_child<QDialog>(&toolbar);
        QVERIFY(dialog);
        // A second click raises the same dialog.
        button_with_tooltip(&toolbar, "视频设置")->click();
        QCOMPARE(toolbar.findChildren<QDialog *>().size(), 1);
        dialog->findChild<QSpinBox *>()->setValue(25);
        auto *presets = dialog->findChild<QComboBox *>();
        presets->setCurrentIndex(presets->findData(8.0));
        QCOMPARE(dialog->findChild<QDoubleSpinBox *>()->value(), 8.0);
        // A typed bitrate selects the custom entry.
        dialog->findChild<QDoubleSpinBox *>()->setValue(6.5);
        QVERIFY(!presets->currentData().isValid());
        for (auto *box : dialog->findChildren<QCheckBox *>()) box->setChecked(box->text() == QString::fromUtf8("录制系统声音"));
        dialog->accept();
        QCOMPARE(config->get_app_setting("video_record_fps").toInt(), 25);
        QCOMPARE(config->get_app_setting("video_record_bitrate").toInt(), 6500000);
        QVERIFY(config->get_app_setting("video_record_system_audio").toBool());
        QVERIFY(!config->get_app_setting("video_record_hardware").toBool());
        QVERIFY(!config->get_app_setting("video_record_cursor").toBool());
        QCOMPARE(fps_changed.last().at(0).toInt(), 25);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(toolbar.findChildren<QDialog *>().size(), 0);
        // The quick switches.
        QVERIFY(pick_from_menu(button_with_tooltip(&toolbar, "录制系统声音"), QString::fromUtf8("录制系统声音")));
        QVERIFY(!config->get_app_setting("video_record_system_audio").toBool());
        button_with_tooltip(&toolbar, "录制鼠标指针")->click();
        QVERIFY(config->get_app_setting("video_record_cursor").toBool());
        // Frame rate from the menu, then a custom one.
        ClickMenuButton *fps = nullptr;
        for (auto *button : toolbar.findChildren<ClickMenuButton *>())
            if (button->text().endsWith("fps")) fps = button;
        QVERIFY(fps);
        QCOMPARE(fps->text(), QString("25 fps"));
        QVERIFY(pick_from_menu(fps, "60 fps"));
        QCOMPARE(config->get_app_setting("video_record_fps").toInt(), 60);
        QVERIFY(pick_from_menu(fps, QString::fromUtf8("自定义帧率…")));
        auto *input = visible_child<QInputDialog>(&toolbar);
        QVERIFY(input);
        QCOMPARE(input->intValue(), 60);
        input->setIntValue(37);
        input->accept();
        QCOMPARE(config->get_app_setting("video_record_fps").toInt(), 37);
        QCOMPARE(fps->text(), QString("37 fps"));
        QCOMPARE(toolbar.get_current_fps(), 37);
        // A cancelled custom rate keeps the current one.
        QVERIFY(pick_from_menu(fps, QString::fromUtf8("自定义帧率…")));
        input = visible_child<QInputDialog>(&toolbar);
        QVERIFY(input);
        input->reject();
        QCOMPARE(fps->text(), QString("37 fps"));
        // GIF has its own frame rates.
        QVERIFY(pick_from_menu(button_with_tooltip(&toolbar, "录制格式"), "GIF"));
        QVERIFY(!button_with_tooltip(&toolbar, "视频设置"));
        QVERIFY(pick_from_menu(fps, "10 fps"));
        QCOMPARE(config->get_gif_fps(), 10);
        QCOMPARE(toolbar.get_current_fps(), 10);
        silent_options().save_to_config(config);
        config->set_app_setting("recording_format", "gif");
    }

    // Playback through its toolbar: seeking, trimming, speed, play to the end, pause, the
    // cursor switch, and saving the trimmed range.
    void playbackSeeksTrimsPlaysAndSaves() {
        auto *config = get_tool_settings_manager();
        config->set_app_setting("recording_format", "gif");
        config->set_app_setting("recording_delay", 0);
        const QRect rect = centred_rect(QSize(320, 200));
        AnimatedWindow page;
        page.setGeometry(rect);
        page.show();
        auto *window = new GifRecordWindow(rect);
        active_gif_window() = window;
        QSignalSpy closed(window, &GifRecordWindow::closed);
        RecordToolbar *toolbar = nullptr;
        QTRY_VERIFY((toolbar = visible_top_level<RecordToolbar>()));
        button_with_tooltip(toolbar, "开始录制")->click();
        QTRY_VERIFY_WITH_TIMEOUT(button_with_tooltip(toolbar, "停止录制") && button_with_tooltip(toolbar, "暂停录制")->isEnabled(), 3000);
        QTest::qWait(2300);
        button_with_tooltip(toolbar, "停止录制")->click();
        PlaybackToolbar *playback = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((playback = visible_top_level<PlaybackToolbar>()), 10000);
        auto *slider = playback->slider();
        const int last = slider->trim_end();
        QVERIFY2(last >= 10, qPrintable(QString::number(last)));
        QLabel *time = nullptr;
        for (auto *label : playback->findChildren<QLabel *>())
            if (label->text().contains(" / ")) time = label;
        QVERIFY(time);
        const QString at_start = time->text();

        // A click on the empty track seeks there; the time shows whole seconds.
        QTest::mouseClick(slider, Qt::LeftButton, Qt::NoModifier, QPoint(slider->width() * 9 / 10, slider->height() / 2));
        QTRY_VERIFY(time->text() != at_start);
        // Dragging the start handle trims the beginning.
        const int margin = slider->height() > 0 ? scaled(RangeSlider::BASE_HANDLE_W) : 0;
        QTest::mousePress(slider, Qt::LeftButton, Qt::NoModifier, QPoint(margin, slider->height() / 2));
        QTest::mouseMove(slider, QPoint(slider->width() / 5, slider->height() / 2));
        QTest::mouseMove(slider, QPoint(slider->width() / 4, slider->height() / 2));
        QTest::mouseRelease(slider, Qt::LeftButton, Qt::NoModifier, QPoint(slider->width() / 4, slider->height() / 2));
        QVERIFY2(slider->trim_start() > 0, qPrintable(QString::number(slider->trim_start())));
        const auto [trim_start, trim_end] = playback->get_trim_range();

        // Faster playback runs to the end of the range and stops there.
        ClickMenuButton *speed = nullptr;
        for (auto *button : playback->findChildren<ClickMenuButton *>())
            if (button->text().endsWith('x')) speed = button;
        QVERIFY(speed);
        QVERIFY(pick_from_menu(speed, "2.0x"));
        button_with_tooltip(playback, "播放")->click();
        QVERIFY(button_with_tooltip(playback, "暂停"));
        QTRY_VERIFY_WITH_TIMEOUT(button_with_tooltip(playback, "播放"), 10000);
        // Pausing part-way keeps the position.
        button_with_tooltip(playback, "播放")->click();
        QTest::qWait(150);
        button_with_tooltip(playback, "暂停")->click();
        QVERIFY(button_with_tooltip(playback, "播放"));
        const QString paused = time->text();
        QTest::qWait(300);
        QCOMPARE(time->text(), paused);
        // The cursor overlay switches off and on.
        auto *cursor = button_with_tooltip(playback, "显示鼠标光标");
        cursor->click();
        QVERIFY(!cursor->isChecked());
        cursor->click();
        QVERIFY(cursor->isChecked());

        // Saving asks for a file, writes the trimmed frames and closes.
        QTemporaryDir out;
        const QString target = out.filePath("saved.gif");
        QTimer answer;
        connect(&answer, &QTimer::timeout, this, [&] {
            auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            auto *name = dialog ? dialog->findChild<QLineEdit *>("fileNameEdit") : nullptr;
            if (!name || !dialog->isVisible()) return;
            answer.stop();
            // Typed like a person would: selectFile is reset when the dialog finishes showing.
            dialog->setDirectory(out.path());
            name->setText("saved.gif");
            static_cast<QDialog *>(dialog)->accept();
        });
        answer.start(50);
        // Never left waiting for a person: an unanswered dialog is cancelled.
        QTimer guard;
        guard.setSingleShot(true);
        connect(&guard, &QTimer::timeout, this, [] {
            if (auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) dialog->reject();
        });
        guard.start(10000);
        button_with_tooltip(playback, "另存为")->click();
        QTRY_COMPARE_WITH_TIMEOUT(closed.size(), 1, 30000);
        QImageReader reader(target);
        QVERIFY2(reader.canRead(), qPrintable(reader.errorString()));
        QVERIFY2(std::abs(reader.imageCount() - (trim_end - trim_start + 1)) <= 1, qPrintable(QString("%1 vs %2").arg(reader.imageCount()).arg(trim_end - trim_start + 1)));
        QTRY_VERIFY(active_gif_window().isNull());
    }

    // The playback cursor: a click bursts for three frames (held while pressed), scrolling shows arrows.
    void theCursorOverlayDrawsClicksAndScrolling() {
        CursorOverlay overlay;
        overlay.update_rect(QRect(centred_rect(QSize(200, 150)).topLeft(), QSize(200, 150)));
        overlay.show();
        const auto snapshot = [](int press, int scroll) {
            CursorSnapshot cursor;
            cursor.x = 60;
            cursor.y = 50;
            cursor.press = press;
            cursor.scroll = scroll;
            return std::optional<CursorSnapshot>(cursor);
        };
        overlay.set_frame_cursor(snapshot(0, 0));
        const QImage idle = overlay.grab().toImage();
        overlay.set_frame_cursor(snapshot(1, 0));
        const QImage pressed = overlay.grab().toImage();
        QVERIFY(pressed != idle);
        // Released, the burst runs out on its own.
        overlay.set_frame_cursor(snapshot(0, 0));
        QTest::qWait(250);
        QCOMPARE(overlay.grab().toImage(), idle);
        overlay.set_frame_cursor(snapshot(2, 0));
        overlay.set_frame_cursor(snapshot(0, 1));
        QVERIFY(overlay.grab().toImage() != idle);
        overlay.set_frame_cursor(snapshot(0, -1));
        QVERIFY(!overlay.grab().isNull());
        // Hidden by the switch or outside the region, nothing is drawn.
        overlay.set_cursor_visible(false);
        QVERIFY(!overlay.is_cursor_visible());
        overlay.set_cursor_visible(true);
        overlay.set_frame_cursor(std::nullopt);
        QTest::qWait(250);
        QVERIFY(overlay.grab().toImage() != idle);
    }

    // Export walks the same burst rules over the recorded frames.
    void exportedBurstsFollowTheFrames() {
        const auto at = [](int press) {
            CursorSnapshot cursor;
            cursor.press = press;
            return std::optional<CursorSnapshot>(cursor);
        };
        const std::vector<std::optional<CursorSnapshot>> frames = {at(0), at(1), at(1), at(0), at(0), at(0), at(0), at(2), std::nullopt, at(0)};
        const auto states = compute_burst_states(frames);
        QCOMPARE(states.size(), frames.size());
        QCOMPARE(states[0], std::make_pair(0, 0));
        QCOMPARE(states[1], std::make_pair(1, 1));
        // Held: stays on the first frame.
        QCOMPARE(states[2], std::make_pair(1, 1));
        QCOMPARE(states[3], std::make_pair(2, 1));
        QCOMPARE(states[4], std::make_pair(3, 1));
        QCOMPARE(states[5].first, 0);
        QCOMPARE(states[7], std::make_pair(1, 2));
        const auto sprites = rasterize_cursor_sprites(0.5);
        QVERIFY(sprites.cursor.w > 0);
        QVERIFY(sprites.burst_left[2].w >= sprites.burst_left[0].w);
        QVERIFY(sprites.scroll_up.w > 0 && sprites.scroll_down.w > 0);
    }
};

int main(int argc, char **argv) {
    // The MP4 worker the recording window starts is this executable, as it is the application.
    if (argc > 1 && std::string(argv[1]) == "--video-recorder-worker") {
        std::vector<std::string> args;
        int count = 0;
        if (LPWSTR *wide = CommandLineToArgvW(GetCommandLineW(), &count)) {
            for (int i = 2; i < count; ++i) args.push_back(QString::fromWCharArray(wide[i]).toStdString());
            LocalFree(wide);
        }
        return video::worker_main(args);
    }
    // Qt's own file dialog can be driven from the test; the native one runs its own loop.
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    GifTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "test_gif.moc"
