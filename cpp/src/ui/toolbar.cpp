#include "ui/toolbar.h"
#include "canvas/canvas_host.h"
#include "canvas/view.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "core/resource_manager.h"
#include "core/theme.h"
#include "core/ui_scale.h"
#include "settings/tool_settings.h"
#include "tools/base.h"
#include "tools/cursor_manager.h"
#include "tools/number.h"
#include "tools/watermark.h"
#include "ui/arrow_settings_panel.h"
#include "ui/base_settings_panel.h"
#include "ui/dialogs.h"
#include "ui/fluent_lite/icons.h"
#include "ui/mosaic_settings_panel.h"
#include "ui/number_settings_panel.h"
#include "ui/paint_settings_panel.h"
#include "ui/shape_settings_panel.h"
#include "ui/text_settings_panel.h"
#include "ui/toolbar_layout.h"
#include "ui/toolbar_layout_dialog.h"
#include "ui/watermark_input_dialog.h"
#include "ui/watermark_settings_panel.h"
#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>

namespace jietuba {
namespace {
QString toolbar_tr(const char *text) { return i18n_tr(text, "Toolbar"); }

const QColor GRIP_DOT_COLOR(0x5F, 0x63, 0x68);
const QColor GRIP_DOT_COLOR_IDLE(0xB4, 0xB8, 0xBE);

// Three vertical dots centred in rect.
void paint_vertical_dots(QPainter &painter, const QRect &rect, const QColor &color) {
    const auto center = QRectF(rect).center();
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    const double step = scaled_f(7);
    const double dot = scaled_f(2);
    for (double dy : {-step, 0.0, step}) painter.drawEllipse(QPointF(center.x(), center.y() + dy), dot, dot);
}

QIcon cached_icon(const QString &relative_path, int fit = 0) { return ResourceManager::get_icon(ResourceManager::get_resource_path(relative_path), 0, fit); }

// Buttons move between the toolbar and the "..." popup and stylesheets cascade by
// parent, so both carry the same sheet. The margin insets hover and checked fills
// into rounded blocks clear of the toolbar edges.
QString button_qss() {
    const auto tc = get_theme().theme_color();
    return QString(R"(
        QPushButton {
            background-color: transparent;
            border: none;
            border-radius: %1px;
            margin: %2px;
            padding: 0px;
        }
        QPushButton:hover {
            background-color: rgba(0, 0, 0, 0.06);
        }
        QPushButton:pressed {
            background-color: rgba(0, 0, 0, 0.12);
        }
        QPushButton:checked {
            background-color: rgba(%3, %4, %5, 0.3);
        }
    )").arg(scaled(9)).arg(scaled(3)).arg(tc.red()).arg(tc.green()).arg(tc.blue());
}
}

const QStringList Toolbar::PANEL_TOOLS = {"pen", "highlighter", "rect", "ellipse", "arrow", "number", "text", "mosaic"};

ToolbarDragHandle::ToolbarDragHandle(QWidget *parent) : QWidget(parent) { setCursor(Qt::SizeAllCursor); }

void ToolbarDragHandle::set_manual_mode(bool manual) {
    if (manual_mode_ != manual) {
        manual_mode_ = manual;
        update();
    }
}

void ToolbarDragHandle::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && manual_mode_) emit reset_requested();
    QWidget::mouseDoubleClickEvent(event);
}

void ToolbarDragHandle::paintEvent(QPaintEvent *) {
    // No theme fill: blends with the white toolbar. Darker dots hint that double-click resets.
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    paint_vertical_dots(painter, rect(), manual_mode_ ? GRIP_DOT_COLOR : GRIP_DOT_COLOR_IDLE);
    painter.end();
}

ToolbarMoreHandle::ToolbarMoreHandle(QWidget *parent) : QAbstractButton(parent) {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
}

void ToolbarMoreHandle::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    paint_vertical_dots(painter, rect(), GRIP_DOT_COLOR);
    painter.end();
}

ToolbarMorePopup::ToolbarMorePopup(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    adjust_btn = new QPushButton(this);
    adjust_btn->setObjectName("more_adjust");
    adjust_btn->setIcon(fluent_icon(FluentIcon::Setting));
    adjust_btn->setFocusPolicy(Qt::NoFocus);
    apply_scale();
}

void ToolbarMorePopup::apply_scale() {
    setStyleSheet(button_qss() + QString(R"(
            QPushButton#more_adjust {
                color: #5F6368;
                font-size: %1px;
                padding: 0px %2px;
            }
        )").arg(scaled(BASE_ADJUST_FONT)).arg(scaled(8)));
    const int icon = scaled(BASE_ADJUST_ICON);
    adjust_btn->setIconSize(QSize(icon, icon));
}

void ToolbarMorePopup::set_buttons(const QList<QAbstractButton *> &buttons, const QSize &cell) {
    const int pad = scaled(BASE_PADDING);
    const int count = static_cast<int>(buttons.size());
    const int rows = (count + COLUMNS - 1) / COLUMNS;
    const int grid_width = std::min(count, COLUMNS) * cell.width();
    const int adjust_width = adjust_btn->sizeHint().width();
    const int adjust_height = static_cast<int>(python_round(cell.height() * 0.7));
    const int inner = std::max(grid_width, adjust_width);
    // Centred when the grid is narrower than "Adjust", so no gap is left on the right.
    const int left = pad + (inner - grid_width) / 2;
    for (int index = 0; index < count; ++index) {
        auto *button = buttons[index];
        if (button->parent() != this) button->setParent(this);
        const int row = index / COLUMNS, column = index % COLUMNS;
        button->setGeometry(left + column * cell.width(), pad + row * cell.height(), cell.width(), cell.height());
        button->show();
    }
    const int width = inner + 2 * pad;
    const int top = pad + rows * cell.height();
    adjust_btn->setGeometry(width - pad - adjust_width, top, adjust_width, adjust_height);
    resize(width, top + adjust_height + pad);
}

