#include "gif/playback_controller.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "core/ui_theme.h"
#include "gif/composer.h"
#include "gif/cursor_overlay.h"
#include "gif/frame_recorder.h"
#include "gif/playback_engine.h"
#include "gif/playback_toolbar.h"
#include "gif/record_toolbar.h"
#include "settings/tool_settings.h"
#include "ui/dialogs.h"
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QMimeData>
#include <QPixmap>
#include <QUrl>

namespace jietuba {
namespace {
QString tr_text(const char *text) { return i18n_tr(text, "GifRecorder"); }

QString rect_text(const QRect &rect) {
    return QString("QRect(%1, %2, %3, %4)").arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height());
}
}

PlaybackController::PlaybackController(QObject *parent) : QObject(parent) {}

void PlaybackController::set_context(const QRect &rect, FrameRecorder *recorder, RecordToolbar *record_toolbar, RepositionFn reposition_fn) {
    rect_ = rect;
    recorder_ = recorder;
    record_toolbar_ = record_toolbar;
    reposition_fn_ = std::move(reposition_fn);
}

// ── Entering / leaving playback ──

void PlaybackController::start_playback(std::vector<FrameData> &frames) {
    log_info(T("切换到回放模式, {frame_count} 帧", {{"frame_count", static_cast<qlonglong>(frames.size())}}), "GIF");

    // The preview layer above the overlay shows frame images.
    preview_label_ = new QLabel;
    preview_label_->setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    preview_label_->setAttribute(Qt::WA_TranslucentBackground, false);
    set_own_style(preview_label_, "background: black;");
    preview_label_->setAlignment(Qt::AlignCenter);
    preview_label_->setGeometry(rect_);
    preview_label_->show();
    log_debug(T("预览层创建, geometry={geometry}", {{"geometry", rect_text(rect_)}}), "GIF");

    // The cursor overlay above the preview layer.
    cursor_overlay_ = new CursorOverlay;
    cursor_overlay_->update_rect(rect_);
    cursor_overlay_->show();
    cursor_overlay_->raise();

    // The playback toolbar.
    const int fps = record_toolbar_->get_current_fps();
    const int total_ms = recorder_->total_duration_ms();
    playback_toolbar_ = new PlaybackToolbar;
    connect_playback_toolbar();
    playback_toolbar_->set_total_frames(static_cast<int>(frames.size()), fps, total_ms);
    playback_toolbar_->show();
    if (reposition_fn_) reposition_fn_(playback_toolbar_);

    // The playback engine.
    engine_ = new PlaybackEngine;
    engine_->load(&frames, fps, recorder_->store());
    engine_->set_display_size(preview_label_->width(), preview_label_->height());
    connect(engine_, &PlaybackEngine::frame_ready, this, &PlaybackController::on_frame_ready);
    connect(engine_, &PlaybackEngine::frame_changed, this, &PlaybackController::on_playback_frame_index);
    connect(engine_, &PlaybackEngine::playback_finished, this, &PlaybackController::on_playback_finished);

    // Decodes and shows the first frame synchronously.
    QApplication::processEvents();
    render_frame(0);
}

void PlaybackController::teardown() {
    if (engine_) {
        disconnect(engine_, nullptr, this, nullptr);
        engine_->cleanup();
        engine_->deleteLater();
        engine_ = nullptr;
    }
    disconnect_and_destroy_playback_toolbar();
    destroy_preview_label();
}

void PlaybackController::hide_all() {
    if (playback_toolbar_) playback_toolbar_->hide();
    if (preview_label_) preview_label_->hide();
    if (cursor_overlay_) cursor_overlay_->hide();
}

void PlaybackController::disconnect_engine_signals() {
    if (!engine_) return;
    disconnect(engine_, &PlaybackEngine::frame_ready, nullptr, nullptr);
    disconnect(engine_, &PlaybackEngine::frame_changed, nullptr, nullptr);
    disconnect(engine_, &PlaybackEngine::playback_finished, nullptr, nullptr);
}

