#include "gif/record_toolbar.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "gif/_widgets.h"
#include "settings/tool_settings.h"
#include "tools/controller.h"
#include "ui/arrow_settings_panel.h"
#include "ui/base_settings_panel.h"
#include "ui/paint_settings_panel.h"
#include "ui/shape_settings_panel.h"
#include "ui/text_settings_panel.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>
#include <QVBoxLayout>
#include <algorithm>
#include <set>

namespace jietuba {
namespace {
QString tr_text(const char *text) { return i18n_tr(text, "GifRecordToolbar"); }

QString seconds_text(int seconds) {
    return seconds ? tr_text("{seconds} 秒").replace("{seconds}", QString::number(seconds)) : tr_text("无延迟");
}

// Drawing tool buttons: (tool_id, svg_file, tooltip).
struct DrawTool {
    const char *id;
    const char *svg;
    const char *tip;
};
const DrawTool DRAW_TOOLS[] = {
    {"pen", "画笔.svg", "画笔"},
    {"rect", "方框.svg", "矩形"},
    {"ellipse", "圆框.svg", "椭圆"},
    {"arrow", "箭头.svg", "箭头"},
    {"text", "文字.svg", "文字"},
    {"eraser", "橡皮.svg", "橡皮擦"},
};

template <class Panel>
void prepare_panel(Panel *panel) {
    panel->setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    panel->setAttribute(Qt::WA_ShowWithoutActivating);
    panel->setFocusPolicy(Qt::NoFocus);
}
}

RecordToolbar::RecordToolbar(QWidget *parent) : QWidget(parent), config_(get_tool_settings_manager()) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::NoFocus);

    mode_ = jietuba::get_recording_mode(config_);
    video_options_ = RecordingOptions::from_config(config_);
    const int delay = config_->get_recording_delay();
    const std::set<int> delays{0, 1, 2, 3, 5, 10};
    start_delay_ = delays.count(delay) ? delay : 0;

    build_ui();
    init_settings_panels();
    // Recomputes sizes on scale changes; the connection ends with this widget.
    connect(&get_ui_scale(), &UIScaleManager::scale_changed, this, &RecordToolbar::apply_scale);
}

// The panels are windows of their own without a parent, so the toolbar deletes them.
RecordToolbar::~RecordToolbar() {
    for (QWidget *panel : unique_panels()) delete panel;
}

// ── UI ──

QString RecordToolbar::time_label_qss() const {
    return QString("QPushButton { color: #596579; font-size: %1px; border: none; min-width: %2px; padding: %3px; }"
                   "QPushButton:hover { background: rgba(15,23,42,0.07); }")
        .arg(scaled(12))
        .arg(scaled(52))
        .arg(scaled(4));
}

void RecordToolbar::paintEvent(QPaintEvent *) { paint_rounded_panel(this); }

std::vector<QWidget *> RecordToolbar::unique_panels() const {
    std::vector<QWidget *> panels;
    for (QWidget *panel : std::initializer_list<QWidget *>{paint_panel, shape_panel, arrow_panel, text_panel})
        if (panel) panels.push_back(panel);
    return panels;
}

void RecordToolbar::apply_scale() {
    apply_scale_sizes();
    if (paint_panel) paint_panel->apply_scale();
    if (shape_panel) shape_panel->apply_scale();
    if (arrow_panel) arrow_panel->apply_scale();
    if (text_panel) text_panel->apply_scale();
    adjustSize();
    reposition_panels();
}

void RecordToolbar::set_button_icon(QPushButton *button, const QString &svg) {
    auto it = std::find_if(button_svgs_.begin(), button_svgs_.end(), [button](const auto &entry) { return entry.first == button; });
    if (it == button_svgs_.end()) button_svgs_.emplace_back(button, svg);
    else it->second = svg;
    button->setIcon(svg_icon(svg, scaled(BASE_ICON)));
}

void RecordToolbar::apply_scale_sizes() {
    setFixedHeight(scaled(BASE_HEIGHT));
    container_->setStyleSheet(toolbar_style());
    result_container_->setStyleSheet(toolbar_style());
    const int inset = python_floordiv(scaled(BASE_HEIGHT) - scaled(BASE_BTN_H), 2);
    row_layout_->setContentsMargins(scaled(8), inset, scaled(8), inset);
    row_layout_->setSpacing(scaled(6));

    const int btn_w = scaled(BASE_BTN_W), btn_h = scaled(BASE_BTN_H);
    const int icon_sz = scaled(BASE_ICON);
    for (const auto &[button, svg] : button_svgs_) {
        button->setFixedSize(btn_w, btn_h);
        button->setIconSize(QSize(icon_sz, icon_sz));
        button->setIcon(svg_icon(svg, icon_sz));
    }
    time_label_->setStyleSheet(time_label_qss());
    fps_btn_->apply_scale();
    mode_btn_->apply_scale();
    for (auto *button : result_buttons_) {
        button->setMinimumHeight(btn_h);
        button->setStyleSheet(QString("padding: 0 %1px;").arg(scaled(9)));
    }
    result_layout_->setContentsMargins(scaled(12), inset, scaled(12), inset);
    result_layout_->setSpacing(scaled(8));
    result_label_->setStyleSheet(QString("color: #3e7960; font-size: %1px;").arg(scaled(12)));
    for (auto *sep : separators_) sep->setFixedSize(1, scaled(BASE_SEP_HEIGHT));
    const int handle_icon = scaled(BASE_HANDLE_ICON);
    move_handle_->setFixedSize(scaled(BASE_HANDLE_WIDTH), btn_h);
    move_handle_->setPixmap(svg_icon(QString::fromUtf8("移动窗口.svg"), handle_icon).pixmap(QSize(handle_icon, handle_icon)));
}

