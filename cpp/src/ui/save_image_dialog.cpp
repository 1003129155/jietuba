#include "ui/save_image_dialog.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "core/save.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "settings/tool_settings.h"
#include "ui/dialogs.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/segmented.h"
#include "ui/fluent_lite/theme.h"
#include "ui/fluent_lite/titlebar.h"
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGridLayout>
#include <QStyle>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QRegularExpression>
#include <QScreen>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QThreadPool>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <functional>
#include <cmath>

namespace jietuba {
namespace {
QString tr_text(const char *text) { return i18n_tr(text, "SaveImageDialog"); }

// (stored value, label); the stored value is SaveService's format name.
const std::vector<std::pair<QString, QString>> &FORMATS() {
    static const std::vector<std::pair<QString, QString>> formats{{"PNG", ".PNG"}, {"JPG", ".JPG"}, {"WEBP", ".WEBP"}, {"BMP", ".BMP"}, {"PDF", ".PDF"}};
    return formats;
}

QString human_size(qint64 bytes) {
    if (bytes < 1024) return QString("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 2);
    return QString("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 2);
}

// Notices and the folder mark; the theme has no warning colour of its own.
QString warning_color(const UIThemeTokens &t) { return t.is_dark() ? "#E2A447" : "#D08A1E"; }

// Pill with text drawn over the preview; anchored at its top-left or top-right corner.
void draw_pill(QPainter &painter, const QPointF &anchor, bool align_right, const QString &text) {
    QFont font = painter.font();
    font.setPixelSize(dialog_scaled(11));
    painter.setFont(font);
    const QFontMetricsF metrics(font);
    const double pad_h = dialog_scaled(8), pad_v = dialog_scaled(3);
    const QSizeF size(metrics.horizontalAdvance(text) + 2 * pad_h, metrics.height() + 2 * pad_v);
    const QRectF box(align_right ? anchor.x() - size.width() : anchor.x(), anchor.y(), size.width(), size.height());
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 150));
    painter.drawRoundedRect(box, size.height() / 2, size.height() / 2);
    painter.setPen(Qt::white);
    painter.drawText(box, Qt::AlignCenter, text);
}

// Icons the shared set lacks, drawn at the device pixel ratio of the screen.
QIcon drawn_icon(int size, const std::function<void(QPainter &, double)> &draw) {
    const double ratio = qApp->devicePixelRatio();
    QPixmap pixmap(QSize(size, size) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    draw(painter, size);
    painter.end();
    return QIcon(pixmap);
}

QIcon lock_icon(const QColor &color, bool locked) {
    return drawn_icon(dialog_scaled(16), [color, locked](QPainter &painter, double s) {
        QPen pen(color, s / 10.0);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        const QRectF body(s * 0.2, s * 0.45, s * 0.6, s * 0.45);
        painter.drawRoundedRect(body, s * 0.08, s * 0.08);
        // The shackle swings open to the right when unlocked.
        QPainterPath shackle;
        const double left = s * 0.32, right = s * 0.68, top = s * 0.12;
        shackle.moveTo(left, body.top());
        shackle.lineTo(left, s * 0.3);
        shackle.arcTo(QRectF(left, top, right - left, s * 0.36), 180, -180);
        if (locked) shackle.lineTo(right, body.top());
        else shackle.lineTo(right, s * 0.3);
        if (!locked) shackle.translate(s * 0.14, -s * 0.04);
        painter.drawPath(shackle);
    });
}

QIcon plus_icon(const QColor &color) {
    return drawn_icon(dialog_scaled(16), [color](QPainter &painter, double s) {
        painter.setPen(QPen(color, s / 9.0, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(s / 2, s * 0.2), QPointF(s / 2, s * 0.8));
        painter.drawLine(QPointF(s * 0.2, s / 2), QPointF(s * 0.8, s / 2));
    });
}

QString normalized_folder(const QString &path) { return QDir::cleanPath(QDir::fromNativeSeparators(path.trimmed())); }

int confirm(QWidget *parent, const QString &title, const QString &message) {
    const std::vector<DialogButton> buttons = {{0, tr_text("Cancel"), QDialogButtonBox::RejectRole}, {1, tr_text("OK"), QDialogButtonBox::AcceptRole, true}};
    return show_custom_confirm_dialog(parent, title, message, buttons).toInt();
}
}

bool use_modern_save_dialog(ToolSettingsManager *config) {
    if (!config) config = get_tool_settings_manager();
    return config->get_app_setting("save_dialog_style").toString() != "classic";
}

bool same_folder(const QString &a, const QString &b) { return normalized_folder(a).compare(normalized_folder(b), Qt::CaseInsensitive) == 0; }

std::vector<PathTag> load_path_tags(ToolSettingsManager *config) {
    std::vector<PathTag> tags;
    for (const auto &entry : config->get_app_setting("save_dialog_path_tags").toStringList()) {
        // Folder paths cannot hold '|', so the last one separates the name.
        const int bar = entry.lastIndexOf('|');
        if (bar > 0 && bar < entry.size() - 1) tags.push_back({entry.left(bar), entry.mid(bar + 1)});
    }
    if (tags.empty()) tags.push_back({tr_text("Desktop"), QDir::toNativeSeparators(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation))});
    return tags;
}

void save_path_tags(ToolSettingsManager *config, const std::vector<PathTag> &tags) {
    QStringList entries;
    for (const auto &tag : tags) entries << tag.name + "|" + tag.path;
    config->set_app_setting("save_dialog_path_tags", entries);
}

PathTagNameDialog::PathTagNameDialog(const QString &initial, QWidget *parent) : FrostedFramelessDialog(parent) {
    scale_dialog_font(*this);
    auto *title_bar = new FluentTitleBar(this);
    configure_dialog_control(*title_bar);
    setTitleBar(title_bar);
    title_bar->maxBtn->hide();
    title_bar->minBtn->hide();
    setWindowTitle(tr_text("Tag Name"));
    setResizeEnabled(false);
    name_edit = new LineEdit(this);
    name_edit->setText(initial);
    name_edit->selectAll();
    cancel_button = new PushButton(tr_text("Cancel"), this);
    save_button = new PrimaryPushButton(tr_text("Save"), this);
    save_button->setDefault(true);
    connect(cancel_button, &QPushButton::clicked, this, &QDialog::reject);
    connect(save_button, &QPushButton::clicked, this, [this] {
        if (!name_edit->text().trimmed().isEmpty()) accept();
    });
    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(dialog_scaled(12));
    buttons->addWidget(cancel_button, 1);
    buttons->addWidget(save_button, 1);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(dialog_scaled(16), title_bar->height() + dialog_scaled(8), dialog_scaled(16), dialog_scaled(16));
    root->setSpacing(dialog_scaled(16));
    root->addWidget(name_edit);
    root->addStretch(1);
    root->addLayout(buttons);
    configure_dialog_controls(*this);
    setFixedSize(dialog_scaled(360), dialog_scaled(190));
}

std::optional<QString> PathTagNameDialog::ask(QWidget *parent, const QString &initial) {
    PathTagNameDialog dialog(initial, parent);
    if (parent) dialog.move(parent->window()->geometry().center() - dialog.rect().center());
    if (exec_dialog(&dialog, parent) != QDialog::Accepted) return std::nullopt;
    return dialog.name_edit->text().trimmed();
}

PathTagChip::PathTagChip(const PathTag &tag, QWidget *parent) : QFrame(parent), tag(tag) {
    setObjectName("pathTag");
    setFixedHeight(dialog_scaled(26));
    setCursor(Qt::PointingHandCursor);
    setToolTip(QDir::toNativeSeparators(tag.path));
    const auto &t = ui_tokens(this);
    const int icon = dialog_scaled(16);
    auto *folder = new QLabel(this);
    folder->setPixmap(tinted_icon(FluentIcon::Folder, warning_color(t)).pixmap(icon, icon));
    auto *name = new QLabel(tag.name, this);
    name->setObjectName("pathTagText");
    edit_button = new QToolButton(this);
    close_button = new QToolButton(this);
    edit_button->setIcon(tinted_icon(FluentIcon::Edit, "#4A9AD0"));
    close_button->setIcon(tinted_icon(FluentIcon::Close, "#D34D4D"));
    edit_button->setToolTip(tr_text("Edit"));
    close_button->setToolTip(tr_text("Delete"));
    for (auto *button : {edit_button, close_button}) {
        button->setObjectName("pathTagAction");
        button->setFixedSize(icon, icon);
        button->setIconSize(QSize(icon - 2, icon - 2));
        button->setAutoRaise(true);
        button->hide();
    }
    connect(edit_button, &QToolButton::clicked, this, &PathTagChip::edit_requested);
    connect(close_button, &QToolButton::clicked, this, &PathTagChip::delete_requested);
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(dialog_scaled(8), 0, dialog_scaled(8), 0);
    layout->setSpacing(dialog_scaled(4));
    layout->addWidget(folder);
    layout->addWidget(name);
    layout->addWidget(edit_button);
    layout->addWidget(close_button);
}

void PathTagChip::set_current(bool current) {
    setProperty("current", current);
    style()->unpolish(this);
    style()->polish(this);
}

void PathTagChip::enterEvent(QEnterEvent *event) {
    QFrame::enterEvent(event);
    edit_button->show();
    close_button->show();
}

void PathTagChip::leaveEvent(QEvent *event) {
    QFrame::leaveEvent(event);
    edit_button->hide();
    close_button->hide();
}

void PathTagChip::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) emit clicked();
    QFrame::mouseReleaseEvent(event);
}

