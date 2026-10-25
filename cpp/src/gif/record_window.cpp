#include "gif/record_window.h"
#include "canvas/items/arrow_item.h"
#include "canvas/smart_edit_controller.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "core/platform_utils.h"
#include "core/ui_scale.h"
#include "gif/_widgets.h"
#include "gif/drawing_view.h"
#include "gif/frame_recorder.h"
#include "gif/gifrecorder.h"
#include "gif/overlay.h"
#include "gif/playback_controller.h"
#include "gif/playback_engine.h"
#include "gif/playback_toolbar.h"
#include "gif/record_toolbar.h"
#include "gif/video_recorder.h"
#include "settings/tool_settings.h"
#include "tools/controller.h"
#include "tools/cursor_manager.h"
#include "ui/dialogs.h"
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QMimeData>
#include <QProgressBar>
#include <QScreen>
#include <QUrl>
#include <QVBoxLayout>

namespace jietuba {
namespace {
QString gif_tr(const char *text) { return i18n_tr(text, "GifRecorder"); }
QString tr_text(const char *text) { return i18n_tr(text, "GifRecordWindow"); }

QString countdown_text(int seconds) { return tr_text("倒计时 {seconds}").replace("{seconds}", QString::number(seconds)); }

QString rect_text(const QRect &rect) { return QString("QRect(%1, %2, %3, %4)").arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height()); }

bool future_done(const std::shared_future<void> &future) { return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }
}

QPointer<GifRecordWindow> &active_gif_window() {
    static QPointer<GifRecordWindow> window;
    return window;
}

GifRecordWindow::GifRecordWindow(const QRect &capture_rect, QObject *parent) : QObject(parent), rect_(capture_rect), countdown_timer_(this), release_timer_(this) {
    log_info(T("GifRecordWindow 初始化, 区域={capture_rect}", {{"capture_rect", rect_text(capture_rect)}}), "GIF");

    // Recorders.
    recorder_ = new FrameRecorder(this);
    video_ = new VideoRecorder(this);
    connect(video_, &VideoRecorder::ready, this, &GifRecordWindow::on_video_ready);
    connect(video_, &VideoRecorder::progress, this, &GifRecordWindow::on_video_progress);
    connect(video_, &VideoRecorder::paused_changed, this, &GifRecordWindow::on_video_paused);
    connect(video_, &VideoRecorder::completed, this, &GifRecordWindow::on_video_completed);
    connect(video_, &VideoRecorder::failed, this, &GifRecordWindow::on_video_failed);
    connect(video_, &VideoRecorder::finished, this, &GifRecordWindow::on_video_finished);
    countdown_timer_.setInterval(1000);
    connect(&countdown_timer_, &QTimer::timeout, this, &GifRecordWindow::advance_countdown);
    release_timer_.setInterval(25);
    connect(&release_timer_, &QTimer::timeout, this, &GifRecordWindow::start_pending_video);
    connect(qApp, &QCoreApplication::aboutToQuit, video_, &VideoRecorder::shutdown);

    // Three window layers.
    overlay_ = new CaptureOverlay(rect_);
    connect(overlay_, &CaptureOverlay::rect_changed, this, &GifRecordWindow::on_rect_changed);

    // The drawing layer reuses the screenshot canvas and tools.
    drawing_view_ = new GifDrawingView(rect_);

    record_toolbar_ = new RecordToolbar;
    connect_record_toolbar();

    playback_ = new PlaybackController(this);
    connect(playback_, &PlaybackController::close_requested, this, &GifRecordWindow::close_all);
    connect(playback_, &PlaybackController::rerecord_requested, this, &GifRecordWindow::on_rerecord);

    overlay_->show();
    drawing_view_->show();
    record_toolbar_->show();
    record_toolbar_->set_record_rect(rect_);
    reposition_toolbar(record_toolbar_);

    // Toolbars recompute their own sizes on scale changes, but the side they stick to is decided here.
    connect(&get_ui_scale(), &UIScaleManager::scale_changed, this, &GifRecordWindow::reposition_current_toolbar);

    enter_state(AppState::IDLE);
    // Builds the capture thread while the selection is adjusted, so recording starts at once.
    if (record_toolbar_->get_recording_mode() == "gif") recorder_->prepare();
    log_debug(T("GifRecordWindow 初始化完成"), "GIF");
}