void RecordToolbar::build_ui() {
    container_ = new QWidget(this);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    // The two state rows alternate; the hidden result row must not widen the main row to another language's width.
    outer->setSizeConstraint(QLayout::SetNoConstraint);
    outer->addWidget(container_);

    auto *layout = new QHBoxLayout(container_);
    row_layout_ = layout;
    // Sizes are set together by apply_scale_sizes for the current scale.

    // Time and the main actions first. Settings buttons keep their place while recording to avoid misclicks.
    time_label_ = new QPushButton("00:00");
    time_label_->setToolTip(tr_text("录制延迟"));
    connect(time_label_, &QPushButton::clicked, this, &RecordToolbar::show_delay_menu);
    layout->addWidget(time_label_);

    // ── Left: recording control ──
    mode_btn_ = new ClickMenuButton({{"GIF", "gif"}, {"MP4", "mp4"}}, mode_ == "mp4" ? 1 : 0);
    mode_btn_->setToolTip(tr_text("录制格式"));
    connect(mode_btn_, &ClickMenuButton::option_selected, this, [this](const QVariant &value) { on_mode_selected(value.toString()); });
    layout->addWidget(mode_btn_);

    // Frame rate.
    QList<qint64> fps_options{5, 10, 16, 24, 48, 60};
    qint64 fps_default = 16;
    try {
        const auto options = config_->get_gif_fps_options().toList();
        fps_options.clear();
        for (const auto &value : options) fps_options.append(value.toLongLong());
        fps_default = config_->get_gif_fps();
    } catch (const std::exception &e) {
        log_exception(e, T("加载 GIF 帧率设置"));
        fps_options = {5, 10, 16, 24, 48, 60};
        fps_default = 16;
    }
    for (const auto value : fps_options) gif_fps_pairs_.emplace_back(QString("%1 fps").arg(value), value);
    int default_index = 0;
    for (int i = 0; i < static_cast<int>(gif_fps_pairs_.size()); ++i) {
        if (gif_fps_pairs_[i].second.toLongLong() == fps_default) {
            default_index = i;
            break;
        }
    }
    fps_btn_ = new ClickMenuButton(gif_fps_pairs_, default_index);
    // An old configuration may keep a removed frame rate; it falls back to the first item like the menu.
    gif_fps_ = gif_fps_pairs_[default_index].second.toInt();
    connect(fps_btn_, &ClickMenuButton::option_selected, this, &RecordToolbar::on_fps_selected);
    layout->addWidget(fps_btn_);

    video_settings_btn_ = new QPushButton;
    set_button_icon(video_settings_btn_, QString::fromUtf8("录制设置.svg"));
    video_settings_btn_->setToolTip(tr_text("视频设置"));
    connect(video_settings_btn_, &QPushButton::clicked, this, &RecordToolbar::show_video_settings);

    // Record / stop.
    record_btn_ = new QPushButton;
    set_button_icon(record_btn_, QString::fromUtf8("开始录制.svg"));
    record_btn_->setToolTip(tr_text("开始录制"));
    connect(record_btn_, &QPushButton::clicked, this, &RecordToolbar::on_record_clicked);
    layout->addWidget(record_btn_);

    // Pause / resume.
    pause_btn_ = new QPushButton;
    set_button_icon(pause_btn_, QString::fromUtf8("暂停不可.svg"));
    pause_btn_->setToolTip(tr_text("暂停录制"));
    pause_btn_->setEnabled(false);
    connect(pause_btn_, &QPushButton::clicked, this, &RecordToolbar::on_pause_clicked);
    layout->addWidget(pause_btn_);

    audio_btn_ = new QPushButton;
    audio_btn_->setToolTip(tr_text("录制系统声音"));
    connect(audio_btn_, &QPushButton::clicked, this, &RecordToolbar::show_audio_menu);
    layout->addWidget(audio_btn_);
    cursor_btn_ = new QPushButton;
    cursor_btn_->setCheckable(true);
    set_button_icon(cursor_btn_, QString::fromUtf8("鼠标指针.svg"));
    cursor_btn_->setToolTip(tr_text("录制鼠标指针"));
    connect(cursor_btn_, &QPushButton::clicked, this, [this](bool checked) { save_quick_option(std::nullopt, checked); });
    layout->addWidget(cursor_btn_);
    layout->addWidget(video_settings_btn_);

    auto add_separator = [this, layout] {
        auto *sep = new QWidget;
        set_own_style(sep, "background: rgba(15,23,42,0.12); border: none;");
        separators_.push_back(sep);
        layout->addWidget(sep);
    };
    add_separator();

    // ── Middle: drawing tools ──
    for (const auto &tool : DRAW_TOOLS) {
        auto *btn = new QPushButton;
        set_button_icon(btn, QString::fromUtf8(tool.svg));
        btn->setToolTip(tr_text(tool.tip));
        btn->setCheckable(true);
        const QString tool_id = tool.id;
        connect(btn, &QPushButton::clicked, this, [this, tool_id] { on_tool_clicked(tool_id); });
        layout->addWidget(btn);
        tool_buttons_.emplace_back(tool_id, btn);
    }

    add_separator();

    // ── Undo / redo ──
    undo_btn_ = new QPushButton;
    set_button_icon(undo_btn_, QString::fromUtf8("撤回.svg"));
    undo_btn_->setToolTip(tr_text("撤销"));
    connect(undo_btn_, &QPushButton::clicked, this, &RecordToolbar::undo_requested);
    layout->addWidget(undo_btn_);

    redo_btn_ = new QPushButton;
    set_button_icon(redo_btn_, QString::fromUtf8("复原.svg"));
    redo_btn_->setToolTip(tr_text("重做"));
    connect(redo_btn_, &QPushButton::clicked, this, &RecordToolbar::redo_requested);
    layout->addWidget(redo_btn_);

    // ── Right: drag + close ──
    layout->addStretch();

    move_handle_ = new QLabel;
    move_handle_->setAlignment(Qt::AlignCenter);
    set_own_style(move_handle_, "background: transparent; border: none;");
    move_handle_->setCursor(QCursor(Qt::SizeAllCursor));
    move_handle_->installEventFilter(this);
    layout->addWidget(move_handle_);

    close_btn_ = new QPushButton;
    set_button_icon(close_btn_, QString::fromUtf8("结束截图.svg"));
    close_btn_->setToolTip(tr_text("关闭"));
    connect(close_btn_, &QPushButton::clicked, this, &RecordToolbar::close_requested);
    layout->addWidget(close_btn_);

    result_container_ = new QWidget(this);
    result_layout_ = new QHBoxLayout(result_container_);
    result_layout_->setContentsMargins(scaled(12), scaled(6), scaled(12), scaled(6));
    result_layout_->setSpacing(scaled(8));
    result_label_ = new QLabel(tr_text("视频已保存"));
    result_layout_->addWidget(result_label_);
    const std::pair<const char *, void (RecordToolbar::*)()> result_actions[] = {
        {"播放视频", &RecordToolbar::video_open_requested},
        {"打开文件夹", &RecordToolbar::video_folder_requested},
        {"复制文件", &RecordToolbar::video_copy_requested},
        {"重新录制", &RecordToolbar::video_rerecord_requested},
    };
    for (const auto &[text, signal] : result_actions) {
        auto *button = new QPushButton(tr_text(text));
        button->setStyleSheet(QString("padding: 0 %1px;").arg(scaled(9)));
        connect(button, &QPushButton::clicked, this, signal);
        result_buttons_.push_back(button);
        result_layout_->addWidget(button);
    }
    auto *result_close = new QPushButton;
    set_button_icon(result_close, QString::fromUtf8("结束截图.svg"));
    result_close->setToolTip(tr_text("关闭"));
    connect(result_close, &QPushButton::clicked, this, &RecordToolbar::close_requested);
    result_layout_->addWidget(result_close);
    outer->addWidget(result_container_);
    result_container_->hide();

    refresh_recording_options();

    apply_scale_sizes();
    adjustSize();
}