void PathTagChip::contextMenuEvent(QContextMenuEvent *event) {
    QMenu menu(this);
    auto *edit = menu.addAction(tr_text("Edit"));
    auto *remove = menu.addAction(tr_text("Delete"));
    configure_dialog_controls(menu);
    const auto *chosen = menu.exec(event->globalPos());
    if (chosen == edit) emit edit_requested();
    else if (chosen == remove) emit delete_requested();
}

PathTagBar::PathTagBar(ToolSettingsManager *config, int width, QWidget *parent)
    : QWidget(parent), config_(config), tags_(load_path_tags(config)), width_(width), rows_(new QVBoxLayout(this)) {
    rows_->setContentsMargins(0, 0, 0, 0);
    rows_->setSpacing(dialog_scaled(8));
    add_button = new QToolButton(this);
    add_button->setObjectName("pathTagAdd");
    add_button->setToolTip(tr_text("Add path tag"));
    add_button->setFixedSize(dialog_scaled(26), dialog_scaled(26));
    add_button->setIcon(plus_icon(css_color(ui_tokens(this).text_muted)));
    connect(add_button, &QToolButton::clicked, this, [this] { add_path(current_path_); });
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] {
        add_button->setIcon(plus_icon(css_color(ui_tokens(this).text_muted)));
        rebuild();
    });
    rebuild();
}

int PathTagBar::index_of_name(const QString &name) const {
    for (int i = 0; i < static_cast<int>(tags_.size()); ++i)
        if (tags_[i].name == name) return i;
    return -1;
}

int PathTagBar::index_of_path(const QString &path) const {
    for (int i = 0; i < static_cast<int>(tags_.size()); ++i)
        if (same_folder(tags_[i].path, path)) return i;
    return -1;
}

void PathTagBar::set_current_path(const QString &path) {
    current_path_ = path;
    for (auto *chip : chips) chip->set_current(same_folder(chip->tag.path, path));
}

void PathTagBar::add_path(const QString &path) {
    if (normalized_folder(path).isEmpty()) return;
    auto name = QFileInfo(normalized_folder(path)).fileName();
    if (name.isEmpty()) name = QDir::toNativeSeparators(normalized_folder(path));
    while (const auto chosen = PathTagNameDialog::ask(this, name)) {
        if (add_tag(*chosen, path) != AddResult::RETRY) return;
        name = *chosen;
    }
}

PathTagBar::AddResult PathTagBar::add_tag(const QString &name, const QString &path) {
    const int named = index_of_name(name);
    if (named >= 0) {
        if (same_folder(tags_[named].path, path)) {
            show_info_dialog(this, tr_text("Notice"), tr_text("This path already has this name"));
            return AddResult::CANCELLED;
        }
        const auto message = tr_text("This name is already used by another path \"%1\". Please choose a different name for the current path.")
                                 .arg(QDir::toNativeSeparators(tags_[named].path));
        return confirm(this, tr_text("Name Already Exists"), message) == 1 ? AddResult::RETRY : AddResult::CANCELLED;
    }
    const int existing = index_of_path(path);
    if (existing >= 0) {
        const auto message = tr_text("This path already exists with the name \"%1\". Do you want to change its name to \"%2\"?").arg(tags_[existing].name, name);
        if (confirm(this, tr_text("Path Already Exists"), message) != 1) return AddResult::CANCELLED;
        tags_[existing].name = name;
    } else {
        tags_.push_back({name, QDir::toNativeSeparators(normalized_folder(path))});
    }
    persist();
    return AddResult::DONE;
}

void PathTagBar::rename_tag(int index) {
    if (index < 0 || index >= static_cast<int>(tags_.size())) return;
    QString name = tags_[index].name;
    while (const auto chosen = PathTagNameDialog::ask(this, name)) {
        const int taken = index_of_name(*chosen);
        if (taken < 0 || taken == index) {
            tags_[index].name = *chosen;
            persist();
            return;
        }
        const auto message = tr_text("The name \"%1\" is already used by path \"%2\". Do you want to set a new name for the current path?")
                                 .arg(*chosen, QDir::toNativeSeparators(tags_[taken].path));
        if (confirm(this, tr_text("Name Already Exists"), message) != 1) return;
        name = *chosen;
    }
}