bool GifRecordWindow::video_busy() const {
    return save_dialog_ || pending_video_ || video_->active() || (countdown_action_ && record_toolbar_ && record_toolbar_->get_recording_mode() == "mp4");
}

bool GifRecordWindow::overlay_visible() const { return overlay_ && overlay_->isVisible(); }

void GifRecordWindow::reposition_current_toolbar() {
    auto *pb_tb = playback_->playback_toolbar();
    QWidget *toolbar = pb_tb && pb_tb->isVisible() ? static_cast<QWidget *>(pb_tb) : record_toolbar_;
    reposition_toolbar(toolbar);
}

// ── Signal wiring ──

void GifRecordWindow::connect_record_toolbar() {
    auto *tb = record_toolbar_;
    connect(tb, &RecordToolbar::fps_changed, this, &GifRecordWindow::on_fps_changed);
    connect(tb, &RecordToolbar::mode_changed, this, &GifRecordWindow::on_mode_changed);
    connect(tb, &RecordToolbar::record_start, this, &GifRecordWindow::on_record_start);
    connect(tb, &RecordToolbar::record_stop, this, &GifRecordWindow::on_record_stop);
    connect(tb, &RecordToolbar::pause_toggled, this, &GifRecordWindow::on_pause_toggled);
    connect(tb, &RecordToolbar::move_requested, this, &GifRecordWindow::on_move_requested);
    connect(tb, &RecordToolbar::drag_ended, this, &GifRecordWindow::on_drag_ended);
    connect(tb, &RecordToolbar::close_requested, this, &GifRecordWindow::close_all);
    connect(tb, &RecordToolbar::video_open_requested, this, [this] { use_video_result("open"); });
    connect(tb, &RecordToolbar::video_folder_requested, this, [this] { use_video_result("folder"); });
    connect(tb, &RecordToolbar::video_copy_requested, this, [this] { use_video_result("copy"); });
    connect(tb, &RecordToolbar::video_rerecord_requested, this, &GifRecordWindow::rerecord_video);
    // Drawing tools.
    connect(tb, &RecordToolbar::tool_selected, this, &GifRecordWindow::on_drawing_tool_selected);
    connect(tb, &RecordToolbar::deactivate_requested, this, &GifRecordWindow::on_drawing_deactivate);
    connect(tb, &RecordToolbar::undo_requested, this, [this] { drawing_view_->undo_stack()->undo(); });
    connect(tb, &RecordToolbar::redo_requested, this, [this] { drawing_view_->undo_stack()->redo(); });
    connect(tb, &RecordToolbar::color_changed, this, &GifRecordWindow::on_drawing_color_changed);
    connect(tb, &RecordToolbar::width_changed, this, &GifRecordWindow::on_drawing_width_changed);
    connect(tb, &RecordToolbar::opacity_changed, this, &GifRecordWindow::on_drawing_opacity_changed);
    // Text tool signals go to SmartEditController.
    connect(tb, &RecordToolbar::font_changed, this, [this](const QFont &font) {
        if (drawing_view_->smart_edit_controller) drawing_view_->smart_edit_controller->on_text_font_changed(font);
    });
    connect(tb, &RecordToolbar::background_changed, this, [this](bool enabled, const QColor &color, int opacity) {
        if (drawing_view_->smart_edit_controller) drawing_view_->smart_edit_controller->on_text_background_changed(enabled, color, opacity);
    });
    connect(tb, &RecordToolbar::outline_changed, this, [this](bool enabled, const QColor &color, double width) {
        if (drawing_view_->smart_edit_controller) drawing_view_->smart_edit_controller->on_text_outline_changed(enabled, color, width);
    });
    connect(tb, &RecordToolbar::shadow_changed, this, [this](bool enabled, const QColor &color) {
        if (drawing_view_->smart_edit_controller) drawing_view_->smart_edit_controller->on_text_shadow_changed(enabled, color);
    });
    // Styles.
    connect(tb, &RecordToolbar::arrow_style_changed, this, &GifRecordWindow::on_drawing_arrow_style_changed);
    connect(tb, &RecordToolbar::line_style_changed, this, [this](const QString &style) { drawing_view_->apply_line_style_change_to_selection(style); });
    // Captured frames update the elapsed time.
    connect(recorder_, &FrameRecorder::frame_captured, this, &GifRecordWindow::on_record_frame_count);
    // The time limit runs the same stop as the stop button.
    connect(recorder_, &FrameRecorder::limit_reached, this, &GifRecordWindow::on_record_stop);
}

