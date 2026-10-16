#pragma once
#include <QLabel>
#include <QPushButton>
#include <QRectF>
#include <QWidget>
#include <functional>
#include <optional>

class QGraphicsView;
class QHBoxLayout;
class QSpacerItem;

namespace jietuba {
// What the panel needs of a view: its viewport and scene-to-viewport mapping. A
// QGraphicsView provides both; the quick capture overlay maps global coordinates itself.
struct InfoPanelView {
    std::function<QWidget *()> viewport;
    std::function<QPoint(const QPointF &)> map_from_scene;
    static InfoPanelView of(QGraphicsView *view);
};

// Info and quick-action strip above the selection; UI only. A small overlay
// moved to follow the selection, outside the scene so exports stay clean.
class SelectionInfoPanel : public QWidget {
    Q_OBJECT
public:
    static constexpr int BASE_RADIUS = 7;
    static constexpr int BASE_HEIGHT = 34;
    static constexpr int BASE_MARGIN_ABOVE = 7;  // panel bottom to selection top
    static constexpr int BASE_BTN = 26;
    static constexpr int BASE_ICON = 19;
    static constexpr int BASE_SEP_HEIGHT = 19;
    static constexpr int BASE_FONT = 14;
    SelectionInfoPanel(QWidget *parent, QGraphicsView *view);
    SelectionInfoPanel(QWidget *parent, InfoPanelView view);
    // Toggle states and text stay; only sizes change.
    void apply_scale();
    // Above the selection left-aligned, else left of it, else right of it, else inside its top-left corner.
    void follow_rect(const QRectF &scene_rect);
    // The action buttons show only once the selection is confirmed.
    void set_confirmed(bool confirmed);
    void update_info_text(const QRectF &rect);
    QPushButton *btn_rounded = nullptr;
    QPushButton *btn_lock = nullptr;
    QPushButton *btn_border = nullptr;
    QPushButton *btn_refresh = nullptr;
signals:
    void border_toggled(bool checked);
    void rounded_toggled(bool checked);
    void lock_ratio_toggled(bool checked);
    // Pressed starts a single refresh or a long press; released ends it.
    void refresh_pressed();
    void refresh_released();
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    void init_ui();
    QPushButton *make_btn(const QString &svg, bool checkable, const char *tip);
    InfoPanelView view_;
    // Re-anchored above the selection after a scale change.
    std::optional<QRectF> last_rect_;
    QHBoxLayout *row_layout_ = nullptr;
    QLabel *info_label_ = nullptr;
    QSpacerItem *label_spacer_ = nullptr;
    QLabel *sep_ = nullptr;
    QList<QPushButton *> buttons_;
    QList<QWidget *> action_widgets_;
};
}