void ToolbarMorePopup::paintEvent(QPaintEvent *) { paint_rounded_panel(this); }

void ToolbarMorePopup::enterEvent(QEnterEvent *event) {
    QWidget::enterEvent(event);
    emit hover_changed(true);
}

void ToolbarMorePopup::leaveEvent(QEvent *event) {
    QWidget::leaveEvent(event);
    emit hover_changed(false);
}

Toolbar::Toolbar(QWidget *parent, const toolbar_layout::LayoutProfile &layout_profile)
    : QWidget(parent), layout_profile(layout_profile) { init_ui(); }

void Toolbar::init_ui() {
    // Buttons are only created here; arrange places them per layout, shared by the
    // capture (user layout) and pin windows (fixed list).
    make_floating(this);
    setAttribute(Qt::WA_TranslucentBackground, true);
    drag_handle = new ToolbarDragHandle(this);
    drag_handle->setToolTip(toolbar_tr("Drag to move"));
    // Events pass through; the toolbar handles dragging.
    drag_handle->installEventFilter(this);
    connect(drag_handle, &ToolbarDragHandle::reset_requested, this, [this] { reset_auto_position(); });
    // Tooltip sources, refreshed together after a language switch.
    tooltip_sources_.emplace_back(drag_handle, "Drag to move");

    const int wide = BASE_WIDE_WIDTH;
    const int tool = BASE_BTN_WIDTH;
    long_screenshot_btn = add_button("long_screenshot", QString::fromUtf8("svg/长截图.svg"), "Long screenshot (scroll)", wide, [this] { emit long_screenshot_clicked(); });
    save_btn = add_button("save", QString::fromUtf8("svg/下载.svg"), "Save to file", wide, [this] { emit save_clicked(); });
    screenshot_translate_btn = add_button("screenshot_translate", QString::fromUtf8("svg/翻译.svg"), "Screenshot translate (OCR + Translate)", wide,
                                          [this] { emit screenshot_translate_clicked(); });
    text_recognize_btn = add_button("text_recognize", QString::fromUtf8("svg/文字识别.svg"), "Recognize text (OCR)", wide, [this] { emit text_recognize_clicked(); });
    scan_code_btn = add_button("scan_code", QString::fromUtf8("svg/扫码.svg"), "Scan QR code / barcode", wide, [this] { emit scan_code_clicked(); });
    gif_btn = add_button("gif", "svg/gif.svg", "GIF / Video recording", wide, [this] { emit gif_record_clicked(); });
    // Copy is laid out only in pin windows; the capture layout has no copy button.
    copy_btn = add_button("copy", "svg/copy.svg", "Copy image", wide, [this] { emit copy_clicked(); });

    pen_btn = add_tool_button("pen", QString::fromUtf8("svg/画笔.svg"), "Pen tool (hold Shift for straight line)", tool);
    highlighter_btn = add_tool_button("highlighter", QString::fromUtf8("svg/荧光笔.svg"), "Highlighter (hold Shift for straight line)", tool);
    mosaic_btn = add_tool_button("mosaic", QString::fromUtf8("svg/马赛克.svg"), "Mosaic (mouse wheel to resize)", tool);
    spotlight_btn = add_tool_button("spotlight", QString::fromUtf8("svg/聚光灯.svg"), "Spotlight (dim outside the box)", tool);
    watermark_btn = add_tool_button("watermark", QString::fromUtf8("svg/水印.svg"), "Watermark", tool);
    arrow_btn = add_tool_button("arrow", QString::fromUtf8("svg/箭头.svg"), "Draw arrow", tool);
    number_btn = add_tool_button("number", QString::fromUtf8("svg/序号.svg"), "Number (Shift+scroll to change number)", tool);
    rect_btn = add_tool_button("rect", QString::fromUtf8("svg/方框.svg"), "Draw rectangle (hold Shift for square)", tool);
    ellipse_btn = add_tool_button("ellipse", QString::fromUtf8("svg/圆框.svg"), "Draw ellipse (hold Shift for circle)", tool);
    text_btn = add_tool_button("text", QString::fromUtf8("svg/文字.svg"), "Add text", tool);
    eraser_btn = add_tool_button("eraser", QString::fromUtf8("svg/橡皮.svg"), "Eraser tool", tool);

    undo_btn = add_button("undo", QString::fromUtf8("svg/撤回.svg"), "Undo", tool, [this] { emit undo_clicked(); });
    redo_btn = add_button("redo", QString::fromUtf8("svg/复原.svg"), "Redo", tool, [this] { emit redo_clicked(); });
    cancel_btn = add_button("cancel", QString::fromUtf8("svg/结束截图.svg"), "Cancel screenshot (ESC)", wide, [this] { emit cancel_clicked(); });
    pin_btn = add_button("pin", QString::fromUtf8("svg/钉图.svg"), "Pin image", wide, [this] { emit pin_clicked(); });
    confirm_btn = add_button("confirm", QString::fromUtf8("svg/确定.svg"), "Confirm and save", wide, [this] { emit confirm_clicked(); });

    // "...": hover or click opens folded buttons; registered like a button, arranged last.
    more_btn = new ToolbarMoreHandle(this);
    connect(more_btn, &QAbstractButton::clicked, this, &Toolbar::show_more_popup);
    more_btn->installEventFilter(this);
    buttons_["more"] = more_btn;
    button_order_.append("more");
    // Same width as the drag handle; the icon is the painted dots.
    base_widths_["more"] = BASE_BTN_HEIGHT * HANDLE_WIDTH_RATIO;

    // Background and border are painted; #toolbar_root stays transparent. The object
    // name must be set before the stylesheet or the selector misses.
    setObjectName("toolbar_root");
    apply_button_sizes();
    tool_buttons = {
        {"pen", pen_btn}, {"highlighter", highlighter_btn}, {"mosaic", mosaic_btn}, {"spotlight", spotlight_btn},
        {"watermark", watermark_btn}, {"arrow", arrow_btn}, {"number", number_btn}, {"rect", rect_btn},
        {"ellipse", ellipse_btn}, {"text", text_btn}, {"eraser", eraser_btn},
    };
    // Keep common panels ready: highlighter/pen, rectangle/ellipse and numbering.
    ensure_panel_for_tool("highlighter");
    ensure_panel_for_tool("rect");
    ensure_panel_for_tool("number");
    reload_layout();
    // Connections end with this toolbar.
    connect(I18nManager::instance().language_signals(), &I18nSignals::language_changed, this, [this](const QString &) { retranslate(); });
    connect(&get_ui_scale(), &UIScaleManager::scale_changed, this, &Toolbar::apply_scale);
}