void PathTagBar::remove_tag(int index) {
    if (index < 0 || index >= static_cast<int>(tags_.size())) return;
    tags_.erase(tags_.begin() + index);
    persist();
}

void PathTagBar::persist() {
    save_path_tags(config_, tags_);
    // Deleting the last tag brings back the Desktop one, as on the next open.
    if (tags_.empty()) tags_ = load_path_tags(config_);
    rebuild();
}

// Rows are filled up to the bar's width, counting each chip with its hover buttons
// shown so that hovering never pushes a chip past the edge.
void PathTagBar::rebuild() {
    for (auto *chip : chips) chip->deleteLater();
    chips.clear();
    while (auto *item = rows_->takeAt(0)) {
        if (auto *row = item->layout()) {
            while (auto *child = row->takeAt(0)) delete child;
        }
        delete item;
    }
    const int gap = dialog_scaled(8);
    QHBoxLayout *row = nullptr;
    int used = 0;
    const auto place = [&](QWidget *widget, int width) {
        if (!row || (used > 0 && used + gap + width > width_)) {
            if (row) row->addStretch(1);
            row = new QHBoxLayout;
            row->setSpacing(gap);
            rows_->addLayout(row);
            used = 0;
        }
        row->addWidget(widget);
        used += (used > 0 ? gap : 0) + width;
    };
    for (int index = 0; index < static_cast<int>(tags_.size()); ++index) {
        auto *chip = new PathTagChip(tags_[index], this);
        connect(chip, &PathTagChip::clicked, this, [this, index] { emit path_chosen(tags_[index].path); });
        connect(chip, &PathTagChip::edit_requested, this, [this, index] { QTimer::singleShot(0, this, [this, index] { rename_tag(index); }); });
        connect(chip, &PathTagChip::delete_requested, this, [this, index] { QTimer::singleShot(0, this, [this, index] { remove_tag(index); }); });
        chips.push_back(chip);
        const int extra = 2 * (chip->edit_button->width() + chip->layout()->spacing());
        place(chip, chip->sizeHint().width() + extra);
    }
    place(add_button, add_button->width());
    row->addStretch(1);
    set_current_path(current_path_);
}

SavePreviewView::SavePreviewView(QWidget *parent) : QWidget(parent) {
    setMouseTracking(true);
    setMinimumSize(dialog_scaled(320), dialog_scaled(240));
    const int cell = dialog_scaled(8);
    checker_ = QPixmap(2 * cell, 2 * cell);
    checker_.fill(QColor(255, 255, 255));
    QPainter painter(&checker_);
    painter.fillRect(0, 0, cell, cell, QColor(224, 224, 224));
    painter.fillRect(cell, cell, cell, cell, QColor(224, 224, 224));
}

void SavePreviewView::set_image(const QImage &image) {
    const bool resized = image.size() != image_.size();
    image_ = image;
    if (resized) encoded_ = QImage();
    if (resized || fitted_) fit();
    else update();
}

void SavePreviewView::set_original(const QImage &original) {
    original_ = original;
    update();
}

void SavePreviewView::set_encoded(const QImage &encoded) {
    encoded_ = encoded.size() == image_.size() ? encoded : QImage();
    update();
}

void SavePreviewView::set_mode(Mode mode) {
    mode_ = mode;
    update_cursor(mapFromGlobal(QCursor::pos()));
    update();
}

void SavePreviewView::set_loading(bool loading) {
    if (loading_ == loading) return;
    loading_ = loading;
    update();
}

void SavePreviewView::set_top_inset(int inset) {
    top_inset_ = inset;
    if (fitted_) fit();
    else clamp_offset();
}

QRectF SavePreviewView::fit_area() const {
    const double margin = dialog_scaled(12);
    return QRectF(rect()).adjusted(margin, top_inset_ + margin, -margin, -margin);
}

void SavePreviewView::fit() {
    double zoom = 1.0;
    if (!image_.isNull()) {
        const auto area = fit_area();
        zoom = std::min({area.width() / image_.width(), area.height() / image_.height(), 1.0});
        zoom = std::max(zoom, MIN_ZOOM);
    }
    const bool changed = zoom != zoom_;
    zoom_ = zoom;
    fitted_ = true;
    clamp_offset();
    if (changed) emit zoom_changed(zoom_);
    update();
}

void SavePreviewView::set_zoom(double zoom, const QPointF &anchor) {
    zoom = std::clamp(zoom, MIN_ZOOM, MAX_ZOOM);
    if (image_.isNull() || zoom == zoom_) return;
    const QPointF image_point = (anchor - offset_) / zoom_;
    zoom_ = zoom;
    offset_ = anchor - image_point * zoom_;
    fitted_ = false;
    clamp_offset();
    emit zoom_changed(zoom_);
    update();
}

QRectF SavePreviewView::image_rect() const { return QRectF(offset_, QSizeF(image_.size()) * zoom_); }

// Along an axis where the image fits the free area it is centred there; otherwise it
// keeps covering the view.
void SavePreviewView::clamp_offset() {
    const QSizeF size = QSizeF(image_.size()) * zoom_;
    const auto area = fit_area();
    const auto axis = [](double offset, double extent, double start, double span, double view) {
        return extent <= span ? start + (span - extent) / 2 : std::clamp(offset, view - extent, 0.0);
    };
    offset_ = QPointF(axis(offset_.x(), size.width(), area.left(), area.width(), width()),
                      axis(offset_.y(), size.height(), area.top(), area.height(), height()));
}

double SavePreviewView::divider_x() const {
    const auto rect = image_rect();
    return std::clamp(rect.left() + split_ * rect.width(), 0.0, static_cast<double>(width()));
}

bool SavePreviewView::near_divider(const QPointF &pos) const {
    return splitting() && std::abs(pos.x() - divider_x()) <= dialog_scaled(6) && pos.y() >= image_rect().top() && pos.y() <= image_rect().bottom();
}

void SavePreviewView::update_cursor(const QPointF &pos) {
    const QSizeF size = QSizeF(image_.size()) * zoom_;
    const bool pannable = size.width() > width() || size.height() > height();
    if (near_divider(pos)) setCursor(Qt::SplitHCursor);
    else if (pannable) setCursor(drag_ == PAN ? Qt::ClosedHandCursor : Qt::OpenHandCursor);
    else unsetCursor();
}