// ── Settings panels ──

void RecordToolbar::init_settings_panels() {
    paint_panel = new PaintSettingsPanel;
    prepare_panel(paint_panel);
    connect(paint_panel, &BaseSettingsPanel::color_changed, this, &RecordToolbar::color_changed);
    connect(paint_panel, &BaseSettingsPanel::size_changed, this, &RecordToolbar::width_changed);
    connect(paint_panel, &BaseSettingsPanel::opacity_changed, this, &RecordToolbar::opacity_changed);
    connect(paint_panel, &PaintSettingsPanel::line_style_changed, this, &RecordToolbar::on_line_style_changed);
    paint_panel->hide();

    // Rectangle and ellipse.
    shape_panel = new ShapeSettingsPanel;
    prepare_panel(shape_panel);
    connect(shape_panel, &BaseSettingsPanel::color_changed, this, &RecordToolbar::color_changed);
    connect(shape_panel, &BaseSettingsPanel::size_changed, this, &RecordToolbar::width_changed);
    connect(shape_panel, &BaseSettingsPanel::opacity_changed, this, &RecordToolbar::opacity_changed);
    connect(shape_panel, &ShapeSettingsPanel::line_style_changed, this, &RecordToolbar::on_line_style_changed);
    shape_panel->hide();

    arrow_panel = new ArrowSettingsPanel;
    prepare_panel(arrow_panel);
    connect(arrow_panel, &ArrowSettingsPanel::color_changed, this, &RecordToolbar::color_changed);
    connect(arrow_panel, &ArrowSettingsPanel::size_changed, this, &RecordToolbar::width_changed);
    connect(arrow_panel, &ArrowSettingsPanel::opacity_changed, this, &RecordToolbar::opacity_changed);
    connect(arrow_panel, &ArrowSettingsPanel::arrow_style_changed, this, &RecordToolbar::on_arrow_style_changed);
    arrow_panel->hide();

    text_panel = new TextSettingsPanel;
    prepare_panel(text_panel);
    connect(text_panel, &TextSettingsPanel::color_changed, this, &RecordToolbar::color_changed);
    connect(text_panel, &TextSettingsPanel::font_changed, this, &RecordToolbar::on_text_font_changed);
    connect(text_panel, &TextSettingsPanel::background_changed, this, &RecordToolbar::on_text_background_changed);
    connect(text_panel, &TextSettingsPanel::outline_changed, this, &RecordToolbar::on_text_outline_changed);
    connect(text_panel, &TextSettingsPanel::shadow_changed, this, &RecordToolbar::on_text_shadow_changed);
    text_panel->hide();

    panel_map_ = {
        {"pen", paint_panel},
        {"rect", shape_panel},
        {"ellipse", shape_panel},
        {"arrow", arrow_panel},
        {"text", text_panel},
    };
    text_menu = text_panel;

    load_saved_settings();
}

