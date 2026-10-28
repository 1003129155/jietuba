#pragma once
#include "ui/toolbar_layout.h"
#include <QAbstractButton>
#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QHash>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QWidget>
#include <optional>

class QGraphicsScene;
class QScreen;

namespace jietuba {
class ArrowSettingsPanel;
class CursorManager;
class MosaicSettingsPanel;
class NumberSettingsPanel;
class PaintSettingsPanel;
class ShapeSettingsPanel;
class TextSettingsPanel;
class WatermarkSettingsPanel;
struct ToolContext;

// Drag grip at the toolbar's left end, the same vertical grey dots as the "..." on the right.
// Light dots while positioned automatically; dark once moved by hand (double-click resets).
class ToolbarDragHandle : public QWidget {
    Q_OBJECT
public:
    explicit ToolbarDragHandle(QWidget *parent = nullptr);
    void set_manual_mode(bool manual);
signals:
    void reset_requested();
protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
private:
    bool manual_mode_ = false;
};

// The "..." at the toolbar's right end. No tooltip: hovering opens the popup at once,
// and a system tooltip would cover it.
class ToolbarMoreHandle : public QAbstractButton {
    Q_OBJECT
public:
    explicit ToolbarMoreHandle(QWidget *parent = nullptr);
protected:
    void paintEvent(QPaintEvent *event) override;
};

// The "..." popup holding folded buttons, with an "Adjust" entry at the bottom. The
// toolbar's own buttons are reparented here, so connections, checked state and the
// tool_buttons map follow them.
class ToolbarMorePopup : public QWidget {
    Q_OBJECT
public:
    static constexpr int COLUMNS = 5;
    static constexpr int BASE_PADDING = 4;
    static constexpr int BASE_ADJUST_ICON = 14;
    static constexpr int BASE_ADJUST_FONT = 12;
    explicit ToolbarMorePopup(QWidget *parent = nullptr);
    // The grid is recomputed with the new cell size on the next set_buttons.
    void apply_scale();
    // COLUMNS buttons per row in cells of cell size; "Adjust" below the grid, right-aligned.
    void set_buttons(const QList<QAbstractButton *> &buttons, const QSize &cell);
    QPushButton *adjust_btn = nullptr;
signals:
    void hover_changed(bool hovering);
protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
};

// Capture toolbar.
class Toolbar : public QWidget {
    Q_OBJECT
public:
    // 100% base sizes; the overall scale comes from core/ui_scale.
    static constexpr int BASE_BTN_WIDTH = 38;
    static constexpr int BASE_BTN_HEIGHT = 40;  // also the toolbar height
    static constexpr int BASE_WIDE_WIDTH = 42;  // action buttons such as save and confirm
    // Icons are drawn on a 24 grid, so lines land on whole pixels at 100%.
    static constexpr int BASE_ICON = 24;
    static constexpr double HANDLE_WIDTH_RATIO = 0.32;  // handle width / toolbar height
    // Shifts the auto position right; tuned by eye, not computed.
    static constexpr int BASE_RIGHT_NUDGE = 4;
    // Tools with a settings panel. Pen/highlighter and rect/ellipse share panels but
    // store settings per tool, so this lists tools rather than panels.
    static const QStringList PANEL_TOOLS;