QPushButton *Toolbar::add_button(const QString &key, const QString &icon, const char *tooltip, int width, std::function<void()> on_click, bool checkable) {
    // Size and icon come from apply_button_sizes; position from arrange.
    auto *button = new QPushButton(this);
    button->setToolTip(toolbar_tr(tooltip));
    button->setCheckable(checkable);
    // No keyboard focus, so Space/Enter cannot trigger buttons by accident.
    button->setFocusPolicy(Qt::NoFocus);
    // Close the popup first: the action may open a modal dialog.
    connect(button, &QPushButton::clicked, this, &Toolbar::hide_more_popup);
    connect(button, &QPushButton::clicked, this, [on_click](bool) { on_click(); });
    buttons_[key] = button;
    button_order_.append(key);
    base_widths_[key] = width;
    icon_paths_[key] = icon;
    tooltip_sources_.emplace_back(button, tooltip);
    return button;
}

void Toolbar::apply_button_sizes() {
    // Insets and radii in the button style scale too, so the whole sheet is reapplied.
    setStyleSheet("#toolbar_root { background-color: transparent; border: none; }" + button_qss());
    btn_height_ = scaled(BASE_BTN_HEIGHT);
    const int handle_w = scaled(BASE_BTN_HEIGHT * HANDLE_WIDTH_RATIO);
    drag_handle->setGeometry(0, 0, handle_w, btn_height_);
    for (const auto &key : button_order_) button_widths_[key] = scaled(base_widths_.value(key));
    const int icon_size = scaled(BASE_ICON);
    for (auto it = icon_paths_.begin(); it != icon_paths_.end(); ++it) {
        auto *button = buttons_[it.key()];
        button->setIcon(cached_icon(it.value(), icon_size));
        button->setIconSize(QSize(icon_size, icon_size));
    }
}

QPushButton *Toolbar::add_tool_button(const QString &tool_id, const QString &icon, const char *tooltip, int width) {
    return add_button(tool_id, icon, tooltip, width, [this, tool_id] { on_tool_clicked(tool_id); }, true);
}

void Toolbar::make_floating(QWidget *widget) const {
    // In a capture these are children of the capture window; a pin toolbar has no
    // parent and is a separate topmost tool window that never takes focus.
    if (!parent()) {
        widget->setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool | Qt::X11BypassWindowManagerHint);
        widget->setAttribute(Qt::WA_ShowWithoutActivating);
    } else {
        widget->setWindowFlags(Qt::FramelessWindowHint);
    }
    widget->setFocusPolicy(Qt::NoFocus);
    widget->setAttribute(Qt::WA_StyledBackground, true);
}

void Toolbar::arrange(const QStringList &keys) {
    for (const auto &key : button_order_)
        if (!keys.contains(key)) buttons_[key]->hide();
    int x = drag_handle->width();
    for (const auto &key : keys) {
        // A subclass's own keys have no button yet while the base constructor arranges.
        auto *button = buttons_.value(key);
        if (!button) continue;
        // Just moved back from the "..." popup.
        if (button->parent() != this) button->setParent(this);
        const int width = button_widths_[key];
        button->setGeometry(x, 0, width, btn_height_);
        button->show();
        x += width;
    }
    resize(x, btn_height_);
}

void Toolbar::reload_layout() {
    const auto layout = toolbar_layout::load_layout(*get_tool_settings_manager(), layout_profile);
    folded_keys_.clear();
    QStringList shown;
    for (const auto &value : layout) {
        const auto entry = value.toArray();
        const auto key = entry[0].toString(), mode = entry[1].toString();
        if (mode == toolbar_layout::MORE) folded_keys_.append(key);
        if (mode == toolbar_layout::SHOW) shown.append(key);
    }
    shown.append("more");
    arrange(shown);
}

QList<QWidget *> Toolbar::panels() const {
    QList<QWidget *> result;
    for (QWidget *panel : std::initializer_list<QWidget *>{paint_panel, shape_panel, arrow_panel, number_panel, text_panel, mosaic_panel, watermark_panel})
        if (panel) result.append(panel);
    return result;
}

