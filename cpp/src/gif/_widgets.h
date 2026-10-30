#pragma once
#include <QIcon>
#include <QMenu>
#include <QPushButton>
#include <QPointer>
#include <QVariant>
#include <utility>
#include <vector>

// Small widgets and helpers shared by the GIF module.
namespace jietuba {
// Recording and playback share transparent controls; the panel is painted by paint_rounded_panel.
QString toolbar_style();

// Shows a floating menu next to a button without blocking, flipping at screen edges.
void popup_menu(QMenu *menu, QWidget *anchor, bool below = true);

// Renders an SVG icon to a square of size pixels so every icon shows at the same size.
QIcon svg_icon(const QString &name, int size = 22);

// Pops up an option menu on click; a choice updates the text and emits option_selected.
class ClickMenuButton : public QPushButton {
    Q_OBJECT
public:
    // options: [(label, value), ...]
    ClickMenuButton(std::vector<std::pair<QString, QVariant>> options, int default_index = 0, QWidget *parent = nullptr);
    // Re-applies the style for the current scale (sizes come from the style's font and padding).
    void apply_scale() { apply_style(); }
    QVariant current_value() const { return options_[current_index_].second; }
    void set_enabled(bool enabled);
    // Replaces the options and selection without emitting option_selected.
    void set_options(std::vector<std::pair<QString, QVariant>> options, int current_index) {
        options_ = std::move(options);
        current_index_ = current_index;
        update_text();
    }
    bool popup_below = true;
signals:
    void option_selected(const QVariant &value);
protected:
    void hideEvent(QHideEvent *event) override;
private:
    void apply_style();
    void update_text() { setText(options_[current_index_].first); }
    void show_menu();
    void on_select(int index);
    std::vector<std::pair<QString, QVariant>> options_;
    int current_index_;
    bool enabled_flag_ = true;
    QPointer<QMenu> menu_;
};
}