// ── State machine ──

void GifRecordWindow::enter_state(AppState state) {
    state_ = state;
    if (state == AppState::IDLE) {
        // Leaving annotation changes only mouse interaction, never the real recording / pause state.
        const auto video_state = video_->state();
        const bool gif_active = recorder_->state() == RecordState::RECORDING || recorder_->state() == RecordState::PAUSED;
        const bool video_recording = video_state == "recording" || video_state == "pausing" || video_state == "paused" || video_state == "resuming" || video_state == "stopping";
        const bool recording = video_recording || gif_active;
        const bool paused = video_state == "paused" || video_state == "resuming" || recorder_->state() == RecordState::PAUSED;
        overlay_->set_recording(recording, paused);
        const bool locked = video_->active() || gif_active || countdown_action_;
        overlay_->set_mode(locked ? OverlayMode::PASSTHROUGH : OverlayMode::RESIZE);
        drawing_view_->activate_tool("cursor");
        record_toolbar_->highlight_tool(std::nullopt);
    } else if (state == AppState::RESIZING) {
        overlay_->set_mode(OverlayMode::RESIZE);
        drawing_view_->activate_tool("cursor");
        record_toolbar_->highlight_tool(std::nullopt);
    } else if (state == AppState::DRAWING) {
        // The record toolbar decides the drawing layer's tool.
        overlay_->set_mode(OverlayMode::PASSTHROUGH);
    } else if (state == AppState::PLAYBACK) {
        overlay_->set_recording(false);
        overlay_->set_mode(OverlayMode::PASSTHROUGH);
        drawing_view_->activate_tool("cursor");
        drawing_view_->hide();
        record_toolbar_->highlight_tool(std::nullopt);
    }
}

// ── Geometry ──

void GifRecordWindow::sync_geometry(const QRect &rect, bool reposition) {
    rect_ = rect;
    overlay_->update_rect(rect);
    if (drawing_view_) drawing_view_->update_rect(rect);
    // The recording area decides which side the panels open on.
    record_toolbar_->set_record_rect(rect_);
    playback_->update_rect(rect_);
    if (reposition) reposition_current_toolbar();
    record_toolbar_->reposition_panels();
}

void GifRecordWindow::reposition_toolbar(QWidget *toolbar) {
    if (!toolbar) return;
    const QRect r = rect_;
    const int tw = toolbar->width();
    const int th = toolbar->height();
    const int gap = scaled(4);

    // The screen under the recording area's centre, not always the primary one.
    QScreen *screen = QApplication::screenAt(r.center());
    if (!screen) screen = QApplication::primaryScreen();
    const QRect screen_rect = screen->geometry();

    // The tallest settings panel is placed together with the toolbar.
    auto *record_toolbar = qobject_cast<RecordToolbar *>(toolbar);
    const int panel_extra = record_toolbar ? record_toolbar->get_max_panel_height() : 0;

    // Horizontally centred x for the above / below placements.
    int x_center = r.left() + python_floordiv(r.width() - tw, 2);
    x_center = std::max(screen_rect.left(), std::min(x_center, screen_rect.right() - tw));

    int x, y;
    bool toolbar_below;
    const int y_below = r.bottom() + gap;
    if (y_below + th + panel_extra <= screen_rect.bottom()) {
        // Below the recording area.
        x = x_center, y = y_below, toolbar_below = true;
    } else if (r.top() - th - gap >= screen_rect.top()) {
        // Above it.
        x = x_center, y = r.top() - th - gap, toolbar_below = false;
    } else {
        // Left or right when the area fills the screen height, vertically centred on it.
        int y_mid = r.top() + python_floordiv(r.height() - th, 2);
        y_mid = std::max(screen_rect.top(), std::min(y_mid, screen_rect.bottom() - th));
        if (r.left() - tw - gap >= screen_rect.left()) {
            x = r.left() - tw - gap, y = y_mid, toolbar_below = false;
        } else if (r.right() + gap + tw <= screen_rect.right()) {
            x = r.right() + gap, y = y_mid, toolbar_below = true;
        } else {
            // Nothing fits outside (a full-screen area): inside along the bottom edge, above the taskbar.
            const int bottom = std::min(r.bottom(), screen->availableGeometry().bottom());
            x = x_center;
            y = std::max(screen_rect.top(), bottom - th - scaled(10));
            toolbar_below = false;
        }
    }
    if (record_toolbar) record_toolbar->set_toolbar_side(toolbar_below);
    else if (auto *playback_toolbar = qobject_cast<PlaybackToolbar *>(toolbar)) playback_toolbar->set_toolbar_side(toolbar_below);
    toolbar->move(x, y);
}