void Toolbar::apply_scale() {
    // Visible instances update now; hidden or reused ones are rescaled by the host before showing.
    hide_more_popup();
    apply_button_sizes();
    reload_layout();
    if (more_popup_) more_popup_->apply_scale();
    if (paint_panel) paint_panel->apply_scale();
    if (shape_panel) shape_panel->apply_scale();
    if (arrow_panel) arrow_panel->apply_scale();
    if (number_panel) number_panel->apply_scale();
    if (text_panel) text_panel->apply_scale();
    if (mosaic_panel) mosaic_panel->apply_scale();
    if (watermark_panel) watermark_panel->apply_scale();
    reposition_self();
    sync_all_panels_position();
}

void Toolbar::reposition_self() {
    if (auto *host = dynamic_cast<CanvasHost *>(host_window())) host->update_toolbar_position();
}

void Toolbar::show_more_popup() {
    // Created on first use: most captures and pins never open it, so they skip the extra window.
    if (!more_popup_) {
        more_popup_ = new ToolbarMorePopup(parentWidget());
        make_floating(more_popup_);
        more_popup_->adjust_btn->setText(toolbar_tr("Adjust"));
        connect(more_popup_->adjust_btn, &QPushButton::clicked, this, &Toolbar::open_layout_dialog);
        connect(more_popup_, &ToolbarMorePopup::hover_changed, this, &Toolbar::on_more_hover);
        more_close_timer_ = new QTimer(this);
        more_close_timer_->setSingleShot(true);
        more_close_timer_->setInterval(300);
        connect(more_close_timer_, &QTimer::timeout, this, &Toolbar::hide_more_popup);
    }
    more_close_timer_->stop();
    if (more_popup_->isVisible()) return;
    int cell_width = 0;
    QList<QAbstractButton *> folded;
    for (const auto &key : folded_keys_) {
        cell_width = std::max(cell_width, button_widths_[key]);
        folded.append(buttons_[key]);
    }
    more_popup_->set_buttons(folded, QSize(cell_width, btn_height_));
    sync_panel_position(more_popup_, true);
    more_popup_->show();
    more_popup_->raise();
}

void Toolbar::on_more_hover(bool hovering) {
    if (hovering) show_more_popup();
    else if (more_popup_) more_close_timer_->start();
}

void Toolbar::hide_more_popup() {
    if (more_popup_) {
        more_close_timer_->stop();
        more_popup_->hide();
    }
}

void Toolbar::open_layout_dialog() {
    // "Adjust": edit the layout, then save and rearrange at once.
    hide_more_popup();
    auto *host = host_window();
    QMap<QString, QIcon> icons;
    for (const auto &key : button_order_) icons[key] = buttons_[key]->icon();
    ToolbarLayoutDialog dialog(toolbar_layout::load_layout(*get_tool_settings_manager(), layout_profile), icons, host, layout_profile);
    // The capture window spans the virtual desktop, so its centre may be on another
    // screen; centre on the toolbar's screen instead.
    auto *screen = QApplication::screenAt(mapToGlobal(QPoint(0, 0)));
    if (!screen) screen = QApplication::primaryScreen();
    dialog.move(screen->availableGeometry().center() - dialog.rect().center());
    if (!exec_dialog(&dialog)) return;
    toolbar_layout::save_layout(*get_tool_settings_manager(), dialog.entries(), layout_profile);
    reload_layout();
    // The width changed; re-anchor (positions moved by hand stay).
    reposition_self();
}