void SavePreviewView::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    const auto &t = ui_tokens(this);
    QPainterPath frame;
    const double radius = dialog_scaled(12);
    frame.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillPath(frame, css_color(t.window_top));
    painter.setClipPath(frame);
    // A dot grid tells the backdrop apart from white or transparent image content.
    const int step = dialog_scaled(14);
    auto dot = css_color(t.border_hover);
    dot.setAlphaF(dot.alphaF() * 0.6);
    painter.setPen(Qt::NoPen);
    painter.setBrush(dot);
    for (int y = step / 2; y < height(); y += step)
        for (int x = step / 2; x < width(); x += step) painter.drawEllipse(QPointF(x, y), 0.9, 0.9);
    if (!image_.isNull()) {
        const auto target = image_rect();
        painter.fillRect(target, QBrush(checker_));
        // Smooth when shrunk; at 100% and above every pixel stays a sharp block, which is
        // where compression artefacts show. The source is resampled whenever the result
        // has another size.
        const auto &source = original_source();
        const auto &result = this->result();
        const auto draw_source = [&] {
            painter.setRenderHint(QPainter::SmoothPixmapTransform, zoom_ < 1.0 || source.size() != result.size());
            painter.drawImage(target, source);
        };
        painter.setRenderHint(QPainter::SmoothPixmapTransform, zoom_ < 1.0);
        if (splitting()) {
            const double x = divider_x();
            painter.save();
            painter.setClipRect(QRectF(0, 0, x, height()), Qt::IntersectClip);
            draw_source();
            painter.restore();
            painter.save();
            painter.setClipRect(QRectF(x, 0, width() - x, height()), Qt::IntersectClip);
            painter.drawImage(target, result);
            painter.restore();
            const double top = std::max(target.top(), 0.0), bottom = std::min(target.bottom(), static_cast<double>(height()));
            // White with a dark edge, so the line shows on light and dark images alike.
            painter.setPen(QPen(QColor(0, 0, 0, 110), dialog_scaled(2) + 2));
            painter.drawLine(QPointF(x, top), QPointF(x, bottom));
            painter.setPen(QPen(Qt::white, dialog_scaled(2)));
            painter.drawLine(QPointF(x, top), QPointF(x, bottom));
            const QPointF knob(x, (top + bottom) / 2);
            painter.setPen(QPen(QColor(0, 0, 0, 60), 1));
            painter.setBrush(Qt::white);
            painter.drawEllipse(knob, dialog_scaled(9), dialog_scaled(9));
            painter.setPen(QPen(QColor(60, 60, 60), dialog_scaled(1.5)));
            const double a = dialog_scaled(3);
            painter.drawLine(QPointF(knob.x() - a, knob.y() - a), QPointF(knob.x() - 2 * a, knob.y()));
            painter.drawLine(QPointF(knob.x() - 2 * a, knob.y()), QPointF(knob.x() - a, knob.y() + a));
            painter.drawLine(QPointF(knob.x() + a, knob.y() - a), QPointF(knob.x() + 2 * a, knob.y()));
            painter.drawLine(QPointF(knob.x() + 2 * a, knob.y()), QPointF(knob.x() + a, knob.y() + a));
            const double label_top = std::max(target.top(), static_cast<double>(top_inset_)) + dialog_scaled(8);
            draw_pill(painter, QPointF(x - dialog_scaled(8), label_top), true, tr_text("Original Image"));
            draw_pill(painter, QPointF(x + dialog_scaled(8), label_top), false, tr_text("Adjusted"));
        } else {
            if (mode_ == ORIGINAL) draw_source();
            else painter.drawImage(target, result);
        }
        if (loading_) draw_pill(painter, QPointF(width() - dialog_scaled(10), height() - dialog_scaled(30)), true, tr_text("Estimating..."));
    }
    painter.setClipping(false);
    painter.setPen(QPen(css_color(t.border), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(frame);
}

void SavePreviewView::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    if (fitted_) fit();
    else clamp_offset();
}

void SavePreviewView::wheelEvent(QWheelEvent *event) {
    const double steps = event->angleDelta().y() / 120.0;
    if (steps != 0) set_zoom(zoom_ * std::pow(1.25, steps), event->position());
    event->accept();
}

void SavePreviewView::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) return QWidget::mousePressEvent(event);
    drag_ = near_divider(event->position()) ? DIVIDER : PAN;
    drag_origin_ = event->position();
    drag_offset_ = offset_;
    update_cursor(event->position());
}

void SavePreviewView::mouseMoveEvent(QMouseEvent *event) {
    const auto pos = event->position();
    if (drag_ == DIVIDER) {
        const auto rect = image_rect();
        if (rect.width() > 0) split_ = std::clamp((pos.x() - rect.left()) / rect.width(), 0.0, 1.0);
        update();
    } else if (drag_ == PAN) {
        offset_ = drag_offset_ + (pos - drag_origin_);
        fitted_ = false;
        clamp_offset();
        update();
    } else {
        update_cursor(pos);
    }
}

void SavePreviewView::mouseReleaseEvent(QMouseEvent *event) {
    drag_ = NONE;
    update_cursor(event->position());
}

void SavePreviewView::mouseDoubleClickEvent(QMouseEvent *) { fit(); }