void GifRecordWindow::on_rect_changed(const QRect &rect) {
    if (video_->active() || pending_video_ || countdown_action_ || recorder_->state() == RecordState::RECORDING || recorder_->state() == RecordState::PAUSED) return;
    sync_geometry(rect);
    // Back to IDLE once resizing is done.
    enter_state(AppState::IDLE);
}

void GifRecordWindow::on_move_requested(const QPoint &delta) {
    if (video_->active() || pending_video_ || countdown_action_ || recorder_->state() == RecordState::RECORDING || recorder_->state() == RecordState::PAUSED) return;
    // While dragging the toolbar follows without smart placement; on_drag_ended aligns it.
    sync_geometry(rect_.translated(delta), false);
    auto *pb_tb = playback_->playback_toolbar();
    QWidget *tb = pb_tb && pb_tb->isVisible() ? static_cast<QWidget *>(pb_tb) : record_toolbar_;
    tb->move(tb->pos() + delta);
}

void GifRecordWindow::on_drag_ended() { reposition_current_toolbar(); }

// ── Recording ──

void GifRecordWindow::on_fps_changed(int fps) {
    log_debug(T("FPS 切换: {fps}", {{"fps", fps}}), "GIF");
    if (record_toolbar_->get_recording_mode() == "gif") recorder_->set_fps(fps);
}

void GifRecordWindow::on_mode_changed(const QString &mode) {
    if (mode == "mp4") {
        // Switching the format alone releases the GIF capture thread; MP4 never holds a FrameStore.
        gif_release_ = recorder_->release();
        recorder_->reset_data();
    } else if (gif_release_) {
        // The capture session can be lent again only after it came back, so an old task cannot clear a new loan.
        record_toolbar_->set_busy(tr_text("正在准备…"));
        start_pending_video();
    } else {
        recorder_->prepare();
    }
    reposition_current_toolbar();
}

void GifRecordWindow::on_record_frame_count(int count) {
    const int fps = record_toolbar_->get_current_fps();
    const double elapsed_s = fps > 0 ? static_cast<double>(count) / fps : 0;
    record_toolbar_->set_elapsed(elapsed_s);
}

void GifRecordWindow::on_record_start() {
    completed_video_path_.reset();
    if (record_toolbar_->get_recording_mode() == "mp4") {
        start_video();
        return;
    }
    if (gif_release_) {
        record_toolbar_->set_busy(tr_text("正在准备…"));
        start_pending_video();
        return;
    }
    begin_countdown([this] { start_gif_recording(); });
}

void GifRecordWindow::start_gif_recording() {
    const int fps = record_toolbar_->get_current_fps();
    log_info(T("开始录制, fps={fps}, 区域={rect}", {{"fps", fps}, {"rect", rect_text(rect_)}}), "GIF");
    recorder_->set_fps(fps);
    recorder_->set_rect(rect_);
    recorder_->start();
    // Red border, no dragging.
    overlay_->set_recording(true);
    overlay_->set_mode(OverlayMode::PASSTHROUGH);
}