QWidget *Toolbar::ensure_panel_for_tool(const QString &tool_id) {
    QString kind;
    if (tool_id == "pen" || tool_id == "highlighter" || tool_id == "spotlight") kind = "paint";
    else if (tool_id == "rect" || tool_id == "ellipse") kind = "shape";
    else if (tool_id == "watermark") return ensure_watermark_panel();
    else kind = tool_id;
    auto *parent = parentWidget();
    bool created = false;
    if (kind == "paint" && !paint_panel) {
        paint_panel = new PaintSettingsPanel(parent);
        make_floating(paint_panel);
        connect(paint_panel, &BaseSettingsPanel::color_changed, this, &Toolbar::on_panel_color_changed);
        connect(paint_panel, &BaseSettingsPanel::size_changed, this, &Toolbar::stroke_width_changed);
        connect(paint_panel, &BaseSettingsPanel::opacity_changed, this, &Toolbar::opacity_changed);
        connect(paint_panel, &PaintSettingsPanel::line_style_changed, this, &Toolbar::on_line_style_changed);
        connect(paint_panel, &PaintSettingsPanel::highlighter_mode_changed, this, &Toolbar::on_highlighter_mode_changed);
        // Start with the last pen line style.
        if (auto *pen_settings = get_tool_settings_manager()->get_tool_settings("pen")) paint_panel->set_line_style(pen_settings->get("line_style", "solid").toString());
        paint_panel->hide();
        created = true;
    }
    if (kind == "shape" && !shape_panel) {
        shape_panel = new ShapeSettingsPanel(parent);
        make_floating(shape_panel);
        connect(shape_panel, &BaseSettingsPanel::color_changed, this, &Toolbar::on_panel_color_changed);
        connect(shape_panel, &BaseSettingsPanel::size_changed, this, &Toolbar::stroke_width_changed);
        connect(shape_panel, &BaseSettingsPanel::opacity_changed, this, &Toolbar::opacity_changed);
        connect(shape_panel, &ShapeSettingsPanel::line_style_changed, this, &Toolbar::on_line_style_changed);
        shape_panel->hide();
        created = true;
    }
    if (kind == "arrow" && !arrow_panel) {
        arrow_panel = new ArrowSettingsPanel(parent);
        make_floating(arrow_panel);
        connect(arrow_panel, &ArrowSettingsPanel::color_changed, this, &Toolbar::on_panel_color_changed);
        connect(arrow_panel, &ArrowSettingsPanel::size_changed, this, &Toolbar::stroke_width_changed);
        connect(arrow_panel, &ArrowSettingsPanel::opacity_changed, this, &Toolbar::opacity_changed);
        connect(arrow_panel, &ArrowSettingsPanel::arrow_style_changed, this, &Toolbar::on_arrow_style_changed);
        arrow_panel->hide();
        created = true;
    }
    if (kind == "number" && !number_panel) {
        number_panel = new NumberSettingsPanel(parent);
        // The panel's Qt parent is the toolbar's parent; its style popup uses the owner
        // toolbar to open away from the toolbar and panel.
        number_panel->setProperty("owner_toolbar", QVariant::fromValue<QWidget *>(this));
        make_floating(number_panel);
        connect(number_panel, &BaseSettingsPanel::color_changed, this, &Toolbar::on_panel_color_changed);
        connect(number_panel, &BaseSettingsPanel::size_changed, this, &Toolbar::stroke_width_changed);
        connect(number_panel, &BaseSettingsPanel::opacity_changed, this, &Toolbar::opacity_changed);
        connect(number_panel, &NumberSettingsPanel::next_number_changed, this, &Toolbar::on_number_next_changed);
        connect(number_panel, &NumberSettingsPanel::style_changed, this, &Toolbar::number_style_changed);
        number_panel->hide();
        created = true;
    }
    if (kind == "text" && !text_panel) {
        text_panel = new TextSettingsPanel(parent);
        // Background/outline/shadow popups open away from the toolbar too.
        text_panel->setProperty("owner_toolbar", QVariant::fromValue<QWidget *>(this));
        make_floating(text_panel);
        connect(text_panel, &TextSettingsPanel::font_changed, this, &Toolbar::on_text_font_changed);
        connect(text_panel, &TextSettingsPanel::color_changed, this, &Toolbar::on_text_color_changed);
        connect(text_panel, &TextSettingsPanel::background_changed, this, &Toolbar::on_text_background_changed);
        connect(text_panel, &TextSettingsPanel::outline_changed, this, &Toolbar::on_text_outline_changed);
        connect(text_panel, &TextSettingsPanel::shadow_changed, this, &Toolbar::on_text_shadow_changed);
        text_panel->hide();
        created = true;
    }
    if (kind == "mosaic" && !mosaic_panel) {
        mosaic_panel = new MosaicSettingsPanel(parent);
        make_floating(mosaic_panel);
        connect(mosaic_panel, &MosaicSettingsPanel::size_changed, this, &Toolbar::stroke_width_changed);
        connect(mosaic_panel, &MosaicSettingsPanel::draw_mode_changed, this, &Toolbar::on_mosaic_mode_changed);
        connect(mosaic_panel, &MosaicSettingsPanel::style_changed, this, &Toolbar::on_mosaic_style_changed);
        connect(mosaic_panel, &MosaicSettingsPanel::block_size_changed, this, &Toolbar::on_mosaic_block_size_changed);
        mosaic_panel->hide();
        created = true;
    }
    if (created) {
        set_temporary_edit_active(temporary_edit_active);
        reposition_self();
    }
    if (kind == "paint") return paint_panel;
    if (kind == "shape") return shape_panel;
    if (kind == "arrow") return arrow_panel;
    if (kind == "number") return number_panel;
    if (kind == "text") return text_panel;
    if (kind == "mosaic") return mosaic_panel;
    return nullptr;
}

WatermarkSettingsPanel *Toolbar::ensure_watermark_panel() {
    if (!watermark_panel) {
        auto *panel = new WatermarkSettingsPanel(parentWidget());
        panel->setProperty("owner_toolbar", QVariant::fromValue<QWidget *>(this));
        make_floating(panel);
        connect(panel, &WatermarkSettingsPanel::style_changed, this, &Toolbar::watermark_style_changed);
        connect(panel, &WatermarkSettingsPanel::text_edit_requested, this, &Toolbar::edit_watermark_text);
        connect(panel, &WatermarkSettingsPanel::text_cleared, this, [this] { set_watermark_text(""); });
        panel->hide();
        watermark_panel = panel;
    }
    return watermark_panel;
}

void Toolbar::edit_watermark_text() {
    // Cancelling changes nothing.
    auto *panel = ensure_watermark_panel();
    if (const auto template_text = jietuba::edit_watermark_text(panel->text_field, get_tool_settings_manager())) set_watermark_text(*template_text);
}

void Toolbar::set_watermark_text(const QString &template_text) {
    if (watermark_panel) watermark_panel->set_template(template_text);
    emit watermark_text_changed(template_text);
}

void Toolbar::show_watermark(const QString &template_text, const QDateTime &when) {
    if (watermark_panel) watermark_panel->set_template(template_text, when);
}

void Toolbar::paintEvent(QPaintEvent *) { paint_rounded_panel(this); }

