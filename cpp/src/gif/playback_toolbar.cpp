#include "gif/playback_toolbar.h"
#include "core/i18n.h"
#include "core/ui_scale.h"
#include "gif/_widgets.h"
#include "ui/base_settings_panel.h"
#include <QCursor>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>
#include <algorithm>

namespace jietuba {
namespace {
QString tr_text(const char *text) { return i18n_tr(text, "GifPlaybackToolbar"); }
}

RangeSlider::RangeSlider(QWidget *parent) : QWidget(parent) {
    setCursor(QCursor(Qt::PointingHandCursor));
    apply_scale();
}

void RangeSlider::apply_scale() {
    setFixedHeight(scaled(BASE_HEIGHT));
    setMinimumWidth(scaled(BASE_MIN_WIDTH));
    update();
}

void RangeSlider::set_range(int total) {
    total_ = std::max(total, 1);
    trim_start_ = 0;
    trim_end_ = total_ - 1;
    playhead_ = 0;
    update();
}

void RangeSlider::set_playhead(int index) {
    playhead_ = std::max(0, std::min(index, total_ - 1));
    update();
}

void RangeSlider::set_trim(int start, int end) {
    trim_start_ = std::max(0, start);
    trim_end_ = std::min(end, total_ - 1);
    update();
}

QRect RangeSlider::track_rect() const {
    const int track_h = scaled(BASE_TRACK_H);
    const int margin = scaled(BASE_HANDLE_W);
    const int y = python_floordiv(height() - track_h, 2);
    return QRect(margin, y, width() - 2 * margin, track_h);
}

int RangeSlider::index_to_x(int index) const {
    const auto tr = track_rect();
    if (total_ <= 1) return tr.left();
    return tr.left() + static_cast<int>(static_cast<double>(index) / (total_ - 1) * tr.width());
}

int RangeSlider::x_to_index(int x) const {
    const auto tr = track_rect();
    const double ratio = std::max(0.0, std::min(1.0, static_cast<double>(x - tr.left()) / std::max(tr.width(), 1)));
    return static_cast<int>(python_round(ratio * (total_ - 1)));
}

QRect RangeSlider::handle_rect(int index) const {
    const int w = scaled(BASE_HANDLE_W), h = scaled(BASE_HANDLE_H);
    const int x = index_to_x(index);
    const int y = python_floordiv(height() - h, 2);
    return QRect(x - python_floordiv(w, 2), y, w, h);
}

void RangeSlider::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto tr = track_rect();
    // The background track.
    p.setPen(Qt::NoPen);
    p.setBrush(QBrush(QColor(220, 220, 220)));
    p.drawRoundedRect(tr, 3, 3);
    // The selected range.
    const int x1 = index_to_x(trim_start_), x2 = index_to_x(trim_end_);
    p.setBrush(QBrush(QColor(33, 150, 243, 120)));
    p.drawRoundedRect(QRect(x1, tr.top(), x2 - x1, tr.height()), 3, 3);
    // Trim handles.
    for (const auto &rect : {handle_rect(trim_start_), handle_rect(trim_end_)}) {
        const QColor color(33, 150, 243);
        p.setBrush(QBrush(color));
        p.setPen(QPen(color.darker(120), 1));
        p.drawRoundedRect(rect, 2, 2);
    }
    // The playhead.
    const int playhead_w = scaled(BASE_PLAYHEAD_W);
    const int handle_h = scaled(BASE_HANDLE_H);
    const int px = index_to_x(playhead_);
    const int py = python_floordiv(height() - handle_h, 2);
    p.setPen(Qt::NoPen);
    p.setBrush(QBrush(QColor(244, 67, 54)));
    p.drawRoundedRect(QRect(px - python_floordiv(playhead_w, 2), py, playhead_w, handle_h), 2, 2);
    p.end();
}

void RangeSlider::mousePressEvent(QMouseEvent *event) {
    const auto pos = event->pos();
    const int slack = scaled(BASE_GRAB_SLACK);
    if (handle_rect(trim_start_).adjusted(-slack, -slack, slack, slack).contains(pos)) dragging_ = "start";
    else if (handle_rect(trim_end_).adjusted(-slack, -slack, slack, slack).contains(pos)) dragging_ = "end";
    else emit seek_requested(x_to_index(pos.x()));  // a click on empty track seeks
    QWidget::mousePressEvent(event);
}