void GifRecordWindow::on_record_stop() {
    if (countdown_action_) {
        cancel_countdown();
        reset_video_controls();
        return;
    }
    if (video_->active()) {
        record_toolbar_->set_busy(tr_text("正在保存…"));
        video_->stop();
        return;
    }
    if (recorder_->state() != RecordState::RECORDING && recorder_->state() != RecordState::PAUSED) return;

    show_stop_progress();

    auto &frames = recorder_->frames();
    const auto store = recorder_->store();
    if (store) log_info(T("停止录制, 共 {frame_count} 帧, store=有", {{"frame_count", static_cast<qlonglong>(frames.size())}}), "GIF");
    else log_info(T("停止录制, 共 {frame_count} 帧, store=无", {{"frame_count", static_cast<qlonglong>(frames.size())}}), "GIF");

    if (store && store->frame_count() == 0) {
        log_warning(T("FrameStore 为空"), "GIF");
        show_warning_dialog(nullptr, gif_tr("Recording error"), gif_tr("The recording is empty. Please record again."));
        // Back to an operable overlay and toolbar instead of the red pass-through state.
        overlay_->set_recording(false);
        overlay_->set_mode(OverlayMode::RESIZE);
        record_toolbar_->reset_state();
        return;
    }
    if (frames.empty()) {
        log_warning(T("录制帧为空，不进入回放"), "GIF");
        overlay_->set_recording(false);
        overlay_->set_mode(OverlayMode::RESIZE);
        record_toolbar_->reset_state();
        return;
    }
    switch_to_playback(frames);
}

void GifRecordWindow::show_stop_progress() {
    auto *dlg = new QFrame(nullptr);
    dlg->setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    dlg->setAttribute(Qt::WA_TranslucentBackground);
    dlg->setFixedSize(260, 14);

    auto *layout = new QVBoxLayout(dlg);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *bar = new QProgressBar(dlg);
    bar->setRange(0, 0);
    bar->setTextVisible(false);
    bar->setFixedHeight(14);
    bar->setStyleSheet(PROGRESS_BAR_STYLE);
    layout->addWidget(bar);

    dlg->move(rect_.center().x() - 130, rect_.center().y() - 7);

    // stop_async + stop_finished wait for the capture thread to exit.
    QEventLoop loop;
    const auto connection = connect(recorder_, &FrameRecorder::stop_finished, &loop, &QEventLoop::quit);
    recorder_->stop_async();

    // The bar shows after about 400 ms so capture has stopped and it is not recorded.
    QTimer show_timer;
    show_timer.setSingleShot(true);
    connect(&show_timer, &QTimer::timeout, dlg, &QWidget::show);
    show_timer.start(400);

    loop.exec();

    show_timer.stop();
    disconnect(connection);
    bar->setRange(0, 1);
    bar->setValue(1);
    dlg->hide();
    QApplication::processEvents();
    dlg->deleteLater();
}

void GifRecordWindow::on_pause_toggled(bool paused) {
    if (video_->active()) {
        if (video_->pause(paused)) record_toolbar_->set_busy(paused ? tr_text("正在暂停…") : tr_text("正在恢复…"), true);
        return;
    }
    if (paused) {
        log_debug(T("录制暂停"), "GIF");
        recorder_->pause();
    } else {
        log_debug(T("录制恢复"), "GIF");
        recorder_->resume();
    }
    overlay_->set_recording(true, recorder_->state() == RecordState::PAUSED);
}

void GifRecordWindow::begin_countdown(std::function<void()> action) {
    const int seconds = record_toolbar_->get_start_delay();
    if (!seconds) {
        action();
        return;
    }
    countdown_remaining_ = seconds;
    countdown_action_ = std::move(action);
    overlay_->set_mode(OverlayMode::PASSTHROUGH);
    record_toolbar_->set_busy(countdown_text(seconds), true);
    countdown_timer_.start();
}

void GifRecordWindow::advance_countdown() {
    if (!countdown_action_) return;
    --countdown_remaining_;
    if (countdown_remaining_) {
        record_toolbar_->set_busy(countdown_text(countdown_remaining_), true);
        return;
    }
    auto action = countdown_action_;
    cancel_countdown();
    if (record_toolbar_->get_recording_mode() == "mp4") record_toolbar_->set_busy(tr_text("正在准备…"));
    else record_toolbar_->set_video_ready();
    action();
}

