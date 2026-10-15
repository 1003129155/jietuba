#include "ui/settings_ui/color_format_dialog.h"
#include "core/i18n.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/labels.h"
#include "ui/fluent_lite/theme.h"
#include "ui/reorderable_rows.h"
#include <QApplication>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QScreen>
#include <QVBoxLayout>

namespace jietuba {
namespace {
QString tr_text(const char *text) { return i18n_tr(text, "ColorFormatDialog"); }

// Every format is rendered with this colour, so the user sees exactly what gets copied.
const QColor SAMPLE_COLOR(230, 153, 60);

// One row: drag grip, enable check, format name and a rendered sample.
class FormatRow : public DraggableRow {
public:
    explicit FormatRow(const color_formats::ColorFormat &fmt, QWidget *parent = nullptr) : DraggableRow(parent), format(fmt) {
        check = new QCheckBox(this);
        check->setChecked(fmt.enabled);
        auto *name_label = new BodyLabel(fmt.name, this);
        auto *sample_label = new CaptionLabel(this);
        sample_label->setWordWrap(false);
        auto *text_column = new QVBoxLayout;
        text_column->setContentsMargins(0, 0, 0, 0);
        text_column->setSpacing(dialog_scaled(2));
        text_column->addWidget(name_label);
        text_column->addWidget(sample_label);
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(dialog_scaled(8), dialog_scaled(6), dialog_scaled(8), dialog_scaled(6));
        layout->setSpacing(dialog_scaled(10));
        layout->addWidget(grip);
        layout->addWidget(check);
        layout->addLayout(text_column, 1);
        sample_label->setText(fmt.render(SAMPLE_COLOR));
    }
    color_formats::ColorFormat entry() const { return {format.name, format.template_string, check->isChecked()}; }
    color_formats::ColorFormat format;
    QCheckBox *check;
};
}

ColorFormatDialog::ColorFormatDialog(const QVector<color_formats::ColorFormat> &formats, QWidget *parent) : QDialog(parent) {
    scale_dialog_font(*this);
    setWindowTitle(tr_text("Manage Color Formats"));
    setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
    setStyleSheet(QString("QDialog { background: %1; }").arg(ui_tokens(this).window));

    list_ = new ReorderableRowList(this);
    auto *hint = new CaptionLabel(tr_text("The magnifier shows one checked format at a time — Shift cycles between them, C copies whichever is shown. "
                                          "Drag one to the top to make it the default."),
                                  this);
    hint->setWordWrap(true);
    configure_dialog_control(*hint);

    auto *reset_btn = new TransparentPushButton(tr_text("Restore defaults"), this);
    connect(reset_btn, &QPushButton::clicked, this, [this] { fill(color_formats::default_formats()); });
    auto *ok_btn = new PrimaryPushButton(tr_text("OK"), this);
    connect(ok_btn, &QPushButton::clicked, this, &QDialog::accept);
    auto *cancel_btn = new PushButton(tr_text("Cancel"), this);
    connect(cancel_btn, &QPushButton::clicked, this, &QDialog::reject);

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(dialog_scaled(8));
    buttons->addWidget(reset_btn);
    buttons->addStretch(1);
    buttons->addWidget(ok_btn);
    buttons->addWidget(cancel_btn);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(dialog_scaled(16), dialog_scaled(16), dialog_scaled(16), dialog_scaled(14));
    root->setSpacing(dialog_scaled(10));
    root->addWidget(list_, 1);
    root->addWidget(hint);
    root->addLayout(buttons);

    configure_dialog_controls(*this);
    fill(formats);
    fit_height_to_rows();
}

QVector<color_formats::ColorFormat> ColorFormatDialog::entries() const {
    QVector<color_formats::ColorFormat> result;
    for (auto *row : list_->rows()) result.append(static_cast<FormatRow *>(row)->entry());
    return color_formats::normalize(result);
}

void ColorFormatDialog::fill(const QVector<color_formats::ColorFormat> &formats) {
    list_->clear();
    for (const auto &fmt : color_formats::normalize(formats)) {
        auto *row = new FormatRow(fmt);
        list_->add_row(row);
        configure_dialog_controls(*row);
    }
}

void ColorFormatDialog::fit_height_to_rows() {
    int content_height = list_->content_height();
    list_->setFixedHeight(content_height);
    int desired_height = sizeHint().height();
    QScreen *screen = parentWidget() ? parentWidget()->screen() : QApplication::primaryScreen();
    if (screen) {
        const int max_height = std::max(dialog_scaled(360), screen->availableGeometry().height() - dialog_scaled(40));
        if (desired_height > max_height) {
            const int non_list_height = desired_height - content_height;
            content_height = std::max(dialog_scaled(180), max_height - non_list_height);
            list_->setFixedHeight(content_height);
            desired_height = non_list_height + content_height;
        }
    }
    resize(dialog_scaled(520), desired_height);
}
}