std::pair<PlaybackEngine *, PlaybackToolbar *> PlaybackController::detach_for_cleanup() {
    auto *engine = engine_;
    auto *toolbar = playback_toolbar_;
    engine_ = nullptr;
    playback_toolbar_ = nullptr;
    destroy_preview_label();
    return {engine, toolbar};
}

// ── Signal wiring ──

void PlaybackController::connect_playback_toolbar() {
    auto *tb = playback_toolbar_;
    connect(tb, &PlaybackToolbar::play_pause, this, &PlaybackController::on_play_pause);
    connect(tb, &PlaybackToolbar::speed_changed, this, &PlaybackController::on_speed_changed);
    connect(tb, &PlaybackToolbar::seek_requested, this, &PlaybackController::on_seek);
    connect(tb, &PlaybackToolbar::trim_changed, this, &PlaybackController::on_trim_changed);
    connect(tb, &PlaybackToolbar::save_requested, this, &PlaybackController::on_save);
    connect(tb, &PlaybackToolbar::copy_requested, this, &PlaybackController::on_copy);
    connect(tb, &PlaybackToolbar::rerecord, this, &PlaybackController::on_rerecord);
    connect(tb, &PlaybackToolbar::close_requested, this, &PlaybackController::close_requested);
    connect(tb, &PlaybackToolbar::cursor_toggled, this, &PlaybackController::on_cursor_toggled);
}

void PlaybackController::disconnect_and_destroy_playback_toolbar() {
    if (!playback_toolbar_) return;
    auto *tb = playback_toolbar_;
    for (const auto &signal : {&PlaybackToolbar::play_pause, &PlaybackToolbar::save_requested, &PlaybackToolbar::copy_requested,
                               &PlaybackToolbar::rerecord, &PlaybackToolbar::close_requested})
        disconnect(tb, signal, nullptr, nullptr);
    disconnect(tb, &PlaybackToolbar::speed_changed, nullptr, nullptr);
    disconnect(tb, &PlaybackToolbar::seek_requested, nullptr, nullptr);
    disconnect(tb, &PlaybackToolbar::trim_changed, nullptr, nullptr);
    disconnect(tb, &PlaybackToolbar::cursor_toggled, nullptr, nullptr);
    tb->hide();
    tb->deleteLater();
    playback_toolbar_ = nullptr;
}

// ── Playback callbacks ──

void PlaybackController::on_play_pause() {
    if (!engine_) return;
    if (engine_->is_playing()) {
        log_debug(T("回放暂停"), "GIF");
        engine_->pause();
    } else {
        log_debug(T("回放开始"), "GIF");
        engine_->play();
    }
}

void PlaybackController::on_speed_changed(double speed) {
    log_debug(T("回放速度: {speed}", {{"speed", speed}}), "GIF");
    export_speed_ = speed;
    if (engine_) engine_->set_speed(speed);
}

void PlaybackController::on_seek(int index) {
    log_debug(T("跳转帧: {index}", {{"index", index}}), "GIF");
    if (engine_) {
        engine_->seek(index);
        if (!engine_->is_playing()) render_frame(index);
    }
}

void PlaybackController::on_trim_changed(int start, int end) {
    if (engine_) engine_->set_trim(start, end);
}

void PlaybackController::on_rerecord() {
    // The record window handles the actual reset.
    log_info(T("重新录制（回放控制器）"), "GIF");
    teardown();
    emit rerecord_requested();
}

// ── Frame rendering ──

void PlaybackController::render_frame(int index) {
    if (!engine_ || !preview_label_) return;
    try {
        const int w = preview_label_->width();
        const int h = preview_label_->height();
        const QImage img = engine_->get_frame_image(index, w, h);
        if (img.isNull()) return;
        preview_label_->setPixmap(QPixmap::fromImage(img));
        update_cursor_for_frame(index);
    } catch (const std::exception &e) {
        log_error(T("帧渲染失败 index={index}: {e}", {{"index", index}, {"e", QString::fromUtf8(e.what())}}), "GIF");
    }
}

void PlaybackController::on_frame_ready(const QImage &image, int index) {
    if (preview_label_) preview_label_->setPixmap(QPixmap::fromImage(image));
    update_cursor_for_frame(index);
    update_playhead(index);
}