SaveImageDialog::SaveImageDialog(const QImage &image, const QString &base_name, ToolSettingsManager *config, QWidget *parent)
    : FrostedFramelessDialog(parent), image_(image), config_(config ? config : get_tool_settings_manager()), size_(image.size()) {
    scale_dialog_font(*this);
    auto *title_bar = new FluentTitleBar(this);
    configure_dialog_control(*title_bar);
    setTitleBar(title_bar);
    title_bar->maxBtn->hide();
    setWindowTitle(tr_text("Save Image"));
    const int panel_width = dialog_scaled(PANEL_WIDTH);

    // ── Preview, with the view mode floating at its top ──
    preview = new SavePreviewView(this);
    view_mode = new SegmentedWidget(preview);
    view_mode->setLarge(true);
    view_mode->addItem("encoded", tr_text("Adjusted"));
    view_mode->addItem("original", tr_text("Original Image"));
    view_mode->addItem("split", tr_text("Compare"));
    // Starts on the adjusted result, so size and quality changes show without switching.
    view_mode->setCurrentItem("encoded");
    preview->set_mode(SavePreviewView::ENCODED);
    view_mode->adjustSize();
    preview->set_top_inset(view_mode->height() + dialog_scaled(12));
    preview->installEventFilter(this);
    connect(view_mode, &SegmentedWidget::currentItemChanged, this, [this](const QString &key) {
        preview->set_mode(key == "split" ? SavePreviewView::SPLIT : key == "encoded" ? SavePreviewView::ENCODED : SavePreviewView::ORIGINAL);
    });

    // ── Settings panel ──
    panel_ = new QWidget(this);
    panel_->setObjectName("savePanel");
    panel_->setFixedWidth(panel_width);
    auto *panel = new QVBoxLayout(panel_);
    panel->setContentsMargins(0, 0, 0, dialog_scaled(24));
    panel->setSpacing(dialog_scaled(12));
    const auto block = [this, panel](const char *title, QWidget **out = nullptr) {
        auto *widget = new QWidget(panel_);
        auto *layout = new QVBoxLayout(widget);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(dialog_scaled(12));
        if (title) {
            auto *label = new QLabel(tr_text(title), widget);
            label->setObjectName("blockTitle");
            layout->addWidget(label);
        }
        panel->addWidget(widget);
        if (out) *out = widget;
        return layout;
    };

    auto *path_block = block("Save Path");
    auto *directory_row = new QHBoxLayout;
    directory_row->setSpacing(dialog_scaled(8));
    directory_edit = new LineEdit(panel_, false);
    directory_edit->setObjectName("field");
    QString directory = config_->get_app_setting("save_dialog_last_dir").toString();
    if (directory.isEmpty() || !ToolSettingsManager::is_absolute_save_path(directory) || !QFileInfo(directory).isDir())
        directory = config_->get_screenshot_save_path();
    directory_edit->setText(QDir::toNativeSeparators(directory));
    directory_edit->setCursorPosition(0);
    change_button = new PushButton(tr_text("Change"), panel_);
    change_button->setFixedWidth(dialog_scaled(78));
    connect(change_button, &QPushButton::clicked, this, &SaveImageDialog::choose_directory);
    directory_row->addWidget(directory_edit, 1);
    directory_row->addWidget(change_button);
    path_block->addLayout(directory_row);
    tag_bar = new PathTagBar(config_, panel_width, panel_);
    connect(tag_bar, &PathTagBar::path_chosen, this, [this](const QString &path) {
        directory_edit->setText(QDir::toNativeSeparators(path));
        directory_edit->setCursorPosition(0);
    });
    path_block->addWidget(tag_bar);

    auto *name_block = block("File Name");
    auto *name_row = new QHBoxLayout;
    name_row->setSpacing(dialog_scaled(8));
    name_edit = new LineEdit(panel_, false);
    name_edit->setObjectName("field");
    name_edit->setText(base_name);
    format_combo = new ComboBox(panel_);
    format_combo->setFixedWidth(dialog_scaled(94));
    for (const auto &[value, label] : FORMATS()) format_combo->addItem(label, value);
    QString format = config_->get_app_setting("save_dialog_format").toString();
    if (format.isEmpty()) format = config_->get_screenshot_format();
    format_combo->setCurrentIndex(std::max(0, format_combo->findData(SaveService::normalize_format(format))));
    name_row->addWidget(name_edit, 1);
    name_row->addWidget(format_combo);
    name_block->addLayout(name_row);
    name_hint = new QLabel(panel_);
    name_hint->setObjectName("warningHint");
    name_hint->setWordWrap(true);
    name_hint->hide();
    name_block->addWidget(name_hint);
    connect(name_edit, &QLineEdit::textChanged, this, &SaveImageDialog::update_save_enabled);
    connect(directory_edit, &QLineEdit::textChanged, this, [this] {
        tag_bar->set_current_path(directory_edit->text());
        update_save_enabled();
    });
    connect(format_combo, &QComboBox::currentIndexChanged, this, &SaveImageDialog::on_format_changed);

    auto *size_block = block(nullptr);
    auto *size_header = new QHBoxLayout;
    auto *size_title = new QLabel(tr_text("Resize"), panel_);
    size_title->setObjectName("blockTitle");
    size_mode = new SegmentedWidget(panel_);
    size_mode->addItem("value", tr_text("Value"));
    size_mode->addItem("percent", tr_text("Percentage"));
    size_mode->setCurrentItem("value");
    connect(size_mode, &SegmentedWidget::currentItemChanged, this, [this] { show_size(); });
    size_header->addWidget(size_title);
    size_header->addStretch(1);
    size_header->addWidget(size_mode);
    size_block->addLayout(size_header);
    auto *size_row = new QHBoxLayout;
    size_row->setSpacing(dialog_scaled(8));
    const auto size_input = [this](QSpinBox *&spin, QLabel *&suffix, const char *letter) {
        auto *frame = new QFrame(panel_);
        frame->setObjectName("sizeInput");
        frame->setFixedHeight(dialog_scaled(36));
        auto *layout = new QHBoxLayout(frame);
        layout->setContentsMargins(0, 0, dialog_scaled(12), 0);
        layout->setSpacing(0);
        spin = new QSpinBox(frame);
        spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        spin->setFrame(false);
        suffix = new QLabel(letter, frame);
        suffix->setObjectName("sizeSuffix");
        layout->addWidget(spin, 1);
        layout->addWidget(suffix);
        const bool is_width = letter[0] == 'W';
        connect(spin, &QSpinBox::valueChanged, this, [this, is_width] {
            if (!updating_size_) on_size_typed(is_width);
        });
        return frame;
    };
    auto *width_frame = size_input(width_spin, width_suffix, "W");
    auto *height_frame = size_input(height_spin, height_suffix, "H");
    lock_button = new QToolButton(panel_);
    lock_button->setObjectName("sizeLock");
    lock_button->setCheckable(true);
    lock_button->setAutoRaise(true);
    lock_button->setFixedSize(dialog_scaled(16), dialog_scaled(16));
    lock_button->setIconSize(QSize(dialog_scaled(16), dialog_scaled(16)));
    lock_button->setToolTip(tr_text("Lock aspect ratio"));
    lock_button->setChecked(config_->get_app_setting("save_dialog_aspect_locked").toBool());
    connect(lock_button, &QToolButton::toggled, this, [this](bool locked) {
        config_->set_app_setting("save_dialog_aspect_locked", locked);
        apply_theme();
        if (locked) on_size_typed(true);
    });
    reset_size_button = new QToolButton(panel_);
    reset_size_button->setObjectName("outlineIcon");
    reset_size_button->setFixedSize(dialog_scaled(34), dialog_scaled(34));
    reset_size_button->setToolTip(tr_text("Original size"));
    connect(reset_size_button, &QToolButton::clicked, this, [this] { set_target_size(image_.size()); });
    size_row->addWidget(width_frame, 1);
    size_row->addWidget(lock_button);
    size_row->addWidget(height_frame, 1);
    size_row->addWidget(reset_size_button);
    size_block->addLayout(size_row);
    lossless_hint = new QLabel(panel_);
    lossless_hint->setObjectName("hint");
    lossless_hint->setWordWrap(true);
    size_block->addWidget(lossless_hint);

    auto *quality = block("Quality", &quality_block);
    auto *quality_row = new QHBoxLayout;
    quality_row->setSpacing(dialog_scaled(8));
    quality_slider = new QSlider(Qt::Horizontal, panel_);
    quality_slider->setRange(1, 100);
    quality_slider->setValue(std::clamp(static_cast<int>(config_->get_screenshot_quality()), 1, 100));
    quality_value = new QLabel(panel_);
    quality_value->setObjectName("hint");
    quality_value->setMinimumWidth(QFontMetrics(quality_value->font()).horizontalAdvance("100") + dialog_scaled(6));
    quality_value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    quality_row->addWidget(quality_slider, 1);
    quality_row->addWidget(quality_value);
    quality->addLayout(quality_row);
    auto *quality_ends = new QHBoxLayout;
    for (const char *text : {"Low", "High"}) {
        auto *label = new QLabel(tr_text(text), panel_);
        label->setObjectName("hint");
        if (quality_ends->count()) quality_ends->addStretch(1);
        quality_ends->addWidget(label);
    }
    quality_ends->addSpacing(quality_value->minimumWidth() + dialog_scaled(8));
    quality->setSpacing(dialog_scaled(6));
    quality->addLayout(quality_ends);
    connect(quality_slider, &QSlider::valueChanged, this, [this](int value) {
        quality_value->setText(QString::number(value));
        schedule_encode();
    });
    quality_value->setText(QString::number(quality_slider->value()));

    auto *size_estimate = block(nullptr);
    size_estimate->setSpacing(dialog_scaled(4));
    auto *estimate_title = new QLabel(tr_text("Estimated Size"), panel_);
    estimate_title->setObjectName("hint");
    file_size_label = new QLabel("--", panel_);
    file_size_label->setObjectName("sizeValue");
    size_estimate->addWidget(estimate_title);
    size_estimate->addWidget(file_size_label);

    save_button = new PrimaryPushButton(tr_text("Save"), panel_);
    save_button->setDefault(true);
    save_button->setAutoDefault(true);
    connect(save_button, &QPushButton::clicked, this, &SaveImageDialog::accept_save);
    panel->addWidget(save_button);
    panel->addStretch(1);

    // ── Status bar and the switch to the classic dialog ──
    auto *status = new QHBoxLayout;
    status->setSpacing(dialog_scaled(16));
    zoom_label = new QLabel(this);
    zoom_label->setObjectName("hint");
    original_label = new QLabel(tr_text("Original: %1 x %2").arg(image_.width()).arg(image_.height()), this);
    original_label->setObjectName("hint");
    status->addWidget(zoom_label);
    status->addWidget(original_label);
    status->addStretch(1);
    classic_link = new QLabel(this);
    classic_link->setTextFormat(Qt::RichText);
    classic_link->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    connect(classic_link, &QLabel::linkActivated, this, &SaveImageDialog::switch_to_classic);
    connect(preview, &SavePreviewView::zoom_changed, this, &SaveImageDialog::update_status);

    auto *grid = new QGridLayout(this);
    grid->setContentsMargins(dialog_scaled(16), title_bar->height() + dialog_scaled(4), dialog_scaled(16), dialog_scaled(10));
    grid->setHorizontalSpacing(dialog_scaled(16));
    grid->setVerticalSpacing(dialog_scaled(8));
    grid->addWidget(preview, 0, 0);
    grid->addWidget(panel_, 0, 1);
    grid->addLayout(status, 1, 0);
    grid->addWidget(classic_link, 1, 1, Qt::AlignRight);
    grid->setColumnStretch(0, 1);
    grid->setRowStretch(0, 1);
    configure_dialog_controls(*this);

    encode_timer_.setSingleShot(true);
    connect(&encode_timer_, &QTimer::timeout, this, &SaveImageDialog::start_encode);

    preview->set_original(image_);
    preview->set_image(image_);
    show_size();
    on_format_changed();
    tag_bar->set_current_path(directory_edit->text());
    apply_theme();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { apply_theme(); });
    place(parent);
    name_edit->setFocus();
    name_edit->selectAll();
    // The first encode needs no debounce.
    encode_timer_.start(0);
}