void RangeSlider::mouseMoveEvent(QMouseEvent *event) {
    if (!dragging_.isEmpty()) {
        const int idx = x_to_index(event->pos().x());
        if (dragging_ == "start") trim_start_ = std::max(0, std::min(idx, trim_end_ - 1));
        else trim_end_ = std::min(total_ - 1, std::max(idx, trim_start_ + 1));
        emit trim_changed(trim_start_, trim_end_);
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void RangeSlider::mouseReleaseEvent(QMouseEvent *event) {
    dragging_.clear();
    QWidget::mouseReleaseEvent(event);
}

PlaybackToolbar::PlaybackToolbar(QWidget *parent) : QWidget(parent) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::NoFocus);
    build_ui();
    // Recomputes sizes on scale changes; the connection ends with this widget.
    connect(&get_ui_scale(), &UIScaleManager::scale_changed, this, &PlaybackToolbar::apply_scale);
}

void PlaybackToolbar::paintEvent(QPaintEvent *) { paint_rounded_panel(this); }

void PlaybackToolbar::set_toolbar_side(bool below) { speed_label_->popup_below = below; }

void PlaybackToolbar::set_button_icon(QPushButton *button, const QString &svg) {
    auto it = std::find_if(button_svgs_.begin(), button_svgs_.end(), [button](const auto &entry) { return entry.first == button; });
    if (it == button_svgs_.end()) button_svgs_.emplace_back(button, svg);
    else it->second = svg;
    button->setIcon(svg_icon(svg, scaled(BASE_ICON)));
}

void PlaybackToolbar::apply_scale() {
    container_->setStyleSheet(toolbar_style());
    vbox_->setContentsMargins(scaled(10), scaled(6), scaled(10), scaled(6));
    vbox_->setSpacing(scaled(4));
    row_->setSpacing(scaled(6));
    slider_->apply_scale();
    time_label_->setStyleSheet(QString("color: #666; font-size: %1px;").arg(scaled(12)));
    speed_label_->apply_scale();
    const int btn_w = scaled(BASE_BTN_W), btn_h = scaled(BASE_BTN_H);
    const int icon = scaled(BASE_ICON);
    for (const auto &[button, svg] : button_svgs_) {
        button->setFixedSize(btn_w, btn_h);
        button->setIconSize(QSize(icon, icon));
        button->setIcon(svg_icon(svg, icon));
    }
    setMinimumWidth(scaled(BASE_MIN_WIDTH));
    setFixedHeight(scaled(BASE_HEIGHT));
}