void Toolbar::sync_panel_from_settings(const QString &tool_id) {
    ensure_panel_for_tool(tool_id);
    // One place for startup, tool switches, state restores and lone panels: callers
    // decide when, not what. No defaults here: ToolSettings::get falls back to
    // DEFAULT_SETTINGS and the panel setters normalize again.
    auto *manager = get_tool_settings_manager();
    auto *settings = manager ? manager->get_tool_settings(tool_id) : nullptr;
    if (!settings) return;
    if (tool_id == "text") {
        // The text panel has its own complete loader.
        text_panel->load_from_config();
    } else if (tool_id == "pen" || tool_id == "highlighter") {
        paint_panel->set_line_style(settings->get("line_style").toString());
        if (tool_id == "highlighter") paint_panel->set_highlighter_mode(settings->get("draw_mode").toString());
    } else if (tool_id == "rect" || tool_id == "ellipse") {
        shape_panel->set_line_style(settings->get("line_style").toString());
    } else if (tool_id == "arrow") {
        arrow_panel->set_arrow_style(settings->get("arrow_style").toString());
    } else if (tool_id == "number") {
        number_panel->set_style(settings->get("style").toString());
    } else if (tool_id == "mosaic") {
        mosaic_panel->set_draw_mode(settings->get("draw_mode").toString());
        mosaic_panel->set_style(settings->get("style").toString());
        mosaic_panel->set_block_size(settings->get("block_size"));
    } else if (tool_id == "watermark") {
        auto *panel = ensure_watermark_panel();
        panel->load_style(WatermarkTool::load_style(manager));
        panel->set_template(WatermarkTool::current_template(manager));
    }
}

void Toolbar::set_temporary_edit_active(bool active) {
    temporary_edit_active = active;
    const bool enabled = !temporary_edit_active;
    if (paint_panel && paint_panel->mode_widget) paint_panel->mode_widget->setEnabled(enabled);
    if (number_panel) {
        for (auto *button : {number_panel->next_up_btn, number_panel->next_down_btn})
            if (button) button->setEnabled(enabled);
    }
}

void Toolbar::retranslate() {
    for (const auto &[widget, source] : tooltip_sources_)
        if (widget) widget->setToolTip(toolbar_tr(source.toUtf8().constData()));
    // Panel texts are set once at construction, so they are refreshed here.
    if (mosaic_panel) mosaic_panel->retranslate();
    if (text_panel) text_panel->retranslate();
    if (watermark_panel) watermark_panel->retranslate();
    if (more_popup_) more_popup_->adjust_btn->setText(toolbar_tr("Adjust"));
}

void Toolbar::restore_active_tool_state(const QString &tool_id, const ToolContext *ctx, QGraphicsScene *scene) {
    set_temporary_edit_active(false);
    if (tool_id == "cursor" || tool_id.isEmpty()) {
        hide_all_panels();
        return;
    }
    ensure_panel_for_tool(tool_id);
    if (ctx) {
        if (ctx->color.isValid()) set_current_color(ctx->color);
        set_stroke_width(static_cast<int>(python_round(ctx->stroke_width)));
        set_opacity(static_cast<int>(python_round(ctx->opacity * 255)));
    }
    // Panels are refilled from saved settings by show_panel_for_tool; only the next
    // number is added here, since it is scene state rather than a tool default.
    if (tool_id == "number" && scene) set_number_next_value(NumberTool::get_next_number(scene));
    show_panel_for_tool(tool_id);
}

void Toolbar::on_tool_clicked(const QString &tool_id) {
    if (tool_id == "watermark") {
        on_watermark_clicked();
        return;
    }
    select_tool(tool_id, true);
}

void Toolbar::on_watermark_clicked() {
    // Removal is the panel's ×, so closing the panel never removes the watermark too.
    // Without watermark text yet, the input dialog opens right after applying.
    if (current_tool == QString("watermark")) {
        select_tool("cursor");
        return;
    }
    select_tool("watermark");
    emit watermark_requested();
    if (ensure_watermark_panel()->text_field->template_text().isEmpty()) edit_watermark_text();
}

void Toolbar::select_tool(const QString &tool_id, bool toggle) {
    if (tool_id == "cursor") {
        set_temporary_edit_active(false);
        for (auto *button : tool_buttons) button->setChecked(false);
        current_tool.reset();
        emit tool_changed("cursor");
        hide_all_panels();
        return;
    }
    if (!tool_buttons.contains(tool_id)) return;
    if (current_tool == tool_id) {
        if (toggle) select_tool("cursor");
        return;
    }
    set_temporary_edit_active(false);
    for (auto it = tool_buttons.begin(); it != tool_buttons.end(); ++it) it.value()->setChecked(it.key() == tool_id);
    ensure_panel_for_tool(tool_id);
    current_tool = tool_id;
    emit tool_changed(tool_id);
    show_panel_for_tool(tool_id);
}

void Toolbar::hide_all_panels() {
    for (auto *panel : panels()) panel->hide();
}

void Toolbar::show_panel_for_tool(const QString &tool_id) {
    // Panels are shared, so state left by the previous tool or selected item is
    // overwritten here with what drawing now would produce. The panel that stays is not
    // hidden first: a separate top-level window hidden and shown again flickers.
    QWidget *const target = ensure_panel_for_tool(tool_id);
    for (QWidget *panel : panels())
        if (panel != target) panel->hide();
    // Which controls show follows the tool, independent of saved settings.
    if ((tool_id == "pen" || tool_id == "highlighter" || tool_id == "spotlight") && paint_panel) {
        paint_panel->set_line_style_visible(tool_id == "pen");
        paint_panel->set_highlighter_mode_visible(tool_id == "highlighter");
        // The spotlight borrows this panel: opacity is the curtain's darkness; the
        // curtain is always black and the hole has no width.
        paint_panel->set_size_visible(tool_id != "spotlight");
        paint_panel->set_color_visible(tool_id != "spotlight");
    }
    sync_panel_from_settings(tool_id);
    if (target) {
        target->show();
        target->raise();
        sync_panel_position(target);
        // Keeps the toolbar above its panel.
        raise();
    }
}

