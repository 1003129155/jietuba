#pragma once
#include "tools/mosaic.h"
#include "ui/base_settings_panel.h"
#include <QButtonGroup>
#include <QMap>

namespace jietuba {
// Paints what a level actually looks like, through the same reduce-then-enlarge path
// as MosaicItem; mosaic and blur differ only in smooth enlargement. ratio is the
// device pixel ratio so mosaic edges stay hard on high-DPI screens.
QPixmap render_block_size_swatch(int block_size, int side, bool smooth, double ratio = 1.0);

// Mosaic panel: freehand/rect mode and brush size, then the kind (mosaic, blur, smart
// erase) and last the strength levels. No colour or opacity: it smears background pixels.
// Smart erase has no strength; the levels stay in place but are disabled, so switching
// kinds never changes the panel's width.
class MosaicSettingsPanel : public QWidget {
    Q_OBJECT
public:
    // Values come from the tool, so stored strings and tool strings cannot drift apart.
    static inline const QString MODE_FREEHAND_VALUE = MosaicTool::MODE_FREEHAND;
    static inline const QString MODE_RECT_VALUE = MosaicTool::MODE_RECT;
    static inline const QString STYLE_PIXELATE_VALUE = MosaicTool::STYLE_PIXELATE;
    static inline const QString STYLE_BLUR_VALUE = MosaicTool::STYLE_BLUR;
    static inline const QString STYLE_SMART_ERASE_VALUE = MosaicTool::STYLE_SMART_ERASE;
    static constexpr int BASE_MARGIN_H = 12;
    static constexpr int BASE_MARGIN_V = 6;
    static constexpr int BASE_SPACING = 10;
    static constexpr int BASE_SIZE_SPIN_WIDTH = 68;
    static constexpr int BASE_BTN = 28;
    static constexpr int BASE_ICON = 22;
    explicit MosaicSettingsPanel(QWidget *parent = nullptr);
    void apply_scale();
    // Setters for the toolbar; they do not emit.
    void set_size(int size);
    void set_draw_mode(const QString &mode);
    void set_style(const QString &style);
    void set_block_size(const QVariant &value);
    void retranslate();
    QString draw_mode() const { return current_draw_mode; }
    QString style() const { return current_style; }
    int block_size() const { return current_block_size; }

    QString current_draw_mode = MODE_FREEHAND_VALUE;
    QString current_style = STYLE_PIXELATE_VALUE;
    int current_size = 30;
    int current_block_size = MosaicTool::DEFAULT_BLOCK_SIZE;
    QWidget *mode_widget = nullptr;
    QPushButton *freehand_btn = nullptr;
    QPushButton *rect_btn = nullptr;
    QButtonGroup *mode_group = nullptr;
    StepperWidget *size_spin = nullptr;
    QButtonGroup *block_size_group = nullptr;
    QMap<int, QPushButton *> block_size_buttons;
    QButtonGroup *kind_group = nullptr;
    // Keyed by style value.
    QMap<QString, QPushButton *> kind_buttons;
signals:
    void draw_mode_changed(const QString &mode);
    void style_changed(const QString &style);
    void size_changed(int size);
    void block_size_changed(int block_size);
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    void init_ui();
    void connect_signals();
    void on_block_size_clicked(QAbstractButton *button);
    // Strength is shared by mosaic and blur, so the icons follow the current kind;
    // smart erase disables them.
    void refresh_block_size_icons();
    void retranslate_kinds();
    QHBoxLayout *block_size_layout_ = nullptr;
    QHBoxLayout *kind_layout_ = nullptr;
    int swatch_side_ = 0;
};
}
