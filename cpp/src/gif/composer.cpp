#include "gif/composer.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "gif/_widgets.h"
#include "ui/dialogs.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProgressBar>
#include <QScreen>
#include <QTemporaryFile>

namespace jietuba {
namespace {
QString tr_text(const char *text) { return i18n_tr(text, "GifRecorder"); }
}

ComposeWorker::ComposeWorker(ComposeOptions options) : options_(std::move(options)) { options_.speed_multiplier = std::max(0.1, options_.speed_multiplier); }

void ComposeWorker::cancel() {
    cancel_ = true;
    if (options_.store) options_.store->cancel_export();
}

void ComposeWorker::release_inputs() {
    options_.store.reset();
    options_.cursor_sprites.reset();
    options_.cursor_infos.reset();
}

void ComposeWorker::run() {
    try {
        if (!options_.store) {
            emit finished(false, ComposeResult(), true);
            return;
        }
        const auto result = compose();
        emit finished(!cancel_, result, false);
    } catch (const std::exception &e) {
        log_error(T("GIF 合成失败: {e}", {{"e", QString::fromUtf8(e.what())}}), "GIF");
        emit finished(false, ComposeResult(), false);
    }
}

ComposeResult ComposeWorker::compose() {
    const bool need_bytes = !options_.path.has_value();
    QString out_path;
    if (need_bytes) {
        QTemporaryFile temp(QDir(QDir::tempPath()).filePath("jietuba_XXXXXX.gif"));
        temp.setAutoRemove(false);
        if (!temp.open()) throw std::runtime_error(temp.errorString().toStdString());
        out_path = temp.fileName();
        temp.close();
    } else {
        out_path = *options_.path;
    }
    const bool ok = do_compose(out_path);
    if (!ok || cancel_) {
        if (need_bytes && !QFile::remove(out_path)) log_exception("OSError", out_path, T("GIF 取消后删除临时文件"));
        return {};
    }
    if (!need_bytes) return out_path;
    QFile file(out_path);
    QByteArray bytes;
    if (file.open(QIODevice::ReadOnly)) bytes = file.readAll();
    file.close();
    if (!QFile::remove(out_path)) log_exception("OSError", out_path, T("GIF 读取后删除临时文件"));
    return bytes;
}

bool ComposeWorker::do_compose(const QString &out_path) {
    // The output size: scaled to gif_width with an even height, or the original.
    int gif_width = options_.gif_width;
    int gif_height = 0;
    const auto &store = *options_.store;
    if (gif_width > 0 && store.width() > 0) {
        const double ratio = static_cast<double>(gif_width) / store.width();
        gif_height = std::max(2, static_cast<int>(store.height() * ratio));
        gif_height = gif_height % 2 == 0 ? gif_height : gif_height - 1;
    } else {
        gif_width = store.width();
        gif_height = store.height();
    }
    log_info(T("使用 gifrecorder export_gif: {gif_width}x{gif_height}", {{"gif_width", gif_width}, {"gif_height", gif_height}}), "GIF");

    // Returning false cancels.
    auto progress = [this](size_t done, size_t total) {
        emit this->progress(static_cast<int>(done), static_cast<int>(total));
        return !cancel_;
    };
    try {
        const bool with_cursor = options_.cursor_sprites && options_.cursor_infos && !options_.cursor_infos->empty();
        store.export_gif(out_path, gif_width, gif_height, 0, options_.frame_start, options_.frame_end, progress, with_cursor ? &*options_.cursor_sprites : nullptr,
                         with_cursor ? &*options_.cursor_infos : nullptr, options_.speed_multiplier);
    } catch (const std::exception &e) {
        if (QString::fromUtf8(e.what()).contains("cancelled")) return false;
        log_error(T("export_gif 失败: {e}", {{"e", QString::fromUtf8(e.what())}}), "GIF");
        return false;
    }
    const QFileInfo info(out_path);
    const double size_kb = info.isFile() ? info.size() / 1024.0 : 0.0;
    log_info(T("GIF 导出完成: {out_path} ({size_kb:.1f} KB)", {{"out_path", out_path}, {"size_kb", size_kb}}), "GIF");
    return true;
}

ComposerProgressDialog::ComposerProgressDialog(QWidget *) : QFrame(nullptr) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedSize(260, 14);
    bar_ = new QProgressBar(this);
    bar_->setRange(0, 100);
    bar_->setTextVisible(false);
    bar_->setGeometry(0, 0, 260, 14);
    bar_->setStyleSheet(PROGRESS_BAR_STYLE);
}

void ComposerProgressDialog::center_on(QWidget *parent, std::optional<QPoint> center_pos) {
    if (center_pos) {
        move(center_pos->x() - width() / 2, center_pos->y() - height() / 2);
        return;
    }
    if (parent) {
        const auto c = parent->geometry().center();
        move(c.x() - width() / 2, c.y() - height() / 2);
        return;
    }
    const auto screen = QApplication::primaryScreen()->geometry();
    move(screen.center().x() - width() / 2, screen.center().y() - height() / 2);
}

void ComposerProgressDialog::start(ComposeOptions options) {
    qRegisterMetaType<ComposeResult>();
    auto *thread = new QThread;
    auto *worker = new ComposeWorker(std::move(options));
    worker->moveToThread(thread);
    connect(thread, &QThread::started, worker, &ComposeWorker::run);
    // Deleted in its own thread as the thread finishes: once the thread has ended, a deleteLater never runs.
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &ComposeWorker::progress, this, &ComposerProgressDialog::on_progress);
    connect(worker, &ComposeWorker::finished, this, &ComposerProgressDialog::on_finished);
    thread_ = thread;
    worker_ = worker;
    thread->start();
}

void ComposerProgressDialog::on_progress(int done, int total) {
    if (total <= 0) {
        if (bar_->maximum() != 0) bar_->setRange(0, 0);
    } else {
        if (bar_->maximum() == 0) bar_->setRange(0, 100);
        bar_->setValue(static_cast<int>(static_cast<double>(done) / total * 100));
    }
}

void ComposerProgressDialog::on_finished(bool ok, const ComposeResult &result, bool unavailable) {
    ok_ = ok;
    result_ = result;
    // Drops the worker's store reference first so the native store is not kept alive longer.
    if (worker_) worker_->release_inputs();
    if (thread_) {
        thread_->quit();
        thread_->wait();
        thread_->deleteLater();
        thread_ = nullptr;
    }
    worker_ = nullptr;
    hide();
    loop_.quit();
    if (!ok && unavailable)
        show_warning_dialog(nullptr, tr_text("gifrecorder unavailable"),
                            tr_text("The gifrecorder module was not found, so the GIF cannot be exported.\n\nMake sure gifrecorder is installed correctly."));
}

ComposeResult ComposerProgressDialog::run_compose(ComposeOptions options, QWidget *parent, std::optional<QPoint> center_pos) {
    auto *dialog = new ComposerProgressDialog(parent);
    dialog->center_on(parent, center_pos);
    dialog->start(std::move(options));
    dialog->show();
    QApplication::processEvents();
    // A composition that finished during processEvents already quit the loop, which
    // exec() would not see.
    if (dialog->thread_) dialog->loop_.exec();
    dialog->deleteLater();
    return dialog->ok_ ? dialog->result_ : ComposeResult();
}
}
