#pragma once
#include "ui/base_settings_panel.h"
#include "tools/number.h"
#include <QMap>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <vector>

namespace jietuba {
// Paints a real NumberItem, so previews match what is drawn.
QPixmap render_number_style_preview(const QString &style, const QColor &color, int side, int number = 1);

// Style strip that opens when hovering the next-number preview.
class NumberStylePopup : public HoverPopup {
    Q_OBJECT
public:
    static constexpr int BASE_ITEM_SIDE = 20;
    static constexpr int BASE_ITEM_PADDING = 6;
    // A style picker, not a colour: neutral ink stays visible whatever colour is chosen.
    static inline const QColor PREVIEW_INK = QColor("#444444");
    explicit NumberStylePopup(QWidget *parent = nullptr);
    void apply_scale() override;
    void set_current_style(const QString &style);
signals:
    void style_selected(const QString &style);
private:
    void choose(const QString &style);
    QMap<QString, QToolButton *> buttons_;
};

class NumberSettingsPanel : public BaseSettingsPanel {
    Q_OBJECT
public:
    static constexpr int BASE_NEXT_PREVIEW = 22;
    static constexpr int BASE_NEXT_BTN_WIDTH = 13;
    static constexpr int BASE_LABEL_BTN = 28, BASE_LABEL_ICON = 18, BASE_FONT_VIEW_WIDTH = 200;
    static constexpr int BASE_FONT_COMBO_WIDTH = 120, BASE_ARROW_PREVIEW_WIDTH = 40;
    explicit NumberSettingsPanel(QWidget *parent = nullptr);
    void set_next_number(int value);
    // Sets without emitting style_changed.
    void set_style(const QString &style);
    QString current_style() const { return current_style_; }
    // The part for a number's label: icons only, so its width is the same in every language.
    void set_label_section_visible(bool visible);
    bool label_section_visible() const { return label_section_visible_; }
    // Sets without emitting the label signals.
    void set_label_state(const QString &font_family, int size_level, bool outline, bool guide, const QString &arrow_style);
    QLabel *next_preview = nullptr;
    QToolButton *next_up_btn = nullptr;
    QToolButton *next_down_btn = nullptr;
    PillComboBox *label_font_combo = nullptr, *label_size_combo = nullptr, *label_arrow_combo = nullptr;
    QPushButton *label_outline_btn = nullptr, *label_guide_btn = nullptr;
signals:
    void next_number_changed(int value);
    void style_changed(const QString &style);
    void label_font_changed(const QString &family);
    void label_size_changed(int level);
    void label_outline_changed(bool enabled);
    void label_guide_changed(bool enabled);
    void arrow_style_changed(const QString &style);
protected:
    // Shares NumberTool's range: the panel must show exactly what is drawn.
    std::pair<int, int> size_range() const override { return {static_cast<int>(NumberTool::MIN_WIDTH), static_cast<int>(NumberTool::MAX_WIDTH)}; }
    int size_default() const override { return 16; }
    const char *size_tooltip() const override { return "Font Size"; }
    // Number size, not line width, so no width glyph.
    QString size_glyph() const override { return {}; }
    void build_extra_controls(QHBoxLayout *layout) override;
    // The label part's combos are sized by the panel; their lists need the white rows.
    QString build_stylesheet() const override { return build_settings_panel_stylesheet() + combo_popup_qss(); }
    void apply_scale_sizes() override;
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    void update_next_preview(std::optional<int> value = std::nullopt);
    void change_next_number(int delta);
    NumberStylePopup *ensure_style_popup();
    void show_style_popup();
    void build_label_section(QHBoxLayout *layout);
    void apply_label_section_scale();
    std::vector<QWidget *> label_widgets_;
    QLabel *label_icon_ = nullptr;
    bool label_section_visible_ = false;
    PillFrame *next_pill_ = nullptr;
    QHBoxLayout *next_pill_layout_ = nullptr;
    QVBoxLayout *next_btn_layout_ = nullptr;
    int next_value_ = 1;
    QString current_style_ = NumberItem::DEFAULT_STYLE;
    NumberStylePopup *style_popup_ = nullptr;
};
}