void RecordToolbar::load_saved_settings() {
    try {
        if (auto *pen_settings = config_->get_tool_settings("pen"))
            paint_panel->set_line_style(pen_settings->get("line_style", "solid").toString());
        if (auto *arrow_settings = config_->get_tool_settings("arrow"))
            arrow_panel->set_arrow_style(arrow_settings->get("arrow_style", "single").toString());
        text_panel->load_from_config();
    } catch (const std::exception &e) {
        log_exception(e, T("加载绘制工具设置"));
    }
}

void RecordToolbar::on_arrow_style_changed(const QString &style) {
    try {
        config_->update_settings("arrow", {{"arrow_style", style}});
    } catch (const std::exception &e) {
        log_exception(e, T("保存箭头样式"));
    }
    emit arrow_style_changed(style);
}

void RecordToolbar::on_line_style_changed(const QString &style) {
    try {
        const QString tool_id = current_tool_.value_or("pen");
        if (tool_id == "rect" || tool_id == "ellipse") config_->update_settings(tool_id, {{"line_style", style}});
        else if (tool_id == "pen") config_->update_settings("pen", {{"line_style", style}});
    } catch (const std::exception &e) {
        log_exception(e, T("保存线条样式"));
    }
    emit line_style_changed(style);
}

void RecordToolbar::on_text_font_changed(const QFont &font) {
    TextSettingsPanel::save_font_to_config(font);
    emit font_changed(font);
}

void RecordToolbar::on_text_background_changed(bool enabled, const QColor &color, int opacity) {
    TextSettingsPanel::save_background_to_config(enabled, color, opacity);
    emit background_changed(enabled, color, opacity);
}

void RecordToolbar::on_text_outline_changed(bool enabled, const QColor &color, double width) {
    TextSettingsPanel::save_outline_to_config(enabled, color, width);
    emit outline_changed(enabled, color, width);
}

void RecordToolbar::on_text_shadow_changed(bool enabled, const QColor &color) {
    TextSettingsPanel::save_shadow_to_config(enabled, color);
    emit shadow_changed(enabled, color);
}

// ── Panel placement ──

void RecordToolbar::set_toolbar_side(bool below) {
    toolbar_below_record_rect_ = below;
    fps_btn_->popup_below = below;
    mode_btn_->popup_below = below;
}