    // Capture and pin toolbars each read their own layout (layout_profile) and share the arranging. A constructor
    // argument rather than an override: this constructor already arranges the buttons.
    explicit Toolbar(QWidget *parent = nullptr, const toolbar_layout::LayoutProfile &layout_profile = toolbar_layout::SCREENSHOT);
    const toolbar_layout::LayoutProfile &layout_profile;
    // Subclasses with a different constructor flow call this themselves.
    void init_ui();
    // Rearranges per the user's layout: shown buttons on the bar, folded ones for the popup, "..." last.
    virtual void reload_layout();
    // The "..." popup, once created. Pin toolbars have no parent, so it is a top-level window of its own.
    ToolbarMorePopup *more_popup() const { return more_popup_; }
    // Recomputes sizes, layout and position. Only sizes change: tool, checked state and panel values stay.
    void apply_scale();
    // Creates only the requested panel; routine hide/scale operations keep others absent.
    QWidget *ensure_panel_for_tool(const QString &tool_id);
    void edit_watermark_text();
    // Shows the watermark on the image in the panel, expanded at the watermark's own time.
    void show_watermark(const QString &template_text, const QDateTime &when);
    // A temporary edit of the selected item only; must not save new-item defaults.
    void set_temporary_edit_active(bool active);
    // Restores the active tool's full default panel after a selection ends.
    void restore_active_tool_state(const QString &tool_id, const ToolContext *ctx = nullptr, QGraphicsScene *scene = nullptr);
    // Mouse clicks opt into toggling back to the cursor; keyboard shortcuts are idempotent.
    void select_tool(const QString &tool_id, bool toggle = false);
    void hide_all_panels();
    // Shows the tool's panel, refilled from that tool's saved settings. Item-specific
    // syncing after a selection must run after this.
    void show_panel_for_tool(const QString &tool_id);
    // Setters that update the panels without emitting.
    void set_current_color(const QColor &color);
    void set_stroke_width(int width);
    void set_opacity(int opacity_255);
    void set_number_next_value(int value);
    // Below the selection right-aligned, else above; owned by the screen under the selection's centre.
    void position_near_rect(const QRectF &rect, QWidget *parent_widget = nullptr);

    std::optional<QString> current_tool;
    bool temporary_edit_active = false;
    QColor current_color = QColor(255, 0, 0);
    ToolbarDragHandle *drag_handle = nullptr;
    QPushButton *long_screenshot_btn = nullptr, *save_btn = nullptr, *screenshot_translate_btn = nullptr;
    QPushButton *text_recognize_btn = nullptr, *scan_code_btn = nullptr, *gif_btn = nullptr, *copy_btn = nullptr;
    QPushButton *pen_btn = nullptr, *highlighter_btn = nullptr, *mosaic_btn = nullptr, *spotlight_btn = nullptr;
    QPushButton *watermark_btn = nullptr, *arrow_btn = nullptr, *number_btn = nullptr, *rect_btn = nullptr;
    QPushButton *ellipse_btn = nullptr, *text_btn = nullptr, *eraser_btn = nullptr;
    QPushButton *undo_btn = nullptr, *redo_btn = nullptr, *cancel_btn = nullptr, *pin_btn = nullptr, *confirm_btn = nullptr;
    ToolbarMoreHandle *more_btn = nullptr;
    QHash<QString, QPushButton *> tool_buttons;
    PaintSettingsPanel *paint_panel = nullptr;
    ShapeSettingsPanel *shape_panel = nullptr;
    ArrowSettingsPanel *arrow_panel = nullptr;
    NumberSettingsPanel *number_panel = nullptr;
    TextSettingsPanel *text_panel = nullptr;
    MosaicSettingsPanel *mosaic_panel = nullptr;
    WatermarkSettingsPanel *watermark_panel = nullptr;

