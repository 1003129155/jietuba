#include "ui/settings_ui/file_name_dialog.h"
#include "core/file_names.h"
#include "core/i18n.h"
#include "core/qt_utils.h"
#include "core/save.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/labels.h"
#include "ui/fluent_lite/theme.h"
#include <QHBoxLayout>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace jietuba {
namespace {
QString text(const char *s) { return i18n_tr(s, "FileNameDialog"); }
// Rules are stored in Kind order; the original suffix is shown right below the screenshot rule it extends.
constexpr file_names::Kind DISPLAY_ORDER[] = {file_names::Screenshot, file_names::OriginalSuffix, file_names::Pin,
                                              file_names::LongScreenshot, file_names::Gif, file_names::Video};
constexpr int EDIT_WIDTH = 240;
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

// The problem in the hint's place: the same icon and size, in the danger colour.
QWidget *error_row(QWidget *parent, QLabel *&label) {
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(dialog_scaled(4), 0, 0, 0);
    layout->setSpacing(dialog_scaled(8));
    const auto &t = ui_tokens(row);
    auto *icon = new QLabel(row);
    const int side = dialog_scaled(14);
    icon->setFixedSize(dialog_scaled(16), dialog_scaled(16));
    icon->setAlignment(Qt::AlignCenter);
    icon->setPixmap(tinted_icon(FluentIcon::Info, t.danger).pixmap(QSize(side, side), row->devicePixelRatioF()));
    layout->addWidget(icon, 0, Qt::AlignTop);
    label = new QLabel(row);
    label->setObjectName("filename_error");
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    set_own_style(label, QString("color: %1; font: %2px %3; background: transparent;").arg(t.danger).arg(dialog_scaled(12)).arg(FONT_FAMILY));
    layout->addWidget(label, 1);
    return row;
}
}

FileNameDialog::FileNameDialog(const QStringList &patterns, QWidget *parent, const QString &image_format) : FluentDialog(parent) {
    image_extension_ = "." + SaveService::normalize_format(image_format).toLower();
    setObjectName("file_name_dialog");
    setWindowTitle(text("File Name Settings"));
    auto *scroll = new QScrollArea(body());
    scroll->setObjectName("filename_scroll");
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet("QScrollArea { background: transparent; border: none; } QScrollArea > QWidget > QWidget { background: transparent; }" +
                          scrollbar_qss(this));
    auto *cards = new QWidget(scroll);
    auto *cards_layout = new QVBoxLayout(cards);
    cards_layout->setContentsMargins(0, 0, 0, 0);
    cards_layout->setSpacing(dialog_scaled(4));
    cards_layout->setSizeConstraint(QLayout::SetMinimumSize);
    scroll->setWidget(cards);
    bodyLayout()->addWidget(scroll, 1);

    const char *labels[] = {"Screenshot", "Pinned Image", "Long Screenshot", "GIF", "MP4", "Original Suffix"};
    const auto defaults = file_names::defaults();
    edits_.resize(defaults.size());
    previews_.resize(defaults.size());
    field_names_.resize(defaults.size());
    for (const int i : DISPLAY_ORDER) {
        const auto field_name = text(labels[i]);
        field_names_[i] = field_name;
        auto *card = new DialogCard(field_name, cards);
        auto *preview = new FileNamePreview(card);
        preview->setObjectName(QString("filename_preview_%1").arg(i));
        card->setCaptionLabel(preview);
        auto *edit = new LineEdit(card);
        edit->setObjectName(QString("filename_rule_%1").arg(i));
        edit->setAccessibleName(field_name);
        edit->setText(patterns.value(i, defaults[i]));
        // A rule longer than the field shows its start.
        edit->setCursorPosition(0);
        edit->setFixedWidth(dialog_scaled(EDIT_WIDTH));
        card->addControl(edit);
        card->titleLabel->setBuddy(edit);
        edits_[i] = edit;
        previews_[i] = preview;
        cards_layout->addWidget(card);
        connect(edit, &QLineEdit::textChanged, this, [this] { refresh(); });
    }

    // The hint and a problem share one place of the larger height, so a problem cannot move the fields.
    notice_ = new QStackedWidget(body());
    notice_->addWidget(dialog_hint(notice_, text("Edit ordinary text directly, such as the name of a long screenshot. Use {time} for date and time, {ms} for milliseconds, and {direction} for long screenshot direction. Extensions are added automatically.")));
    notice_->addWidget(error_row(notice_, error_));
    bodyLayout()->addSpacing(dialog_scaled(8));
    bodyLayout()->addWidget(notice_);

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
    setFooterButtons({reset}, {ok_, cancel});
    finish_controls();
    for (auto *preview : previews_) preview->setFixedHeight(preview->fontMetrics().height() + dialog_scaled(2));

    const auto available = screen()->availableGeometry().size() - QSize(dialog_scaled(40), dialog_scaled(60));
    const int width = std::min(dialog_scaled(640), available.width());
    setMinimumWidth(std::min(dialog_scaled(520), available.width()));
    resize(width, std::min(height_showing(scroll, width), available.height()));
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
    notice_->setCurrentIndex(error.isEmpty() ? 0 : 1);
    ok_->setEnabled(error.isEmpty());
}
}
