#include "ui/settings_ui/ppocr_model_dialog.h"
#include "core/background_tasks.h"
#include "core/i18n.h"
#include "core/ui_scale.h"
#include "ocr/ocr_manager.h"
#include "ocr/ppocr_models.h"
#include "ui/dialogs.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/labels.h"
#include "ui/fluent_lite/theme.h"
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QGuiApplication>
#include <QProgressBar>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <memory>
#include <stdexcept>

namespace jietuba {
namespace {
QString tr_text(const char *text) { return i18n_tr(text, "PpocrModelDialog"); }

// The speed covers the last few seconds: progress arrives about every 100 ms, and a speed per
// event would keep jumping.
constexpr double SPEED_WINDOW = 3.0;

QString megabytes(qint64 size) { return tr_text("%1 MB").replace("%1", QString::number(size / 1048576.0, 'f', 1)); }

qint64 files_size(const QJsonArray &files) {
    qint64 size = 0;
    for (const auto &file : files) size += file.toObject().value("size").toVariant().toLongLong();
    return size;
}

}

// One row: name, languages/size/state, and one action button (download or delete) on the right.
// The row being downloaded shows a progress bar, the bytes so far and the speed, and a cancel button.
class ModelRow : public QWidget {
public:
    ModelRow(const QString &title, const QString &detail, QString action, bool downloading, QWidget *parent) : QWidget(parent) {
        title_label = new BodyLabel(title, this);
        detail_label = new CaptionLabel(detail, this);
        detail_label->setWordWrap(true);
        auto *text = new QVBoxLayout;
        text->setContentsMargins(0, 0, 0, 0);
        text->setSpacing(dialog_scaled(2));
        text->addWidget(title_label);
        text->addWidget(detail_label);
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(dialog_scaled(10), dialog_scaled(6), dialog_scaled(10), dialog_scaled(6));
        layout->setSpacing(dialog_scaled(10));
        layout->addLayout(text, 1);
        if (downloading) {
            action = tr_text("Cancel");
            progress = new QProgressBar(this);
            progress->setTextVisible(false);
            progress->setRange(0, 0);
            progress->setFixedWidth(dialog_scaled(180));
            progress_label = new CaptionLabel(QString(), this);
            auto *column = new QVBoxLayout;
            column->setContentsMargins(0, 0, 0, 0);
            column->setSpacing(dialog_scaled(2));
            column->addWidget(progress);
            column->addWidget(progress_label);
            layout->addLayout(column);
        }
        if (!action.isEmpty()) {
            button = new PushButton(action, this);
            layout->addWidget(button, 0, Qt::AlignVCenter);
        }
    }
    void set_progress(qint64 received, qint64 total, std::optional<double> speed) {
        progress->setRange(0, 1000);
        progress->setValue(static_cast<int>(received * 1000 / total));
        QString text = megabytes(received) + " / " + megabytes(total);
        if (speed) text += " · " + tr_text("%1 MB/s").replace("%1", QString::number(*speed / 1048576.0, 'f', 1));
        progress_label->setText(text);
    }
    QLabel *title_label;
    QLabel *detail_label;
    QPushButton *button = nullptr;
    QProgressBar *progress = nullptr;
    QLabel *progress_label = nullptr;
};

QString ppocr_language_names(const QStringList &codes) {
    const QHash<QString, QString> names{{"zh", tr_text("Chinese")}, {"ja", tr_text("Japanese")}, {"en", tr_text("English")}, {"ko", tr_text("Korean")},
                                        {"latin", tr_text("Latin-script languages")}, {"cyrillic", tr_text("Cyrillic")}, {"th", tr_text("Thai")}, {"el", tr_text("Greek")}};
    QStringList shown;
    for (const auto &code : codes) shown << names.value(code, code);
    return shown.join(tr_text(", "));
}

PpocrModelDialog::PpocrModelDialog(QWidget *parent) : QDialog(parent) {
    auto timer = std::make_shared<QElapsedTimer>();
    timer->start();
    clock_ = [timer] { return timer->nsecsElapsed() / 1e9; };
    scale_dialog_font(*this);
    setWindowTitle(tr_text("Manage PP-OCR Models"));
    setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
    setStyleSheet(QString("QDialog { background: %1; }").arg(ui_tokens(this).window));

    download_ = new ModelDownload(this);
    connect(download_, &ModelDownload::progress, this, &PpocrModelDialog::on_progress);
    connect(download_, &ModelDownload::finished, this, &PpocrModelDialog::on_downloaded);
    connect(download_, &ModelDownload::failed, this, [this](const QString &reason) { status_->setText(reason); });
    connect(download_, &ModelDownload::stopped, this, &PpocrModelDialog::on_download_stopped);
    connect(this, &PpocrModelDialog::import_done, this, &PpocrModelDialog::on_imported, Qt::QueuedConnection);

    rows_ = new QWidget(this);
    rows_layout_ = new QVBoxLayout(rows_);
    rows_layout_->setContentsMargins(0, 0, 0, 0);
    rows_layout_->setSpacing(dialog_scaled(2));
    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setWidget(rows_);

    status_ = new CaptionLabel(QString(), this);
    status_->setWordWrap(true);

    auto *hint = new CaptionLabel(tr_text("Models are stored in the models folder next to the program. Recognition uses the model selected on the settings page."), this);
    hint->setWordWrap(true);
    configure_dialog_control(*hint);

    add_btn_ = new PushButton(tr_text("Add Local Model..."), this);
    connect(add_btn_, &QPushButton::clicked, this, &PpocrModelDialog::add_local);
    auto *folder_btn = new TransparentPushButton(tr_text("Open Models Folder"), this);
    connect(folder_btn, &QPushButton::clicked, this, &PpocrModelDialog::open_folder);
    auto *close_btn = new PrimaryPushButton(tr_text("Close"), this);
    connect(close_btn, &QPushButton::clicked, this, &PpocrModelDialog::reject);
    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(dialog_scaled(8));
    buttons->addWidget(add_btn_);
    buttons->addWidget(folder_btn);
    buttons->addStretch(1);
    buttons->addWidget(close_btn);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(dialog_scaled(16), dialog_scaled(16), dialog_scaled(16), dialog_scaled(14));
    root->setSpacing(dialog_scaled(10));
    root->addWidget(scroll_, 1);
    root->addWidget(status_);
    root->addWidget(hint);
    root->addLayout(buttons);

    configure_dialog_controls(*this);
    fill();
    fit_height();
}

// No scroll bar while the rows fit; the list scrolls only past the screen's available height.
void PpocrModelDialog::fit_height() {
    auto *root = layout();
    const auto margins = root->contentsMargins();
    int others = margins.top() + margins.bottom() + root->spacing() * (root->count() - 1);
    for (int i = 0; i < root->count(); ++i)
        if (root->itemAt(i)->widget() != scroll_) others += root->itemAt(i)->sizeHint().height();
    const QScreen *screen = this->screen() ? this->screen() : QGuiApplication::primaryScreen();
    const int limit = screen->availableGeometry().height() - dialog_scaled(80);
    // Rows hint taller than they lay out; their minimum leaves no gap at the bottom.
    resize(dialog_scaled(560), std::min(others + rows_layout_->minimumSize().height(), limit));
}

void PpocrModelDialog::fill() {
    QScrollBar *scroll_bar = scroll_->verticalScrollBar();
    const int position = scroll_bar->value();
    // Taking the old rows out collapses the content and sends the scroll bar to the top; the old
    // height holds until the new rows are shown.
    rows_->setMinimumHeight(rows_->height());
    while (auto *item = rows_layout_->takeAt(0)) {
        // Hidden first: deletion waits for the event loop, and until then the old row would paint over the new one.
        if (item->widget()) {
            item->widget()->hide();
            item->widget()->deleteLater();
        }
        delete item;
    }
    const bool busy = download_->busy() || importing_;
    download_row_ = nullptr;
    auto add_row = [this, busy](const QString &title, const QString &detail, const QString &action, bool downloading = false) {
        auto *row = new ModelRow(title, detail, action, downloading, rows_);
        row->setObjectName("ppocr_model_row");
        if (downloading) {
            connect(row->button, &QPushButton::clicked, download_, &ModelDownload::cancel);
            download_row_ = row;
            if (last_progress_) row->set_progress(last_progress_->received, last_progress_->total, last_progress_->speed);
        } else if (row->button) {
            row->button->setEnabled(!busy);
        }
        rows_layout_->addWidget(row);
        return row;
    };
    // This build loads models/onnxruntime.dll at run time; without it no model can run.
    if (!ppocr_runtime_supported(ppocr_models::runtime_path())) {
        const auto runtime = ppocr_models::runtime_entry();
        if (runtime) {
            // What gets downloaded: the wheel when the DLL comes out of one.
            const auto file = runtime->value("file").toObject();
            const qint64 size = (file.contains("archive") ? file.value("archive").toObject() : file).value("size").toVariant().toLongLong();
            const bool downloading = downloading_id_ == ModelDownload::RUNTIME;
            auto *row = add_row("ONNX Runtime", QStringList{tr_text("Needed by PP-OCR"), megabytes(size), downloading ? tr_text("Downloading...") : tr_text("Not downloaded")}.join(" · "),
                                downloading ? QString() : tr_text("Download"), downloading);
            if (!downloading) connect(row->button, &QPushButton::clicked, this, [this] { start_download(ModelDownload::RUNTIME, "ONNX Runtime"); });
        } else {
            add_row("ONNX Runtime", tr_text("Missing: put onnxruntime.dll (version 1.24 or later) in the models folder."), {});
        }
    }
    const QJsonArray installed = ppocr_models::installed();
    QHash<QString, QJsonObject> by_catalog;
    for (const auto &value : installed) {
        const auto model = value.toObject();
        if (model.value("catalog_id").isString()) by_catalog.insert(model.value("catalog_id").toString(), model);
    }
    for (const auto &value : ppocr_models::catalog().value("models").toArray()) {
        const auto entry = value.toObject();
        const QString id = entry.value("id").toString();
        QStringList languages;
        for (const auto &code : entry.value("languages").toArray()) languages << code.toString();
        QString state, action;
        const bool downloading = id == downloading_id_;
        if (downloading) {
            state = tr_text("Downloading...");
        } else if (!by_catalog.contains(id)) {
            state = tr_text("Not downloaded");
            action = tr_text("Download");
        } else if (by_catalog.value(id).value("id").toString().isEmpty()) {
            state = tr_text("Included");
            action = tr_text("Delete");
        } else {
            state = tr_text("Downloaded");
            action = tr_text("Delete");
        }
        const QString name = entry.value("name").toString();
        auto *row = add_row(name, QStringList{ppocr_language_names(languages), megabytes(files_size(entry.value("files").toArray())), state}.join(" · "), action, downloading);
        if (!row->button || downloading) continue;
        if (!by_catalog.contains(id)) connect(row->button, &QPushButton::clicked, this, [this, id, name] { start_download(id, name); });
        else connect(row->button, &QPushButton::clicked, this, [this, model = by_catalog.value(id)] { remove_model(model); });
    }
    for (const auto &value : installed) {
        const auto model = value.toObject();
        if (model.value("catalog_id").isString()) continue;
        auto *row = add_row(model.value("name").toString(), tr_text("Added manually"), tr_text("Delete"));
        connect(row->button, &QPushButton::clicked, this, [this, model] { remove_model(model); });
    }
    rows_layout_->addStretch(1);
    configure_dialog_controls(*rows_);
    add_btn_->setEnabled(!busy);
    // New rows show from the event loop; release the height and scroll back after that.
    QTimer::singleShot(0, this, [this, scroll_bar, position] {
        rows_->setMinimumHeight(0);
        scroll_bar->setValue(position);
    });
}

QVector<PpocrModelDialog::RowInfo> PpocrModelDialog::rows() const {
    QVector<RowInfo> result;
    for (int i = 0; i < rows_layout_->count(); ++i)
        if (auto *row = dynamic_cast<ModelRow *>(rows_layout_->itemAt(i)->widget()))
            result.append({row->title_label->text(), row->detail_label->text(), row->button ? row->button->text() : QString(), row->button,
                           row->progress ? row->progress->value() : -1, row->progress_label ? row->progress_label->text() : QString()});
    return result;
}

QString PpocrModelDialog::status_text() const { return status_->text(); }

void PpocrModelDialog::show_status(const QString &text) { status_->setText(text); }

void PpocrModelDialog::start_download(const QString &catalog_id, const QString &name) {
    if (!download_->start(catalog_id)) {
        status_->setText(tr_text("Could not start the download."));
        return;
    }
    download_name_ = name;
    downloading_id_ = catalog_id;
    last_progress_.reset();
    samples_.clear();
    status_->clear();
    fill();
}

void PpocrModelDialog::on_progress(qint64 received, qint64 total) {
    if (total <= 0) return;
    const double now = clock_();
    samples_.emplace_back(now, received);
    while (samples_.size() > 2 && now - samples_.front().first > SPEED_WINDOW) samples_.pop_front();
    const auto [start, start_received] = samples_.front();
    std::optional<double> speed;
    if (now - start >= 0.5) speed = (received - start_received) / (now - start);
    last_progress_ = Progress{received, total, speed};
    if (download_row_) download_row_->set_progress(received, total, speed);
}

void PpocrModelDialog::on_downloaded(const QJsonObject &) {
    status_->setText(tr_text("%1 is ready.").replace("%1", download_name_));
    emit models_changed();
}

void PpocrModelDialog::on_download_stopped() {
    downloading_id_.clear();
    last_progress_.reset();
    fill();
    if (close_after_download_) QDialog::reject();
}

void PpocrModelDialog::add_local() {
    PpocrImportForm form(this);
    if (exec_dialog(&form) != QDialog::Accepted) return;
    const QString name = form.name(), det = form.det(), rec = form.rec(), dict = form.dict();
    importing_ = true;
    status_->setText(tr_text("Checking %1...").replace("%1", name));
    fill();
    background_tasks::start_thread(
        [this, name, det, rec, dict] {
            try {
                emit import_done(ppocr_models::import_model(name, det, rec, dict), {});
            } catch (const std::exception &e) {
                emit import_done({}, QString::fromUtf8(e.what()));
            }
        },
        "PP-OCR model import");
}

void PpocrModelDialog::on_imported(const QJsonObject &model, const QString &error) {
    importing_ = false;
    fill();
    if (model.isEmpty()) {
        status_->clear();
        show_warning_dialog(this, tr_text("Could not add the model"), error);
        return;
    }
    status_->setText(tr_text("%1 is ready.").replace("%1", model.value("name").toString()));
    emit models_changed();
}

void PpocrModelDialog::remove_model(const QJsonObject &model) {
    const QString name = model.value("name").toString();
    if (!show_confirm_dialog(this, tr_text("Delete Model"), tr_text("Delete %1 and its files?").replace("%1", name))) return;
    try {
        ppocr_models::remove(model.value("id").toString());
    } catch (const std::exception &e) {
        show_warning_dialog(this, tr_text("Could not delete the model"), QString::fromUtf8(e.what()));
        return;
    }
    fill();
    status_->clear();
    emit model_removed(model.value("id").toString());
    emit models_changed();
}

void PpocrModelDialog::open_folder() {
    QDir().mkpath(ppocr_models::models_root());
    QDesktopServices::openUrl(QUrl::fromLocalFile(ppocr_models::models_root()));
}

void PpocrModelDialog::reject() {
    // Closing during a download cancels it first and closes once the helper has exited and
    // removed its temporary files. An import in progress finishes before the window closes.
    if (download_->busy()) {
        close_after_download_ = true;
        download_->cancel();
        return;
    }
    if (importing_) return;
    QDialog::reject();
}

PpocrImportForm::PpocrImportForm(QWidget *parent) : QDialog(parent) {
    scale_dialog_font(*this);
    setWindowTitle(tr_text("Add Local Model"));
    setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
    setStyleSheet(QString("QDialog { background: %1; }").arg(ui_tokens(this).window));
    name_ = new LineEdit(this);
    det_ = new LineEdit(this);
    rec_ = new LineEdit(this);
    dict_ = new LineEdit(this);
    dict_->setPlaceholderText(tr_text("Optional: only if the recognition model has no built-in dictionary"));
    const QString onnx = tr_text("ONNX models (*.onnx)");
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(dialog_scaled(8));
    grid->setVerticalSpacing(dialog_scaled(8));
    const std::tuple<QString, QLineEdit *, QString> rows[] = {
        {tr_text("Name"), name_, {}},
        {tr_text("Detection model"), det_, onnx},
        {tr_text("Recognition model"), rec_, onnx},
        {tr_text("Dictionary"), dict_, tr_text("Text files (*.txt)")},
    };
    int index = 0;
    for (const auto &[label, edit, filter] : rows) {
        grid->addWidget(new BodyLabel(label, this), index, 0);
        grid->addWidget(edit, index, 1);
        if (!filter.isEmpty()) {
            auto *browse_btn = new PushButton(tr_text("Browse..."), this);
            connect(browse_btn, &QPushButton::clicked, this, [this, edit = edit, filter = filter] { browse(edit, filter); });
            grid->addWidget(browse_btn, index, 2);
        }
        ++index;
    }
    ok_ = new PrimaryPushButton(tr_text("Add"), this);
    connect(ok_, &QPushButton::clicked, this, &QDialog::accept);
    auto *cancel = new PushButton(tr_text("Cancel"), this);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(ok_);
    buttons->addWidget(cancel);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(dialog_scaled(16), dialog_scaled(16), dialog_scaled(16), dialog_scaled(14));
    root->setSpacing(dialog_scaled(12));
    root->addLayout(grid);
    root->addLayout(buttons);
    for (auto *edit : {name_, det_, rec_}) connect(edit, &QLineEdit::textChanged, this, &PpocrImportForm::update_ok);
    configure_dialog_controls(*this);
    update_ok();
    resize(dialog_scaled(560), sizeHint().height());
}

QString PpocrImportForm::name() const { return name_->text().trimmed(); }
QString PpocrImportForm::det() const { return det_->text().trimmed(); }
QString PpocrImportForm::rec() const { return rec_->text().trimmed(); }
QString PpocrImportForm::dict() const { return dict_->text().trimmed(); }

void PpocrImportForm::browse(QLineEdit *edit, const QString &filter) {
    const QString path = get_open_file_name(this, tr_text("Choose File"), edit->text(), filter).first;
    if (path.isEmpty()) return;
    edit->setText(path);
    if (edit == rec_ && name_->text().trimmed().isEmpty()) name_->setText(QFileInfo(path).completeBaseName());
}

void PpocrImportForm::update_ok() { ok_->setEnabled(!name().isEmpty() && !det().isEmpty() && !rec().isEmpty()); }
}