    // Unfolds the number panel's label and pointer part showing a selected number's state, or folds it.
    void show_number_extras(const std::optional<QVariantMap> &state);
signals:
    void tool_changed(const QString &tool_id);
    void save_clicked();
    void copy_clicked();
    void pin_clicked();
    void confirm_clicked();
    void cancel_clicked();
    void undo_clicked();
    void redo_clicked();
    void long_screenshot_clicked();
    void screenshot_translate_clicked();
    void text_recognize_clicked();
    void scan_code_clicked();
    void gif_record_clicked();
    void color_changed(const QColor &color);
    void number_style_changed(const QString &style);
    void stroke_width_changed(int width);
    void opacity_changed(int opacity);  // 0-255
    void number_next_changed(int value);
    void text_font_changed(const QFont &font);
    void text_color_changed(const QColor &color);
    // width is one of TextItem::OUTLINE_WIDTH_LEVELS.
    void text_outline_changed(bool enabled, const QColor &color, double width);
    // The colour's alpha is the shadow opacity.
    void text_shadow_changed(bool enabled, const QColor &color);
    void text_background_changed(bool enabled, const QColor &color, int opacity);
    void arrow_style_changed(const QString &style);
    void mosaic_style_changed(const QString &style);
    void mosaic_block_size_changed(int block_size);
    void line_style_changed(const QString &style);
    void highlighter_mode_changed(const QString &mode);
    // Applying the watermark (reusing an existing layer); style changes are
    // (WatermarkStyle field, value); text is a template with time placeholders.
    void watermark_requested();
    void watermark_style_changed(const QString &key, const QVariant &value);
    void watermark_text_changed(const QString &template_text);

protected:
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    // The capture hides the toolbar while dragging the selection or starting a session.
    void hideEvent(QHideEvent *event) override;
    // The host window: the parent in a capture; pin toolbars are top-levels and override this.
    virtual QWidget *host_window() const { return parentWidget(); }
    // Re-anchors after a size change; pin toolbars override.
    virtual void reposition_self();
    // Double-clicking the handle returns to automatic positioning and repositions at once.
    virtual void reset_auto_position();
    void set_manual_positioned(bool manual);
    // Lays keys out in a row right of the handle, hiding every other button; the toolbar shrinks to fit.
    void arrange(const QStringList &keys);
    void sync_all_panels_position();
    // Same side as the toolbar (away from the selection) when there is room, else the other side.
    // Left-aligned with the toolbar; align_right for the "..." popup at the right end.
    void sync_panel_position(QWidget *panel, bool align_right = false);
    // Tallest settings panel plus its gap, reserved when placing the toolbar.
    int max_panel_height() const;
    virtual void show_more_popup();
    virtual void hide_more_popup();
    virtual void open_layout_dialog();
    // Subclasses add the buttons of their own layout keys after this constructor, then call reload_layout.
    QPushButton *add_button(const QString &key, const QString &icon, const char *tooltip, int width, std::function<void()> on_click, bool checkable = false);
    // A tool button of a subclass: the key is the tool id; the subclass also registers it in tool_buttons.
    QPushButton *add_tool_button(const QString &tool_id, const QString &icon, const char *tooltip, int width);
    QHash<QString, int> button_widths_;
    QStringList folded_keys_;
    bool manual_positioned_ = false;
    bool dragging_ = false;
    QPoint drag_offset_;
    bool toolbar_below_selection_ = true;
    int btn_height_ = 0;

private:
    void apply_button_sizes();
    // Window attributes shared by the toolbar, its panels and the popup.
    void make_floating(QWidget *widget) const;
    QList<QWidget *> panels() const;
    // The "..." and the popup have a gap between them, so closing waits a moment.
    void on_more_hover(bool hovering);
    // Created on first use: watermarks are rarely used, so building the toolbar skips it. It skips the
    // generic colour signals, which would change the pen colour and cursor.
    WatermarkSettingsPanel *ensure_watermark_panel();
    void set_watermark_text(const QString &template_text);
    // Fills a tool's panel from its saved settings: what drawing now would produce.
    void sync_panel_from_settings(const QString &tool_id);
    void retranslate();
    void on_tool_clicked(const QString &tool_id);
    // Applies a watermark and opens the panel; when already selected, only closes the panel.
    void on_watermark_clicked();
    CursorManager *host_cursor_manager() const;
    void on_panel_color_changed(const QColor &color);
    void on_highlighter_mode_changed(const QString &mode);
    void on_text_font_changed(const QFont &font);
    void on_text_color_changed(const QColor &color);
    void on_text_background_changed(bool enabled, const QColor &color, int opacity);
    void on_text_outline_changed(bool enabled, const QColor &color, double width);
    void on_text_shadow_changed(bool enabled, const QColor &color);
    void on_arrow_style_changed(const QString &style);
    void on_line_style_changed(const QString &style);
    void on_mosaic_mode_changed(const QString &mode);
    void on_mosaic_style_changed(const QString &style);
    void on_mosaic_block_size_changed(int value);
    void on_number_next_changed(int value);
    QScreen *screen_by_center(const QRect &rect) const;

    QStringList button_order_;
    QHash<QString, QAbstractButton *> buttons_;
    QHash<QString, double> base_widths_;
    QHash<QString, QString> icon_paths_;
    std::vector<std::pair<QPointer<QWidget>, QString>> tooltip_sources_;
    QPointer<ToolbarMorePopup> more_popup_;
    QTimer *more_close_timer_ = nullptr;
};
}