void Toolbar::on_panel_color_changed(const QColor &color) {
    current_color = color;
    emit color_changed(color);
}

CursorManager *Toolbar::host_cursor_manager() const {
    // The cursor previews what will be drawn, so drawing setting changes refresh it.
    auto *host = dynamic_cast<CanvasHost *>(host_window());
    auto *view = host ? host->canvas_view() : nullptr;
    return view ? view->cursor_manager.get() : nullptr;
}

void Toolbar::on_highlighter_mode_changed(const QString &mode) {
    emit highlighter_mode_changed(mode);
    if (temporary_edit_active) return;
    get_tool_settings_manager()->update_settings("highlighter", {{"draw_mode", mode}});
    // Rect mode uses the crosshair cursor.
    if (auto *cursor_manager = host_cursor_manager()) cursor_manager->set_tool_cursor("highlighter", true);
}

void Toolbar::on_text_font_changed(const QFont &font) {
    emit text_font_changed(font);
    if (temporary_edit_active) return;
    TextSettingsPanel::save_font_to_config(font);
}

void Toolbar::on_text_color_changed(const QColor &color) {
    current_color = color;
    emit color_changed(color);
    emit text_color_changed(color);
    if (temporary_edit_active) return;
    get_tool_settings_manager()->update_settings("text", {{"color", color.name()}});
}

void Toolbar::on_text_background_changed(bool enabled, const QColor &color, int opacity) {
    emit text_background_changed(enabled, color, opacity);
    if (temporary_edit_active) return;
    TextSettingsPanel::save_background_to_config(enabled, color, opacity);
}

void Toolbar::on_text_outline_changed(bool enabled, const QColor &color, double width) {
    emit text_outline_changed(enabled, color, width);
    if (temporary_edit_active) return;
    TextSettingsPanel::save_outline_to_config(enabled, color, width);
}

void Toolbar::on_text_shadow_changed(bool enabled, const QColor &color) {
    emit text_shadow_changed(enabled, color);
    if (temporary_edit_active) return;
    TextSettingsPanel::save_shadow_to_config(enabled, color);
}

void Toolbar::on_arrow_style_changed(const QString &style) {
    if (!temporary_edit_active) get_tool_settings_manager()->update_settings("arrow", {{"arrow_style", style}});
    // The capture or pin window updates the selected arrow.
    emit arrow_style_changed(style);
}

void Toolbar::on_line_style_changed(const QString &style) {
    if (!temporary_edit_active) {
        const auto tool_id = current_tool.value_or("pen");
        if (tool_id == "rect" || tool_id == "ellipse") get_tool_settings_manager()->update_settings(tool_id, {{"line_style", style}});
        else if (tool_id == "pen") get_tool_settings_manager()->update_settings("pen", {{"line_style", style}});
    }
    emit line_style_changed(style);
}

void Toolbar::on_mosaic_mode_changed(const QString &mode) {
    if (temporary_edit_active) return;
    get_tool_settings_manager()->update_settings("mosaic", {{"draw_mode", mode}});
    // Rect mode uses the crosshair cursor.
    if (auto *cursor_manager = host_cursor_manager()) cursor_manager->set_tool_cursor("mosaic", true);
}

void Toolbar::on_mosaic_style_changed(const QString &style) {
    if (!temporary_edit_active) get_tool_settings_manager()->update_settings("mosaic", {{"style", style}});
    // The window also switches a selected rect mosaic.
    emit mosaic_style_changed(style);
}

void Toolbar::on_mosaic_block_size_changed(int value) {
    if (!temporary_edit_active) get_tool_settings_manager()->update_settings("mosaic", {{"block_size", value}});
    // The window also re-reduces a selected mosaic.
    emit mosaic_block_size_changed(value);
}

void Toolbar::on_number_next_changed(int value) {
    if (temporary_edit_active) return;
    emit number_next_changed(value);
}

void Toolbar::set_current_color(const QColor &color) {
    current_color = color;
    if (paint_panel) paint_panel->set_color(color);
    if (shape_panel) shape_panel->set_color(color);
    if (arrow_panel) arrow_panel->set_color(color);
    if (number_panel) number_panel->set_color(color);
}

void Toolbar::set_stroke_width(int width) {
    if (paint_panel) paint_panel->set_size(width);
    if (shape_panel) shape_panel->set_size(width);
    if (arrow_panel) arrow_panel->set_size(width);
    if (number_panel) number_panel->set_size(width);
    if (mosaic_panel) mosaic_panel->set_size(width);
}

void Toolbar::set_opacity(int opacity_255) {
    if (paint_panel) paint_panel->set_opacity(opacity_255);
    if (shape_panel) shape_panel->set_opacity(opacity_255);
    if (arrow_panel) arrow_panel->set_opacity(opacity_255);
    if (number_panel) number_panel->set_opacity(opacity_255);
}

void Toolbar::set_number_next_value(int value) {
    if (number_panel) number_panel->set_next_number(value);
}