std::pair<SaveImageDialog::Outcome, QString> SaveImageDialog::run(QWidget *parent, const QImage &image, const QString &base_name, ToolSettingsManager *config) {
    if (image.isNull()) {
        show_warning_dialog(parent, tr_text("Save Failed"), tr_text("There is no image to save."));
        return {Outcome::CANCELLED, {}};
    }
    SaveImageDialog dialog(image, base_name, config, parent);
    // With no window to belong to it would open behind whatever the user is looking at.
    if (!parent) QTimer::singleShot(0, &dialog, [&dialog] {
            dialog.raise();
            dialog.activateWindow();
        });
    if (exec_dialog(&dialog, parent) == QDialog::Accepted) return {Outcome::SAVED, dialog.saved_path()};
    return {dialog.switched_to_classic() ? Outcome::CLASSIC : Outcome::CANCELLED, {}};
}

QString SaveImageDialog::file_name_problem(const QString &name) {
    if (name.trimmed().isEmpty()) return tr_text("Enter a file name.");
    static const QRegularExpression invalid(R"([\\/:*?"<>|\x00-\x1F])");
    if (name.contains(invalid)) return tr_text("A file name cannot contain any of these characters: \\ / : * ? \" < > |");
    if (name.endsWith('.') || name.endsWith(' ')) return tr_text("A file name cannot end with a dot or a space.");
    static const QRegularExpression reserved(R"(^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\..*)?$)", QRegularExpression::CaseInsensitiveOption);
    if (reserved.match(name.trimmed()).hasMatch()) return tr_text("This name is reserved by Windows.");
    return {};
}

QString SaveImageDialog::current_format() const { return format_combo->currentData().toString(); }

int SaveImageDialog::current_quality() const { return quality_slider->value(); }

QSize SaveImageDialog::target_size() const { return size_; }

bool SaveImageDialog::percent_mode() const { return size_mode->currentItem() == "percent"; }

QString SaveImageDialog::target_path() const {
    const auto extension = "." + current_format().toLower();
    auto name = name_edit->text().trimmed();
    // A name typed with the same extension is not given a second one.
    if (name.endsWith(extension, Qt::CaseInsensitive)) name.chop(extension.size());
    return QDir(QDir::fromNativeSeparators(directory_edit->text().trimmed())).filePath(name + extension);
}

void SaveImageDialog::on_format_changed() {
    update_quality_controls();
    schedule_encode();
}

void SaveImageDialog::update_quality_controls() {
    const auto format = current_format();
    const bool has_quality = SaveService::format_has_quality(format);
    quality_block->setVisible(has_quality);
    lossless_hint->setVisible(!has_quality);
    if (!has_quality)
        lossless_hint->setText(format == "PDF" ? tr_text("PDF has no quality setting.")
                                               : tr_text("%1 format is lossless and does not support quality adjustment.").arg(format));
    update_view_mode();
}