void RecordToolbar::position_panel(QWidget *panel) {
    const QPoint tb_pos = mapToGlobal(QPoint(0, 0));
    const int tb_w = width();
    const int tb_h = height();
    const int panel_w = panel->sizeHint().width();
    const int panel_h = panel->sizeHint().height();
    const int gap = scaled(4);

    // The screen under the toolbar, not always the primary one.
    QScreen *screen = QApplication::screenAt(tb_pos);
    if (!screen) screen = QApplication::primaryScreen();
    const QRect screen_rect = screen->geometry();

    // Panels reuse the side the toolbar already chose, as in the screenshot toolbar.
    int y;
    if (toolbar_below_record_rect_) {
        const int y_below = tb_pos.y() + tb_h + gap;
        const int y_above = tb_pos.y() - panel_h - gap;
        if (y_below + panel_h <= screen_rect.bottom()) y = y_below;
        else if (y_above >= screen_rect.top()) y = y_above;
        else y = screen_rect.bottom() - panel_h;
    } else {
        const int y_above = tb_pos.y() - panel_h - gap;
        const int y_below = tb_pos.y() + tb_h + gap;
        if (y_above >= screen_rect.top()) y = y_above;
        else if (y_below + panel_h <= screen_rect.bottom()) y = y_below;
        else y = screen_rect.top();
    }
    y = std::max(screen_rect.top(), std::min(y, screen_rect.bottom() - panel_h));

    // Centred on the toolbar and kept on the current screen.
    int x = tb_pos.x() + python_floordiv(tb_w - panel_w, 2);
    x = std::max(screen_rect.left(), std::min(x, screen_rect.right() - panel_w));
    panel->move(x, y);
}

void RecordToolbar::show_panel_for_tool(const QString &tool_id) {
    hide_all_panels();
    QWidget *panel = panel_map_.value(tool_id);
    if (!panel) return;
    panel->show();
    panel->raise();
    position_panel(panel);
}

void RecordToolbar::hide_all_panels() {
    close_option_menu();
    for (auto *panel : unique_panels()) panel->hide();
}

void RecordToolbar::reposition_panels() {
    for (auto *panel : unique_panels())
        if (panel->isVisible()) position_panel(panel);
}

int RecordToolbar::get_max_panel_height() const {
    const int gap = scaled(4);
    int max_h = 0;
    for (auto *panel : unique_panels()) max_h = std::max(max_h, panel->sizeHint().height());
    return max_h ? max_h + gap : 0;
}

// ── Drawing tools ──

void RecordToolbar::on_tool_clicked(const QString &tool_id) {
    if (current_tool_ == tool_id) {
        // Clicking the same tool again deselects it and returns to pass-through.
        current_tool_.reset();
        update_tool_checked();
        hide_all_panels();
        emit deactivate_requested();
        return;
    }
    current_tool_ = tool_id;
    update_tool_checked();
    show_panel_for_tool(tool_id);
    emit tool_selected(tool_id);
}

void RecordToolbar::update_tool_checked() {
    for (const auto &[tid, btn] : tool_buttons_) btn->setChecked(current_tool_ == tid);
}

void RecordToolbar::highlight_tool(const std::optional<QString> &tool_id) {
    current_tool_ = tool_id && !tool_id->isEmpty() ? tool_id : std::nullopt;
    update_tool_checked();
    if (!current_tool_) hide_all_panels();
}

void RecordToolbar::sync_from_controller(const ToolController &tool_controller) {
    const auto &ctx = tool_controller.ctx;
    const int width = static_cast<int>(ctx.stroke_width);
    const int opacity = static_cast<int>(ctx.opacity * 255);
    for (auto *panel : {static_cast<BaseSettingsPanel *>(paint_panel), static_cast<BaseSettingsPanel *>(shape_panel)}) {
        panel->set_color(ctx.color);
        panel->set_size(width);
        panel->set_opacity(opacity);
    }
    arrow_panel->set_color(ctx.color);
    arrow_panel->set_size(width);
    arrow_panel->set_opacity(opacity);
}

// ── External API ──

void RecordToolbar::set_stroke_width(int width) {
    paint_panel->set_size(width);
    shape_panel->set_size(width);
    arrow_panel->set_size(width);
}

void RecordToolbar::set_opacity(int opacity_255) {
    paint_panel->set_opacity(opacity_255);
    shape_panel->set_opacity(opacity_255);
    arrow_panel->set_opacity(opacity_255);
}

void RecordToolbar::set_elapsed(double seconds) {
    const int whole = static_cast<int>(seconds);
    time_label_->setText(QString("%1:%2").arg(whole / 60, 2, 10, QChar('0')).arg(whole % 60, 2, 10, QChar('0')));
}