void PlaybackController::on_playback_frame_index(int index) {
    if (engine_ && !engine_->is_playing()) update_playhead(index);
}

void PlaybackController::update_playhead(int index) {
    if (!playback_toolbar_) return;
    const auto &frames = recorder_->frames();
    const int total = static_cast<int>(frames.size());
    const int fps = record_toolbar_->get_current_fps();
    const int current_ms = index < total ? frames[static_cast<size_t>(index)].elapsed_ms : -1;
    playback_toolbar_->set_playhead(index, total, fps, current_ms);
}

void PlaybackController::on_playback_finished() {
    log_debug(T("回放结束"), "GIF");
    const int start = engine_ ? engine_->trim_start() : 0;
    if (engine_) {
        engine_->seek(start);
        render_frame(start);
    }
    if (playback_toolbar_) {
        playback_toolbar_->set_playing(false);
        const auto &frames = recorder_->frames();
        const int total = static_cast<int>(frames.size());
        const int fps = record_toolbar_->get_current_fps();
        const int start_ms = start < total ? frames[static_cast<size_t>(start)].elapsed_ms : -1;
        playback_toolbar_->set_playhead(start, total, fps, start_ms);
    }
}

// ── Cursor overlay ──

void PlaybackController::update_cursor_for_frame(int index) {
    if (!cursor_overlay_) return;
    const auto &frames = recorder_->frames();
    if (0 <= index && index < static_cast<int>(frames.size())) cursor_overlay_->set_frame_cursor(frames[static_cast<size_t>(index)].cursor);
    else cursor_overlay_->set_frame_cursor(std::nullopt);
}

void PlaybackController::on_cursor_toggled(bool visible) {
    cursor_export_enabled_ = visible;
    if (cursor_overlay_) cursor_overlay_->set_cursor_visible(visible);
}

// ── Preview layer ──

void PlaybackController::destroy_preview_label() {
    if (cursor_overlay_) {
        cursor_overlay_->hide();
        cursor_overlay_->deleteLater();
        cursor_overlay_ = nullptr;
    }
    if (preview_label_) {
        preview_label_->setPixmap(QPixmap());
        preview_label_->hide();
        preview_label_->deleteLater();
        preview_label_ = nullptr;
        log_debug(T("预览层销毁"), "GIF");
    }
}

// ── Export: save / copy ──

std::pair<int, int> PlaybackController::get_trim_range() const {
    if (playback_toolbar_) return playback_toolbar_->get_trim_range();
    return {0, static_cast<int>(recorder_->frames().size()) - 1};
}

void PlaybackController::on_save() {
    // During playback only the playback toolbar is usable; locking it blocks repeated saves and closing.
    const auto path = get_save_file_name(playback_toolbar_, tr_text("Save GIF"), make_save_path(), tr_text("GIF animation (*.gif)")).first;
    if (path.isEmpty()) {
        log_debug(T("保存取消"), "GIF");
        return;
    }
    log_info(T("保存 GIF: {path}", {{"path", path}}), "GIF");
    compose_and_export(path, false);
}

void PlaybackController::on_copy() {
    const QString out = make_save_path();
    log_info(T("复制 GIF → {out}", {{"out", out}}), "GIF");
    compose_and_export(out, true);
}