// A lossless save at the original size is the original; nothing to switch between.
void SaveImageDialog::update_view_mode() {
    const bool differs = SaveService::format_has_quality(current_format()) || size_ != image_.size();
    view_mode->setEnabled(differs);
    if (!differs) view_mode->setCurrentItem("encoded");
}

void SaveImageDialog::on_size_typed(bool width_changed) {
    const QSize original = image_.size();
    const bool locked = lock_button->isChecked();
    QSize size;
    if (percent_mode()) {
        int width_percent = width_spin->value(), height_percent = height_spin->value();
        if (locked) (width_changed ? height_percent : width_percent) = width_changed ? width_percent : height_percent;
        size = QSize(std::max(1, static_cast<int>(std::lround(original.width() * width_percent / 100.0))),
                     std::max(1, static_cast<int>(std::lround(original.height() * height_percent / 100.0))));
    } else {
        int width = width_spin->value(), height = height_spin->value();
        if (locked && width_changed) height = std::max(1, static_cast<int>(std::lround(double(width) * original.height() / original.width())));
        if (locked && !width_changed) width = std::max(1, static_cast<int>(std::lround(double(height) * original.width() / original.height())));
        size = QSize(width, height);
    }
    set_target_size(size);
}

void SaveImageDialog::set_target_size(const QSize &size) {
    const QSize limit = image_.size() * MAX_SCALE_PERCENT / 100;
    const QSize clamped(std::clamp(size.width(), 1, std::max(1, limit.width())), std::clamp(size.height(), 1, std::max(1, limit.height())));
    const bool changed = clamped != size_;
    size_ = clamped;
    show_size();
    if (changed) schedule_encode();
}

// The inputs show the target size in pixels or as percentages of the original.
void SaveImageDialog::show_size() {
    updating_size_ = true;
    const QSize original = image_.size();
    if (percent_mode()) {
        for (auto *spin : {width_spin, height_spin}) spin->setRange(1, MAX_SCALE_PERCENT);
        width_spin->setValue(static_cast<int>(std::lround(100.0 * size_.width() / original.width())));
        height_spin->setValue(static_cast<int>(std::lround(100.0 * size_.height() / original.height())));
        width_suffix->setText("W %");
        height_suffix->setText("H %");
    } else {
        width_spin->setRange(1, std::max(1, original.width() * MAX_SCALE_PERCENT / 100));
        height_spin->setRange(1, std::max(1, original.height() * MAX_SCALE_PERCENT / 100));
        width_spin->setValue(size_.width());
        height_spin->setValue(size_.height());
        width_suffix->setText("W");
        height_suffix->setText("H");
    }
    updating_size_ = false;
    reset_size_button->setEnabled(size_ != original);
    update_view_mode();
}

void SaveImageDialog::schedule_encode() {
    ++generation_;
    encoded_bytes_ = -1;
    // Started first: a pending encode is not a failed one.
    encode_timer_.start(ENCODE_DELAY_MS);
    preview->set_loading(true);
    update_status();
    update_save_enabled();
}

SaveImageDialog::Encoded SaveImageDialog::encode(const QImage &image, const QSize &size, const QString &format, int quality) {
    Encoded result;
    result.scaled = SaveService::scale_for_save(image, size);
    const auto bytes = SaveService::encode_image(result.scaled, format, quality);
    if (bytes.isEmpty()) return result;
    result.bytes = bytes.size();
    if (SaveService::format_has_quality(format)) result.decoded = QImage::fromData(bytes, format.toLatin1().constData());
    return result;
}

// One encode at a time: a change while one runs is picked up when it finishes, and
// only the result of the latest settings is shown.
void SaveImageDialog::start_encode() {
    if (encoding_) {
        encode_pending_ = true;
        return;
    }
    encoding_ = true;
    const int generation = generation_;
    QPointer<SaveImageDialog> guard(this);
    QThreadPool::globalInstance()->start([guard, generation, image = image_, size = target_size(), format = current_format(), quality = current_quality()] {
        Encoded result;
        try {
            result = encode(image, size, format, quality);
        } catch (const std::exception &error) {
            log_warning(T("保存预览编码失败: {e}", {{"e", QString::fromLocal8Bit(error.what())}}), "SaveImageDialog");
        }
        QMetaObject::invokeMethod(qApp, [guard, generation, result] {
            if (guard) guard->on_encoded(generation, result);
        }, Qt::QueuedConnection);
    });
}

void SaveImageDialog::on_encoded(int generation, const Encoded &result) {
    encoding_ = false;
    if (generation == generation_) {
        preview->set_image(result.scaled.isNull() ? image_ : result.scaled);
        preview->set_encoded(result.decoded);
        preview->set_loading(false);
        encoded_bytes_ = result.bytes;
        update_status();
        update_save_enabled();
        emit encode_finished(result.bytes);
    }
    if (encode_pending_) {
        encode_pending_ = false;
        start_encode();
    }
}

void SaveImageDialog::update_status() {
    zoom_label->setText(tr_text("Zoom: %1%").arg(std::lround(preview->zoom() * 100)));
    if (encoded_bytes_ >= 0) file_size_label->setText(human_size(encoded_bytes_));
    else if (encode_timer_.isActive() || encoding_) file_size_label->setText(tr_text("Estimating..."));
    else file_size_label->setText(tr_text("This format cannot hold an image of this size."));
}

void SaveImageDialog::update_save_enabled() {
    auto problem = file_name_problem(name_edit->text());
    const auto directory = directory_edit->text().trimmed();
    if (problem.isEmpty() && !ToolSettingsManager::is_absolute_save_path(directory)) problem = tr_text("Choose a full folder path.");
    name_hint->setText(problem);
    name_hint->setVisible(!problem.isEmpty());
    // Saving while encoding is allowed: Save encodes again itself. Only a finished
    // encode that failed blocks it.
    const bool encode_failed = encoded_bytes_ < 0 && !encoding_ && !encode_timer_.isActive();
    save_button->setEnabled(problem.isEmpty() && !encode_failed);
}

void SaveImageDialog::choose_directory() {
    const auto chosen = get_existing_directory(this, tr_text("Select Folder"), QDir::fromNativeSeparators(directory_edit->text().trimmed()));
    if (chosen.isEmpty()) return;
    directory_edit->setText(QDir::toNativeSeparators(chosen));
    directory_edit->setCursorPosition(0);
}

void SaveImageDialog::switch_to_classic() {
    if (!show_confirm_dialog(this, tr_text("Use Classic Dialog"),
                             tr_text("Use the system Save As dialog for this and every later image save? "
                                     "You can change this back in Settings > Screenshot > Save Dialog.")))
        return;
    config_->set_app_setting("save_dialog_style", "classic");
    classic_ = true;
    reject();
}