void Toolbar::position_near_rect(const QRectF &rect, QWidget *parent_widget) {
    // Moved by hand: no automatic snapping back.
    if (manual_positioned_) return;
    QRect global_rect;
    if (parent_widget) {
        const auto tl = parent_widget->mapToGlobal(QPoint(static_cast<int>(rect.x()), static_cast<int>(rect.y())));
        const auto br = parent_widget->mapToGlobal(QPoint(static_cast<int>(rect.right()), static_cast<int>(rect.bottom())));
        global_rect = QRect(tl, br);
    } else {
        global_rect = rect.toRect();
    }
    const int toolbar_w = width();
    const int toolbar_h = height();
    // Toolbar plus its tallest panel must fit as a whole.
    const int panel_extra = max_panel_height();
    const auto screen_rect = screen_by_center(global_rect)->geometry();
    const int margin = scaled(10);
    // Below, aligned to the right edge of "Confirm" rather than the whole toolbar, so
    // the pointer still lands on "Confirm" on release.
    const int more_w = button_widths_.value("more", 0);
    int x = global_rect.right() - toolbar_w + more_w + scaled(BASE_RIGHT_NUDGE);
    int y = global_rect.bottom() + margin;
    bool toolbar_below = true;
    if (y + toolbar_h + panel_extra > screen_rect.bottom()) {
        y = global_rect.top() - toolbar_h - margin;
        toolbar_below = false;
    }
    y = std::max(screen_rect.top(), std::min(y, screen_rect.bottom() - toolbar_h));
    // Screen edges stop the toolbar.
    x = std::max(screen_rect.left(), std::min(x, screen_rect.right() - toolbar_w));
    // Panels open on the same side.
    toolbar_below_selection_ = toolbar_below;
    QPoint final_pos(x, y);
    if (parentWidget()) final_pos = parentWidget()->mapFromGlobal(final_pos);
    move(final_pos);
}

QScreen *Toolbar::screen_by_center(const QRect &rect) const {
    if (auto *screen = QApplication::screenAt(rect.center())) return screen;
    return QApplication::primaryScreen();
}

int Toolbar::max_panel_height() const {
    const int gap = scaled(5);
    int max_h = 0;
    for (auto *panel : panels()) max_h = std::max(max_h, panel->sizeHint().height());
    return max_h ? max_h + gap : 0;
}

void Toolbar::set_manual_positioned(bool manual) {
    manual_positioned_ = manual;
    if (drag_handle) drag_handle->set_manual_mode(manual);
}

void Toolbar::reset_auto_position() {
    set_manual_positioned(false);
    // The capture toolbar repositions through update_toolbar_position.
    if (auto *host = dynamic_cast<CanvasHost *>(parentWidget())) host->update_toolbar_position();
}

bool Toolbar::eventFilter(QObject *watched, QEvent *event) {
    // Drag handle mouse events are handled here (double-click passes through); hovering "..." opens the popup.
    if (watched == more_btn && (event->type() == QEvent::Enter || event->type() == QEvent::Leave)) {
        on_more_hover(event->type() == QEvent::Enter);
        return false;
    }
    if (drag_handle && watched == drag_handle) {
        const auto type = event->type();
        if (type == QEvent::MouseButtonDblClick) return false;
        if (type == QEvent::MouseButtonPress) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton) {
                dragging_ = true;
                drag_offset_ = mouse->globalPosition().toPoint() - frameGeometry().topLeft();
                drag_handle->setCursor(Qt::ClosedHandCursor);
            }
            return true;
        }
        if (type == QEvent::MouseMove) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->buttons() == Qt::LeftButton && dragging_) move(mouse->globalPosition().toPoint() - drag_offset_);
            return true;
        }
        if (type == QEvent::MouseButtonRelease) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton) {
                if (dragging_) set_manual_positioned(true);
                dragging_ = false;
                drag_handle->setCursor(Qt::SizeAllCursor);
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void Toolbar::moveEvent(QMoveEvent *event) {
    QWidget::moveEvent(event);
    sync_all_panels_position();
}

void Toolbar::hideEvent(QHideEvent *event) {
    QWidget::hideEvent(event);
    hide_more_popup();
}

void Toolbar::sync_all_panels_position() {
    for (auto *panel : panels())
        if (panel->isVisible()) sync_panel_position(panel);
    if (more_popup_ && more_popup_->isVisible()) sync_panel_position(more_popup_, true);
}

void Toolbar::sync_panel_position(QWidget *panel, bool align_right) {
    if (!panel) return;
    const auto toolbar_global_pos = mapToGlobal(QPoint(0, 0));
    const int gap = scaled(5);
    const int panel_h = panel->height();
    const int toolbar_h = height();
    auto *screen = QApplication::screenAt(toolbar_global_pos);
    if (!screen) screen = QApplication::primaryScreen();
    const auto screen_rect = screen->geometry();
    const int below_y = toolbar_global_pos.y() + toolbar_h + gap;
    const int above_y = toolbar_global_pos.y() - panel_h - gap;
    const bool below_ok = below_y + panel_h <= screen_rect.bottom();
    const bool above_ok = above_y >= screen_rect.top();
    int panel_y;
    if (toolbar_below_selection_) {
        // Toolbar below the selection: below first, else above, else clamped to the bottom.
        if (below_ok) panel_y = below_y;
        else if (above_ok) panel_y = above_y;
        else panel_y = screen_rect.bottom() - panel_h;
    } else {
        if (above_ok) panel_y = above_y;
        else if (below_ok) panel_y = below_y;
        else panel_y = screen_rect.top();
    }
    int panel_x = toolbar_global_pos.x();
    if (align_right) panel_x += width() - panel->width();
    const int edge = scaled(5);
    if (panel_x + panel->width() > screen_rect.right()) panel_x = screen_rect.right() - panel->width() - edge;
    if (panel_x < screen_rect.left()) panel_x = screen_rect.left() + edge;
    QPoint final_pos(panel_x, panel_y);
    if (panel->parentWidget()) final_pos = panel->parentWidget()->mapFromGlobal(final_pos);
    panel->move(final_pos);
}
}