void GifRecordWindow::cancel_countdown() {
    countdown_timer_.stop();
    countdown_action_ = nullptr;
}

void GifRecordWindow::start_video() {
    if (video_busy()) return;
    const QString folder = get_tool_settings_manager()->get_screenshot_save_path();
    const auto now = std::chrono::system_clock::now();
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    const QDateTime local = QDateTime::fromMSecsSinceEpoch(micros / 1000);
    const QString filename = "recording_" + local.toString("yyyyMMdd_HHmmss") + QString("_%1").arg(micros % 1000000, 6, 10, QChar('0')) + ".mp4";
    // The asynchronous file dialog enters no nested event loop; this stage belongs to the recording session too.
    auto *dialog = new QFileDialog(record_toolbar_, tr_text("保存 MP4 视频"));
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->setFileMode(QFileDialog::AnyFile);
    dialog->setNameFilter(tr_text("MP4 视频 (*.mp4)"));
    dialog->setOption(QFileDialog::DontConfirmOverwrite, true);
    dialog->setDirectory(QDir::toNativeSeparators(folder.isEmpty() ? QDir::homePath() : folder));
    dialog->selectFile(filename);
    dialog->setDefaultSuffix("mp4");
    connect(dialog, &QDialog::accepted, this, &GifRecordWindow::on_video_path_selected);
    connect(dialog, &QDialog::rejected, this, &GifRecordWindow::on_video_path_cancelled);
    save_dialog_ = dialog;
    record_toolbar_->set_busy(tr_text("正在准备…"));
    dialog->open();
}

QFileDialog *GifRecordWindow::take_save_dialog() {
    QFileDialog *dialog = save_dialog_;
    save_dialog_ = nullptr;
    return dialog;
}

void GifRecordWindow::on_video_path_cancelled() {
    if (auto *dialog = take_save_dialog()) dialog->deleteLater();
    if (!close_after_video_ && record_toolbar_) record_toolbar_->reset_state();
}

void GifRecordWindow::on_video_path_selected() {
    auto *dialog = take_save_dialog();
    if (!dialog) return;
    const auto paths = dialog->selectedFiles();
    dialog->deleteLater();
    if (paths.isEmpty() || close_after_video_) return;
    QString path = paths.first();
    if (!path.toLower().endsWith(".mp4")) path += ".mp4";
    if (QFileInfo::exists(path)) {
        show_modeless_warning_dialog(record_toolbar_, tr_text("视频录制"), tr_text("目标文件已存在，请选择新的文件名。"));
        record_toolbar_->reset_state();
        return;
    }
    if (!gif_release_) gif_release_ = recorder_->release();
    recorder_->reset_data();
    pending_video_ = PendingVideo{path, record_toolbar_->get_video_options(), std::chrono::steady_clock::now()};
    record_toolbar_->set_busy(tr_text("正在准备…"));
    overlay_->set_mode(OverlayMode::PASSTHROUGH);
    start_pending_video();
}

void GifRecordWindow::start_pending_video() {
    if (!recorder_) {
        release_timer_.stop();
        return;
    }
    if (!pending_video_) {
        if (gif_release_ && record_toolbar_->get_recording_mode() == "gif") {
            if (!future_done(*gif_release_)) {
                release_timer_.start();
                return;
            }
            const auto release = *gif_release_;
            gif_release_.reset();
            try {
                release.get();
            } catch (const std::exception &error) {
                log_exception(error, T("释放 GIF 会话"));
                release_timer_.stop();
                on_video_failed("release_failed", tr_text("无法释放之前的录制会话，请稍后重试。"));
                return;
            }
            recorder_->prepare();
            record_toolbar_->reset_state();
        }
        release_timer_.stop();
        return;
    }
    const auto [path, options, started] = *pending_video_;
    if (gif_release_ && !future_done(*gif_release_)) {
        if (std::chrono::steady_clock::now() - started > std::chrono::seconds(30)) {
            pending_video_.reset();
            release_timer_.stop();
            on_video_failed("release_timeout", tr_text("无法释放之前的录制会话，请稍后重试。"));
            return;
        }
        release_timer_.start();
        return;
    }
    try {
        if (gif_release_) gif_release_->get();
    } catch (const std::exception &error) {
        log_exception(error, T("释放 GIF 会话"));
        pending_video_.reset();
        release_timer_.stop();
        on_video_failed("release_failed", tr_text("无法释放之前的录制会话，请稍后重试。"));
        return;
    }
    release_timer_.stop();
    pending_video_.reset();
    gif_release_.reset();
    begin_countdown([this, path = path, options = options] { video_->start(rect_, path, options); });
}