void RecordToolbar::reset_state() {
    recording_ = false;
    paused_ = false;
    current_tool_.reset();
    update_tool_checked();
    hide_all_panels();
    result_container_->hide();
    container_->show();
    time_label_->setEnabled(true);
    set_button_icon(record_btn_, QString::fromUtf8("开始录制.svg"));
    record_btn_->setToolTip(tr_text("开始录制"));
    pause_btn_->setEnabled(false);
    set_button_icon(pause_btn_, QString::fromUtf8("暂停不可.svg"));
    pause_btn_->setToolTip(tr_text("暂停录制"));
    fps_btn_->set_enabled(true);
    mode_btn_->set_enabled(true);
    video_settings_btn_->setEnabled(true);
    audio_btn_->setEnabled(true);
    cursor_btn_->setEnabled(true);
    record_btn_->setEnabled(true);
    move_handle_->setEnabled(true);
    move_handle_->setCursor(QCursor(Qt::SizeAllCursor));
    move_handle_->setToolTip("");
    time_label_->setText("00:00");
    time_label_->setStyleSheet(time_label_qss());
    adjustSize();
}

void RecordToolbar::close_option_menu() {
    if (option_menu_) option_menu_->close();
}

void RecordToolbar::popup_options(QMenu *menu, QWidget *anchor) {
    hide_all_panels();
    option_menu_ = menu;
    connect(menu, &QMenu::aboutToHide, this, [this] { option_menu_ = nullptr; });
    popup_menu(menu, anchor, toolbar_below_record_rect_);
}

void RecordToolbar::show_delay_menu() {
    if (!time_label_->isEnabled()) return;
    auto *menu = new QMenu(this);
    menu->addSection(tr_text("录制延迟"));
    for (int seconds : {0, 1, 2, 3, 5, 10}) {
        auto *action = menu->addAction(seconds_text(seconds));
        action->setCheckable(true);
        action->setChecked(start_delay_ == seconds);
        connect(action, &QAction::triggered, this, [this, seconds] { set_start_delay(seconds); });
    }
    popup_options(menu, time_label_);
}

void RecordToolbar::set_start_delay(int seconds) {
    start_delay_ = seconds;
    config_->set_recording_delay(seconds);
    time_label_->setToolTip(tr_text("录制延迟") + ": " + seconds_text(seconds));
}

void RecordToolbar::show_audio_menu() {
    if (!audio_btn_->isEnabled() || mode_ != "mp4") return;
    auto *menu = new QMenu(this);
    auto *action = menu->addAction(tr_text("录制系统声音"));
    action->setCheckable(true);
    action->setChecked(video_options_.system_audio);
    connect(action, &QAction::triggered, this, [this](bool checked) { save_quick_option(checked, std::nullopt); });
    menu->addSeparator();
    menu->addAction(tr_text("系统声音不包含麦克风"))->setEnabled(false);
    popup_options(menu, audio_btn_);
}

void RecordToolbar::save_quick_option(std::optional<bool> system_audio, std::optional<bool> cursor) {
    if (system_audio) video_options_.system_audio = *system_audio;
    if (cursor) video_options_.cursor = *cursor;
    video_options_.save_to_config(config_);
    refresh_recording_options();
}

void RecordToolbar::show_video_result(const QString &path) {
    hide_all_panels();
    result_label_->setText(tr_text("视频已保存"));
    result_label_->setToolTip(path);
    result_buttons_[2]->setText(tr_text("复制文件"));
    container_->hide();
    result_container_->show();
    adjustSize();
}

void RecordToolbar::show_video_copied() { result_buttons_[2]->setText(tr_text("已复制")); }

void RecordToolbar::set_busy(const QString &text, bool allow_stop) {
    close_option_menu();
    time_label_->setEnabled(false);
    record_btn_->setEnabled(allow_stop);
    pause_btn_->setEnabled(false);
    fps_btn_->set_enabled(false);
    mode_btn_->set_enabled(false);
    video_settings_btn_->setEnabled(false);
    audio_btn_->setEnabled(false);
    cursor_btn_->setEnabled(false);
    move_handle_->setEnabled(false);
    time_label_->setText(text);
}

void RecordToolbar::set_video_ready() {
    recording_ = true;
    record_btn_->setEnabled(true);
    pause_btn_->setEnabled(true);
    set_button_icon(pause_btn_, QString::fromUtf8("暂停录制.svg"));
    set_button_icon(record_btn_, QString::fromUtf8("结束录制.svg"));
    record_btn_->setToolTip(tr_text("停止录制"));
    time_label_->setText("00:00");
}

void RecordToolbar::set_video_paused(bool paused) {
    paused_ = paused;
    record_btn_->setEnabled(true);
    pause_btn_->setEnabled(true);
    set_button_icon(pause_btn_, QString::fromUtf8(paused ? "重开录制.svg" : "暂停录制.svg"));
    pause_btn_->setToolTip(paused ? tr_text("恢复录制") : tr_text("暂停录制"));
}

void RecordToolbar::on_mode_selected(const QString &mode) {
    // Choosing the current format again does not release the GIF session, keeping a pending hand-back.
    if (mode == mode_) return;
    mode_ = mode;
    set_recording_mode(config_, mode);
    refresh_recording_options();
    emit mode_changed(mode);
}