void PlaybackController::compose_and_export(const QString &output_path, bool copy_to_clipboard) {
    const auto &frames = recorder_->frames();
    const auto store = recorder_->store();
    const auto copy_text = copy_to_clipboard ? QString("True") : QString("False");
    if (store)
        log_info(T("开始导出 GIF: frames={frame_count}, copy={copy_to_clipboard}, store=有", {{"frame_count", static_cast<qlonglong>(frames.size())}, {"copy_to_clipboard", copy_text}}), "GIF");
    else
        log_info(T("开始导出 GIF: frames={frame_count}, copy={copy_to_clipboard}, store=无", {{"frame_count", static_cast<qlonglong>(frames.size())}, {"copy_to_clipboard", copy_text}}), "GIF");

    if (frames.empty()) {
        log_warning(T("帧列表为空，中止导出"), "GIF");
        return;
    }

    // The trim range.
    const int last = static_cast<int>(frames.size()) - 1;
    auto [trim_start, trim_end] = get_trim_range();
    trim_start = std::max(0, std::min(trim_start, last));
    trim_end = std::max(trim_start, std::min(trim_end, last));
    log_info(T("导出裁剪范围: [{trim_start}, {trim_end}]，共 {frame_count} 帧", {{"trim_start", trim_start}, {"trim_end", trim_end}, {"frame_count", trim_end - trim_start + 1}}), "GIF");

    // Cursor export parameters.
    ComposeOptions options;
    options.path = output_path;
    options.store = store;
    options.frame_start = trim_start;
    options.frame_end = trim_end;
    options.speed_multiplier = export_speed_;
    if (cursor_export_enabled_ && store) {
        const int src_w = store->width();
        const int gif_width = DEFAULT_GIF_WIDTH;
        const double scale = src_w > 0 && gif_width > 0 ? static_cast<double>(gif_width) / src_w : 1.0;

        options.cursor_sprites = rasterize_cursor_sprites(scale);

        std::vector<std::optional<CursorSnapshot>> cursors;
        cursors.reserve(frames.size());
        for (const auto &frame : frames) cursors.push_back(frame.cursor);
        const auto burst_states = compute_burst_states(cursors);
        std::vector<std::optional<gifrecorder::CursorInfo>> cursor_infos;
        cursor_infos.reserve(cursors.size());
        for (size_t i = 0; i < cursors.size(); ++i) {
            const auto &cur = cursors[i];
            if (!cur || !cur->visible) {
                cursor_infos.emplace_back(std::nullopt);
            } else {
                const auto [bf, bs] = burst_states[i];
                cursor_infos.emplace_back(gifrecorder::CursorInfo{static_cast<int>(cur->x * scale), static_cast<int>(cur->y * scale), cur->press, cur->scroll, bf, bs});
            }
        }

        int n_active = 0;
        for (int i = trim_start; i <= trim_end; ++i)
            if (cursor_infos[static_cast<size_t>(i)]) ++n_active;
        log_info(T("鼠标导出: {n_active}/{frame_count} 帧有光标", {{"n_active", n_active}, {"frame_count", trim_end - trim_start + 1}}), "GIF");
        options.cursor_infos = std::move(cursor_infos);
    }

    const auto result = ComposerProgressDialog::run_compose(std::move(options), nullptr, rect_.center());

    if (std::holds_alternative<std::monostate>(result)) {
        log_warning(T("GIF 导出取消或失败"), "GIF");
        return;
    }

    const QString result_text = std::holds_alternative<QString>(result) ? std::get<QString>(result) : QString("<%1 bytes>").arg(std::get<QByteArray>(result).size());
    log_info(T("GIF 导出完成: {result}", {{"result", result_text}}), "GIF");

    if (copy_to_clipboard) copy_file_to_clipboard(output_path, "image/gif");

    emit close_requested();
}

void PlaybackController::copy_file_to_clipboard(const QString &path, const QString &mime_type) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        log_error(T("复制到剪贴板失败: {e}", {{"e", file.errorString()}}), "GIF");
        return;
    }
    const QByteArray data_bytes = file.readAll();
    auto *mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(path)});
    mime->setData(mime_type, data_bytes);
    QApplication::clipboard()->setMimeData(mime);
    log_info(T("已复制到剪贴板 [{mime_type}], 大小={size} bytes", {{"mime_type", mime_type}, {"size", static_cast<qlonglong>(data_bytes.size())}}), "GIF");
}

QString PlaybackController::make_save_path() {
    QString save_dir;
    try {
        save_dir = get_tool_settings_manager()->get_screenshot_save_path();
    } catch (const std::exception &e) {
        log_exception(e, T("获取截图保存路径"));
        save_dir = QDir(QDir::homePath()).filePath("Desktop");
    }
    QDir().mkpath(save_dir);
    const QString ts = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    return QDir::toNativeSeparators(QDir(save_dir).filePath(QString("jietuba_gif_%1.gif").arg(ts)));
}
}