void GifRecordWindow::on_video_ready() {
    record_toolbar_->set_video_ready();
    overlay_->set_recording(true);
    overlay_->set_mode(OverlayMode::PASSTHROUGH);
}

void GifRecordWindow::on_video_progress(double elapsed) {
    if (record_toolbar_) record_toolbar_->set_elapsed(elapsed);
}

void GifRecordWindow::on_video_paused(bool paused) {
    record_toolbar_->set_video_paused(paused);
    overlay_->set_recording(true, paused);
}

void GifRecordWindow::on_video_completed(const QString &path) {
    // Re-recording opens only after the helper has exited too, so the capture session is never shared.
    completed_video_path_ = path;
}

void GifRecordWindow::on_video_failed(const QString &code, const QString &message) {
    completed_video_path_.reset();
    log_error(T("视频录制失败: {code}: {message}", {{"code", code}, {"message", message}}), "Video");
    if (!close_after_video_) show_modeless_warning_dialog(record_toolbar_, tr_text("视频录制失败"), failure_message(code, message));
    if (!video_->active()) reset_video_controls();
}

void GifRecordWindow::reset_video_controls() {
    if (!record_toolbar_) return;
    record_toolbar_->reset_state();
    enter_state(AppState::IDLE);
    reposition_current_toolbar();
    request_trim_working_set(1000);
}

void GifRecordWindow::on_video_finished() {
    if (close_after_video_) {
        close_all();
    } else {
        reset_video_controls();
        if (completed_video_path_) {
            record_toolbar_->show_video_result(*completed_video_path_);
            reposition_current_toolbar();
        }
    }
}

void GifRecordWindow::use_video_result(const QString &action) {
    if (!completed_video_path_) return;
    const QString path = *completed_video_path_;
    if (!QFileInfo(path).isFile()) {
        show_modeless_warning_dialog(record_toolbar_, tr_text("视频录制"), tr_text("视频文件不存在，可能已被移动或删除。"));
        return;
    }
    if (action == "copy") {
        auto *mime = new QMimeData;
        mime->setUrls({QUrl::fromLocalFile(path)});
        QApplication::clipboard()->setMimeData(mime);
        record_toolbar_->show_video_copied();
        return;
    }
    const QString target = action == "folder" ? QFileInfo(path).absolutePath() : path;
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(target)))
        show_modeless_warning_dialog(record_toolbar_, tr_text("视频录制"), tr_text("无法打开，请检查默认播放器或文件管理器。"));
}

void GifRecordWindow::rerecord_video() {
    if (video_busy()) return;
    completed_video_path_.reset();
    drawing_view_->clear_all();
    drawing_view_->update_rect(rect_);
    drawing_view_->show();
    reset_video_controls();
    reposition_current_toolbar();
}

// ── Drawing tools ──

void GifRecordWindow::on_drawing_tool_selected(const QString &tool_id) {
    log_debug(T("绘制工具: {tool_id}", {{"tool_id", tool_id}}), "GIF");
    drawing_view_->activate_tool(tool_id);
    // Syncs the current colour and width into the toolbar panels.
    record_toolbar_->sync_from_controller(*drawing_view_->tool_controller());
}

void GifRecordWindow::on_drawing_deactivate() {
    log_debug(T("退出绘制模式"), "GIF");
    drawing_view_->activate_tool("cursor");
    record_toolbar_->highlight_tool(std::nullopt);
}

void GifRecordWindow::on_drawing_color_changed(const QColor &color) {
    drawing_view_->tool_controller()->update_style(color);
    if (drawing_view_->cursor_manager) drawing_view_->cursor_manager->update_tool_cursor_color(color);
    // Selected text items update through SmartEditController.
    if (drawing_view_->smart_edit_controller) drawing_view_->smart_edit_controller->on_text_color_changed(color);
}

