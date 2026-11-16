#include "ui/settings_ui/file_name_dialog.h"
#include "core/file_names.h"
#include "core/i18n.h"
#include "core/save.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/labels.h"
#include <QFormLayout>
#include <QVBoxLayout>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScreen>
#include <algorithm>

namespace jietuba {
namespace {
QString text(const char *s) { return i18n_tr(s, "FileNameDialog"); }
// Rules are stored in Kind order; the original suffix is shown right below the screenshot rule it extends.
constexpr file_names::Kind DISPLAY_ORDER[] = {file_names::Screenshot, file_names::OriginalSuffix, file_names::Pin,
                                              file_names::LongScreenshot, file_names::Gif, file_names::Video};
class FileNamePreview : public CaptionLabel {
public:
    explicit FileNamePreview(QWidget *parent) : CaptionLabel(parent) {
        setWordWrap(false);
        setTextFormat(Qt::PlainText);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }
    void set_preview(const QString &value) {
        full_text_ = value;
        setToolTip(value);
        elide();
    }
protected:
    void resizeEvent(QResizeEvent *event) override {
        CaptionLabel::resizeEvent(event);
        elide();
    }
private:
    void elide() { setText(fontMetrics().elidedText(full_text_, Qt::ElideMiddle, contentsRect().width())); }
    QString full_text_;
};
}
FileNameDialog::FileNameDialog(const QStringList &patterns, QWidget *parent, const QString &image_format) : QDialog(parent) {
    image_extension_ = "." + SaveService::normalize_format(image_format).toLower();
    setObjectName("file_name_dialog");
    scale_dialog_font(*this);
    setWindowTitle(text("File Name Settings"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(dialog_scaled(20), dialog_scaled(16), dialog_scaled(20), dialog_scaled(16));
    root->setSpacing(dialog_scaled(12));
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName("filename_scroll");
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidgetResizable(true);
    auto *body = new QWidget(scroll);
    auto *body_layout = new QVBoxLayout(body);
    body_layout->setContentsMargins(0, 0, 0, 0);
    body_layout->setSpacing(dialog_scaled(12));
    body_layout->setSizeConstraint(QLayout::SetMinimumSize);
    scroll->setWidget(body);
    root->addWidget(scroll, 1);
    auto *hint = new CaptionLabel(text("Edit ordinary text directly, such as the name of a long screenshot. Use {time} for date and time, {ms} for milliseconds, and {direction} for long screenshot direction. Extensions are added automatically."), this);
    hint->setWordWrap(true);
    body_layout->addWidget(hint);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const char *labels[] = {"Screenshot", "Pinned Image", "Long Screenshot", "GIF", "MP4", "Original Suffix"};
    const auto defaults = file_names::defaults();
    edits_.resize(defaults.size());
    previews_.resize(defaults.size());
    field_names_.resize(defaults.size());
    for (const int i : DISPLAY_ORDER) {
        auto *field = new QWidget(this);
        auto *column = new QVBoxLayout(field);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(dialog_scaled(3));
        auto *edit = new LineEdit(field);
        edit->setObjectName(QString("filename_rule_%1").arg(i));
        const auto field_name = text(labels[i]);
        field_names_[i] = field_name;
        edit->setAccessibleName(field_name);
        edit->setText(patterns.value(i, defaults[i]));
        auto *preview = new FileNamePreview(field);
        preview->setObjectName(QString("filename_preview_%1").arg(i));
        edits_[i] = edit;
        previews_[i] = preview;
        column->addWidget(edit);
        column->addWidget(preview);
        form->addRow(field_name, field);
        if (auto *label = qobject_cast<QLabel *>(form->labelForField(field))) label->setBuddy(edit);
        connect(edit, &QLineEdit::textChanged, this, [this] { refresh(); });
    }
    body_layout->addLayout(form);
    error_ = new CaptionLabel(this);
    error_->setObjectName("filename_error");
    error_->setTextFormat(Qt::PlainText);
    error_->setWordWrap(true);
    error_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    error_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    root->addWidget(error_);
    body_layout->addStretch();
    auto *buttons = new QHBoxLayout;
    auto *reset = new TransparentPushButton(text("Restore defaults"), this);
    reset->setAutoDefault(false);
    connect(reset, &QPushButton::clicked, this, [this, defaults] { for (int i = 0; i < edits_.size(); ++i) edits_[i]->setText(defaults[i]); });
    ok_ = new PrimaryPushButton(text("OK"), this);
    ok_->setObjectName("filename_ok");
    ok_->setDefault(true);
    connect(ok_, &QPushButton::clicked, this, &QDialog::accept);
    auto *cancel = new PushButton(text("Cancel"), this);
    cancel->setAutoDefault(false);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(reset);
    buttons->addStretch();
    buttons->addWidget(ok_);
    buttons->addWidget(cancel);
    root->addLayout(buttons);
    configure_dialog_controls(*this);
    for (auto *preview : previews_) preview->setFixedHeight(preview->fontMetrics().height() + dialog_scaled(2));
    // Keep typing errors in the layout so incomplete tokens cannot move the fields.
    error_->setFixedHeight(error_->fontMetrics().height() * 3);
    const auto available = screen()->availableGeometry().size() - QSize(dialog_scaled(40), dialog_scaled(60));
    setMinimumWidth(std::min(dialog_scaled(480), available.width()));
    const auto height = body->sizeHint().height() + error_->height() + root->contentsMargins().top() + root->contentsMargins().bottom() + buttons->sizeHint().height() + root->spacing() * 2;
    resize(std::min(dialog_scaled(600), available.width()), std::min(height, available.height()));
    refresh();
}
QStringList FileNameDialog::patterns() const { QStringList result; for (auto *edit : edits_) result.append(edit->text()); return result; }
void FileNameDialog::accept() { if (ok_->isEnabled()) QDialog::accept(); }
void FileNameDialog::refresh() {
    QString error;
    const QStringList extensions{image_extension_, image_extension_, image_extension_, ".gif", ".mp4"};
    for (const int i : DISPLAY_ORDER) {
        const auto rule = edits_[i]->text();
        const auto problem = file_names::problem(rule, file_names::Kind(i));
        const auto description = problem.isEmpty() ? QString() : text(problem.toUtf8().constData());
        edits_[i]->setAccessibleDescription(description);
        edits_[i]->setToolTip(description);
        if (error.isEmpty() && !description.isEmpty()) error = field_names_[i] + ": " + description;
        auto preview = file_names::render(rule, i18n_tr("Long Screenshot", "ScrollCaptureWindow") + "_" + i18n_tr("Vertical", "ScrollCaptureWindow"), preview_time_);
        if (i == file_names::OriginalSuffix) preview = file_names::render(edits_[0]->text(), {}, preview_time_) + "_" + rule + image_extension_;
        else preview += extensions[i];
        static_cast<FileNamePreview *>(previews_[i])->set_preview(text("Preview: %1").arg(preview));
    }
    error_->setText(error);
    ok_->setEnabled(error.isEmpty());
}
}