void PlaybackToolbar::build_ui() {
    container_ = new QWidget(this);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    outer->addWidget(container_);

    vbox_ = new QVBoxLayout(container_);

    // First row: the range slider.
    slider_ = new RangeSlider;
    connect(slider_, &RangeSlider::seek_requested, this, &PlaybackToolbar::seek_requested);
    connect(slider_, &RangeSlider::trim_changed, this, &PlaybackToolbar::trim_changed);
    vbox_->addWidget(slider_);

    // Second row: buttons.
    row_ = new QHBoxLayout;

    // Play / pause: stopped shows "restart", playing shows "pause".
    play_btn_ = new QPushButton;
    set_button_icon(play_btn_, QString::fromUtf8("重开录制.svg"));
    play_btn_->setToolTip(tr_text("播放"));
    connect(play_btn_, &QPushButton::clicked, this, &PlaybackToolbar::on_play_clicked);
    row_->addWidget(play_btn_);

    time_label_ = new QLabel("00:00 / 00:00");
    row_->addWidget(time_label_);

    // Speed, chosen from a menu.
    speed_label_ = new ClickMenuButton({{"0.5x", 0.5}, {"0.75x", 0.75}, {"1.0x", 1.0}, {"1.25x", 1.25}, {"1.5x", 1.5}, {"2.0x", 2.0}}, 2);
    connect(speed_label_, &ClickMenuButton::option_selected, this, [this](const QVariant &value) { emit speed_changed(value.toDouble()); });
    row_->addWidget(speed_label_);

    row_->addStretch();

    rerecord_btn_ = new QPushButton;
    set_button_icon(rerecord_btn_, QString::fromUtf8("重新录制.svg"));
    rerecord_btn_->setToolTip(tr_text("重新录制"));
    connect(rerecord_btn_, &QPushButton::clicked, this, &PlaybackToolbar::rerecord);
    row_->addWidget(rerecord_btn_);

    // The cursor switch, right of re-record; on by default and shown checked.
    cursor_btn_ = new QPushButton;
    cursor_btn_->setCheckable(true);
    cursor_btn_->setChecked(true);
    cursor_btn_->setToolTip(tr_text("显示鼠标光标"));
    set_button_icon(cursor_btn_, QString::fromUtf8("鼠标.svg"));
    connect(cursor_btn_, &QPushButton::clicked, this, [this] { emit cursor_toggled(cursor_btn_->isChecked()); });
    row_->addWidget(cursor_btn_);

    copy_btn_ = new QPushButton;
    set_button_icon(copy_btn_, QString::fromUtf8("复制.svg"));
    copy_btn_->setToolTip(tr_text("复制到剪贴板"));
    connect(copy_btn_, &QPushButton::clicked, this, &PlaybackToolbar::copy_requested);
    row_->addWidget(copy_btn_);

    save_btn_ = new QPushButton;
    set_button_icon(save_btn_, QString::fromUtf8("保存.svg"));
    save_btn_->setToolTip(tr_text("另存为"));
    connect(save_btn_, &QPushButton::clicked, this, &PlaybackToolbar::save_requested);
    row_->addWidget(save_btn_);

    close_btn_ = new QPushButton;
    set_button_icon(close_btn_, "fluent_icons/Close.svg");
    close_btn_->setToolTip(tr_text("关闭"));
    connect(close_btn_, &QPushButton::clicked, this, &PlaybackToolbar::close_requested);
    row_->addWidget(close_btn_);

    vbox_->addLayout(row_);
    apply_scale();
}

QString PlaybackToolbar::fmt(double seconds) {
    const int whole = static_cast<int>(seconds);
    return QString("%1:%2").arg(whole / 60, 2, 10, QChar('0')).arg(whole % 60, 2, 10, QChar('0'));
}

void PlaybackToolbar::set_total_frames(int total, int fps, int total_ms) {
    fps_ = std::max(fps, 1);
    total_frames_ = std::max(total, 1);
    // The real total duration in ms; 0 falls back to frame indices.
    total_ms_ = total_ms;
    slider_->set_range(total);
    const double total_s = total_ms > 0 ? total_ms / 1000.0 : (total > 1 ? static_cast<double>(total - 1) / fps_ : 0.0);
    time_label_->setText("00:00 / " + fmt(total_s));
}

void PlaybackToolbar::set_playhead(int index, std::optional<int> total, std::optional<int> fps, int current_ms) {
    const int f = fps.value_or(fps_);
    const int t = total.value_or(total_frames_);
    slider_->set_playhead(index);
    const double cur_s = current_ms >= 0 ? current_ms / 1000.0 : (f > 0 ? static_cast<double>(index) / f : 0.0);
    const double total_s = total_ms_ > 0 ? total_ms_ / 1000.0 : (t > 1 && f > 0 ? static_cast<double>(t - 1) / f : 0.0);
    time_label_->setText(fmt(cur_s) + " / " + fmt(total_s));
}

void PlaybackToolbar::set_playing(bool playing) {
    playing_ = playing;
    if (playing) {
        set_button_icon(play_btn_, QString::fromUtf8("暂停录制.svg"));
        play_btn_->setToolTip(tr_text("暂停"));
    } else {
        set_button_icon(play_btn_, QString::fromUtf8("重开录制.svg"));
        play_btn_->setToolTip(tr_text("播放"));
    }
}

void PlaybackToolbar::on_play_clicked() {
    playing_ = !playing_;
    set_playing(playing_);
    emit play_pause();
}
}