void RecordToolbar::refresh_recording_options() {
    std::vector<std::pair<QString, QVariant>> pairs;
    QVariant current;
    if (mode_ == "mp4") {
        std::set<int> values{15, 24, 30, 48, 60, video_options_.fps};
        for (int value : values) pairs.emplace_back(QString("%1 fps").arg(value), value);
        pairs.emplace_back(tr_text("自定义帧率…"), QVariant());
        current = video_options_.fps;
    } else {
        pairs = gif_fps_pairs_;
        current = gif_fps_;
    }
    int index = 0;
    for (int i = 0; i < static_cast<int>(pairs.size()); ++i) {
        if (pairs[i].second.isValid() && pairs[i].second.toLongLong() == current.toLongLong()) {
            index = i;
            break;
        }
    }
    fps_btn_->set_options(std::move(pairs), index);
    video_settings_btn_->setVisible(mode_ == "mp4");
    audio_btn_->setVisible(mode_ == "mp4");
    cursor_btn_->setVisible(mode_ == "mp4");
    cursor_btn_->setChecked(video_options_.cursor);
    set_button_icon(audio_btn_, QString::fromUtf8(video_options_.system_audio ? "录制声音.svg" : "录制静音.svg"));
    adjustSize();
}

void RecordToolbar::show_video_settings() {
    if (video_dialog_) {
        video_dialog_->raise();
        return;
    }
    auto *dialog = new QDialog(this);
    video_dialog_ = dialog;
    dialog->setWindowTitle(tr_text("视频设置"));
    auto *form = new QFormLayout(dialog);
    auto *fps = new QSpinBox(dialog);
    fps->setRange(1, 60);
    fps->setValue(video_options_.fps);
    fps->setSuffix(" fps");
    form->addRow(tr_text("帧率"), fps);
    auto *bitrate = new QDoubleSpinBox(dialog);
    bitrate->setRange(0.5, 50);
    bitrate->setSingleStep(0.5);
    bitrate->setDecimals(1);
    bitrate->setValue(video_options_.bitrate / 1000000.0);
    bitrate->setSuffix(" Mbps");
    auto *presets = new QComboBox(dialog);
    for (int value : {1, 2, 4, 8}) presets->addItem(QString("%1 Mbps").arg(value), static_cast<double>(value));
    presets->addItem(tr_text("自定义"), QVariant());
    const int index = presets->findData(video_options_.bitrate / 1000000.0);
    presets->setCurrentIndex(index >= 0 ? index : 4);
    connect(presets, &QComboBox::currentIndexChanged, dialog, [presets, bitrate](int i) {
        const auto data = presets->itemData(i);
        if (data.isValid()) bitrate->setValue(data.toDouble());
    });
    connect(bitrate, &QDoubleSpinBox::valueChanged, dialog, [presets](double value) {
        const int found = presets->findData(value);
        presets->setCurrentIndex(found >= 0 ? found : 4);
    });
    auto *row = new QHBoxLayout;
    row->addWidget(presets);
    row->addWidget(bitrate);
    form->addRow(tr_text("视频码率"), row);
    auto *audio = new QCheckBox(tr_text("录制系统声音"), dialog);
    audio->setChecked(video_options_.system_audio);
    auto *hardware = new QCheckBox(tr_text("优先使用硬件编码（不可用时使用系统软件编码）"), dialog);
    hardware->setChecked(video_options_.hardware);
    auto *cursor = new QCheckBox(tr_text("录制鼠标指针"), dialog);
    cursor->setChecked(video_options_.cursor);
    for (auto *widget : {audio, hardware, cursor}) form->addRow(widget);
    auto *hint = new QLabel(tr_text("码率是目标值，实际大小随画面变化。较低码率减少文件大小和细节。系统声音不包含麦克风。"), dialog);
    hint->setWordWrap(true);
    form->addRow(hint);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    form->addRow(buttons);
    connect(dialog, &QDialog::accepted, this, [this, fps, bitrate, audio, hardware, cursor] {
        RecordingOptions options;
        options.fps = fps->value();
        options.bitrate = static_cast<int>(python_round(bitrate->value() * 1000000));
        options.system_audio = audio->isChecked();
        options.hardware = hardware->isChecked();
        options.cursor = cursor->isChecked();
        options.validate();
        video_options_ = options;
        video_options_.save_to_config(config_);
        refresh_recording_options();
        emit fps_changed(video_options_.fps);
    });
    connect(dialog, &QDialog::finished, this, [this, dialog] {
        if (video_dialog_ == dialog) video_dialog_ = nullptr;
        dialog->deleteLater();
    });
    dialog->open();
}

// ── Recording ──