void SaveImageDialog::accept_save() {
    update_save_enabled();
    if (!save_button->isEnabled()) return;
    const auto path = target_path();
    const auto directory = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(directory)) {
        show_warning_dialog(this, tr_text("Save Failed"), tr_text("Cannot create the folder:\n%1").arg(QDir::toNativeSeparators(directory)));
        return;
    }
    if (QFileInfo::exists(path) &&
        !show_confirm_dialog(this, tr_text("Replace File"), tr_text("%1 already exists.\nReplace it?").arg(QFileInfo(path).fileName())))
        return;
    const auto image = SaveService::scale_for_save(image_, target_size());
    if (!SaveService::save_qimage_to_path(image, path, current_format(), SaveService::DEFAULT_PDF_DPI, current_quality())) {
        show_warning_dialog(this, tr_text("Save Failed"), QDir::toNativeSeparators(path));
        return;
    }
    config_->set_app_setting("save_dialog_last_dir", QDir::toNativeSeparators(directory));
    config_->set_app_setting("save_dialog_format", current_format());
    config_->set_screenshot_quality(current_quality());
    saved_path_ = QDir::toNativeSeparators(path);
    accept();
}

void SaveImageDialog::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto &t = ui_tokens(this);
    const auto rect = QRectF(this->rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(css_color(t.window_border), 1));
    painter.setBrush(css_color(t.popup_background));
    painter.drawRoundedRect(rect, frosted_corner_radius_, frosted_corner_radius_);
}

bool SaveImageDialog::eventFilter(QObject *watched, QEvent *event) {
    if (watched == preview && event->type() == QEvent::Resize)
        view_mode->move((preview->width() - view_mode->width()) / 2, dialog_scaled(12));
    return FrostedFramelessDialog::eventFilter(watched, event);
}

void SaveImageDialog::apply_theme() {
    const auto &t = ui_tokens(this);
    const int radius = dialog_scaled(12);
    const QString fill = t.is_dark() ? "#1F2327" : "#F1F3F5";
    panel_->setStyleSheet(QString(R"(
        QLabel#blockTitle { color: %1; font-size: %2px; background: transparent; }
        QLabel#hint { color: %1; font-size: %3px; background: transparent; }
        QLabel#warningHint { color: %4; font-size: %3px; background: transparent; }
        QLabel#sizeValue { color: %5; font-size: %6px; background: transparent; }
        QLabel#sizeSuffix { color: %1; font-size: %2px; background: transparent; }
        QLabel#pathTagText { color: %5; font-size: %2px; background: transparent; }
        QLineEdit#field { background: %7; color: %5; border: none; border-radius: %8px; padding: 0 %9px;
            min-height: %10px; max-height: %10px; font-size: %2px; selection-background-color: %11; }
        QFrame#sizeInput { background: %7; border: none; border-radius: %8px; }
        QFrame#sizeInput QSpinBox { background: transparent; border: none; color: %5; font-size: %2px; padding-left: %9px; }
        QFrame#pathTag { background: %12; border: 1px solid %13; border-radius: %8px; }
        QFrame#pathTag:hover { border-color: %14; }
        QFrame#pathTag[current="true"] { border-color: %11; }
        QToolButton#pathTagAction { background: transparent; border: none; }
        QToolButton#pathTagAdd, QToolButton#outlineIcon { background: %12; border: 1px solid %13; border-radius: %8px; }
        QToolButton#pathTagAdd:hover, QToolButton#outlineIcon:hover { border-color: %11; }
        QToolButton#sizeLock { background: transparent; border: none; }
        QSlider { background: transparent; min-height: %15px; }
        QSlider::groove:horizontal { height: %16px; background: %17; border-radius: %18px; }
        QSlider::sub-page:horizontal { background: %11; border-radius: %18px; }
        QSlider::add-page:horizontal { background: %17; border-radius: %18px; }
        QSlider::handle:horizontal { width: %19px; height: %19px; margin: %20px 0; background: white; border: 1px solid %14; border-radius: %21px; }
        QSlider::handle:horizontal:hover { border-color: %11; }
        )")
                              .arg(t.text_muted)
                              .arg(dialog_scaled(13))
                              .arg(dialog_scaled(12))
                              .arg(warning_color(t), t.text)
                              .arg(dialog_scaled(14))
                              .arg(fill)
                              .arg(radius)
                              .arg(dialog_scaled(12))
                              .arg(dialog_scaled(36))
                              .arg(t.accent, t.popup_background, t.border, t.border_hover)
                              .arg(dialog_scaled(20))
                              .arg(dialog_scaled(4))
                              .arg(t.switch_off)
                              .arg(dialog_scaled(2))
                              .arg(dialog_scaled(16))
                              .arg(-dialog_scaled(6))
                              .arg(dialog_scaled(8)));
    for (auto *label : {zoom_label, original_label})
        label->setStyleSheet(QString("QLabel { color: %1; font-size: %2px; background: transparent; }").arg(t.text_muted).arg(dialog_scaled(12)));
    for (auto *button : {save_button, change_button}) button->setFixedHeight(dialog_scaled(34));
    lock_button->setIcon(lock_icon(lock_button->isChecked() ? css_color(t.accent) : css_color(t.text_muted), lock_button->isChecked()));
    reset_size_button->setIcon(tinted_icon(FluentIcon::Sync, t.text_muted));
    reset_size_button->setIconSize(QSize(dialog_scaled(16), dialog_scaled(16)));
    classic_link->setText(QString("<a href=\"classic\" style=\"color: %1; text-decoration: underline;\">%2</a>").arg(t.text_muted, tr_text("Use classic dialog")));
    classic_link->setStyleSheet(QString("QLabel { font-size: %1px; background: transparent; }").arg(dialog_scaled(12)));
    update();
}

// 80% of the screen at most; a small image still gets room for the panel and a usable preview.
void SaveImageDialog::place(QWidget *parent) {
    QScreen *screen = parent && parent->window()->screen() ? parent->window()->screen() : QApplication::screenAt(QCursor::pos());
    if (!screen) screen = QApplication::primaryScreen();
    const auto available = screen->availableGeometry();
    const auto margins = layout()->contentsMargins();
    const int chrome_w = margins.left() + margins.right() + dialog_scaled(16) + panel_->width();
    const int chrome_h = margins.top() + margins.bottom() + dialog_scaled(40) + dialog_scaled(70);
    const int width = std::clamp(image_.width() + dialog_scaled(24) + chrome_w, dialog_scaled(860), static_cast<int>(available.width() * 0.8));
    const int height = std::clamp(image_.height() + dialog_scaled(24) + chrome_h, dialog_scaled(600), static_cast<int>(available.height() * 0.8));
    resize(std::min(width, available.width()), std::min(height, available.height()));
    move(available.center() - rect().center());
}
}