void GifRecordWindow::on_drawing_width_changed(int width) {
    drawing_view_->tool_controller()->update_style(std::nullopt, width);
    // As with colour, the brush cursor preview follows the new width.
    if (drawing_view_->cursor_manager) drawing_view_->cursor_manager->update_tool_cursor_size(width);
}

void GifRecordWindow::on_drawing_opacity_changed(int opacity) { drawing_view_->scene_obj()->update_style(std::nullopt, std::nullopt, opacity / 255.0); }

void GifRecordWindow::on_drawing_arrow_style_changed(const QString &style) {
    if (!drawing_view_->smart_edit_controller) return;
    if (auto *item = dynamic_cast<ArrowItem *>(drawing_view_->smart_edit_controller->selected_item())) {
        item->set_arrow_style(style);
        item->update();
    }
}

// ── Recording to playback ──

void GifRecordWindow::switch_to_playback(std::vector<FrameData> &frames) {
    record_toolbar_->hide();
    // Resets icon states; the limit_reached path skips the button callback.
    record_toolbar_->reset_state();
    drawing_view_->activate_tool("cursor");
    drawing_view_->hide();

    playback_->set_context(rect_, recorder_, record_toolbar_, [this](QWidget *toolbar) { reposition_toolbar(toolbar); });
    playback_->start_playback(frames);
    enter_state(AppState::PLAYBACK);
}

void GifRecordWindow::on_rerecord() {
    log_info(T("重新录制"), "GIF");
    // Clears every frame and frees native memory.
    recorder_->reset();
    record_toolbar_->reset_state();
    if (drawing_view_) {
        drawing_view_->clear_all();
        drawing_view_->update_rect(rect_);
        drawing_view_->show();
    }
    record_toolbar_->show();
    reposition_toolbar(record_toolbar_);
    enter_state(AppState::IDLE);
    recorder_->prepare();
}

// ── Closing ──

void GifRecordWindow::close_all() {
    cancel_countdown();
    release_timer_.stop();
    if (!recorder_) return;
    if (save_dialog_) {
        close_after_video_ = true;
        save_dialog_->reject();
    }
    if (pending_video_) {
        pending_video_.reset();
        release_timer_.stop();
    }
    if (video_->active()) {
        // QObject / QProcess stay until MP4 muxing and the process exit finish; no early deleteLater.
        close_after_video_ = true;
        record_toolbar_->set_busy(tr_text("正在保存…"));
        video_->stop();
        return;
    }
    log_info(T("关闭 GIF 窗口"), "GIF");

    // Queued recorder signals must not reach a destroyed toolbar.
    disconnect(recorder_, &FrameRecorder::frame_captured, nullptr, nullptr);
    disconnect(recorder_, &FrameRecorder::limit_reached, nullptr, nullptr);

    for (QWidget *w : std::initializer_list<QWidget *>{overlay_, drawing_view_, record_toolbar_})
        if (w) w->hide();

    // Stopping the capture thread and handing back the session run in the background; closing does not wait.
    recorder_->release();

    playback_->hide_all();
    playback_->disconnect_engine_signals();

    auto [engine, playback_toolbar] = playback_->detach_for_cleanup();
    auto *recorder = recorder_;
    auto *overlay = overlay_;
    auto *drawing_view = drawing_view_;
    auto *record_toolbar = record_toolbar_;

    overlay_ = nullptr;
    drawing_view_ = nullptr;
    record_toolbar_ = nullptr;
    // Late callbacks find no recorder.
    recorder_ = nullptr;

    // The capture thread stops in the background; the rest is light and finishes here.
    if (engine) {
        engine->stop_timer();
        engine->cleanup(true);
    }
    if (recorder) recorder->reset_data();

    if (active_gif_window() == this) {
        active_gif_window() = nullptr;
        log_debug(T("清除 app._gif_window"), "GIF");
    }

    for (QObject *w : std::initializer_list<QObject *>{engine, overlay, drawing_view, record_toolbar, playback_toolbar})
        if (w) w->deleteLater();

    request_trim_working_set(1000);
    emit closed();
    deleteLater();
}
}