void RecordToolbar::on_record_clicked() {
    if (!recording_) {
        hide_all_panels();
        recording_ = true;
        set_button_icon(record_btn_, QString::fromUtf8("结束录制.svg"));
        record_btn_->setToolTip(tr_text("停止录制"));
        pause_btn_->setEnabled(true);
        set_button_icon(pause_btn_, QString::fromUtf8("暂停录制.svg"));
        pause_btn_->setToolTip(tr_text("暂停录制"));
        fps_btn_->set_enabled(false);
        mode_btn_->set_enabled(false);
        video_settings_btn_->setEnabled(false);
        audio_btn_->setEnabled(false);
        cursor_btn_->setEnabled(false);
        time_label_->setEnabled(false);
        move_handle_->setEnabled(false);
        move_handle_->setCursor(QCursor(Qt::ArrowCursor));
        move_handle_->setToolTip(tr_text("录制中无法移动"));
        emit record_start();
    } else {
        recording_ = false;
        paused_ = false;
        set_button_icon(record_btn_, QString::fromUtf8("开始录制.svg"));
        record_btn_->setToolTip(tr_text("开始录制"));
        pause_btn_->setEnabled(false);
        set_button_icon(pause_btn_, QString::fromUtf8("暂停不可.svg"));
        pause_btn_->setToolTip(tr_text("暂停录制"));
        fps_btn_->set_enabled(true);
        mode_btn_->set_enabled(true);
        video_settings_btn_->setEnabled(true);
        audio_btn_->setEnabled(true);
        cursor_btn_->setEnabled(true);
        time_label_->setEnabled(true);
        move_handle_->setEnabled(true);
        move_handle_->setCursor(QCursor(Qt::SizeAllCursor));
        move_handle_->setToolTip("");
        emit record_stop();
    }
}

void RecordToolbar::on_pause_clicked() {
    // MP4 button states follow the worker's confirmation; nothing shows paused while the request is pending.
    if (mode_ == "mp4") {
        emit pause_toggled(!paused_);
        return;
    }
    paused_ = !paused_;
    if (paused_) {
        set_button_icon(pause_btn_, QString::fromUtf8("重开录制.svg"));
        pause_btn_->setToolTip(tr_text("恢复录制"));
    } else {
        set_button_icon(pause_btn_, QString::fromUtf8("暂停录制.svg"));
        pause_btn_->setToolTip(tr_text("暂停录制"));
    }
    emit pause_toggled(paused_);
}

// ── Dragging ──

bool RecordToolbar::eventFilter(QObject *watched, QEvent *event) {
    if (watched == move_handle_) {
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto *ev = static_cast<QMouseEvent *>(event);
            if (ev->button() == Qt::LeftButton) drag_pos_ = ev->globalPosition().toPoint();
            return true;
        }
        case QEvent::MouseMove:
            if (drag_pos_) {
                const QPoint new_pos = static_cast<QMouseEvent *>(event)->globalPosition().toPoint();
                const QPoint delta = new_pos - *drag_pos_;
                drag_pos_ = new_pos;
                emit move_requested(delta);
            }
            return true;
        case QEvent::MouseButtonRelease:
            drag_pos_.reset();
            emit drag_ended();
            return true;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void RecordToolbar::on_fps_selected(const QVariant &value) {
    if (mode_ == "mp4") {
        if (!value.isValid()) {
            if (fps_dialog_) {
                fps_dialog_->raise();
                return;
            }
            auto *dialog = new QInputDialog(this);
            fps_dialog_ = dialog;
            dialog->setWindowTitle(tr_text("自定义帧率"));
            dialog->setLabelText(tr_text("帧率（1–60）"));
            dialog->setInputMode(QInputDialog::IntInput);
            dialog->setIntRange(1, 60);
            dialog->setIntValue(video_options_.fps);
            connect(dialog, &QDialog::accepted, this, [this, dialog] { on_fps_selected(dialog->intValue()); });
            connect(dialog, &QDialog::finished, this, [this, dialog] {
                if (fps_dialog_ == dialog) fps_dialog_ = nullptr;
                dialog->deleteLater();
            });
            // The menu's "custom" entry is not a real frame rate; cancelling keeps the current one.
            refresh_recording_options();
            dialog->open();
            return;
        }
        video_options_.fps = value.toInt();
        video_options_.save_to_config(config_);
        refresh_recording_options();
        emit fps_changed(video_options_.fps);
        return;
    }
    const int fps = value.toInt();
    gif_fps_ = fps;
    emit fps_changed(fps);
    try {
        config_->set_gif_fps(fps);
    } catch (const std::exception &e) {
        log_exception(e, T("保存 GIF 帧率"));
    }
}

void RecordToolbar::hide() {
    for (QDialog *dialog : {static_cast<QDialog *>(video_dialog_.data()), static_cast<QDialog *>(fps_dialog_.data())})
        if (dialog) dialog->reject();
    hide_all_panels();
    QWidget::hide();
}
}
