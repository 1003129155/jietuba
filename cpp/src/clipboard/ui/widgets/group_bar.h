#pragma once
#include "clipboard/core/models.h"
#include "clipboard/ui/theme/themes.h"
#include "core/i18n.h"
#include <QBoxLayout>
#include <QColor>
#include <QPointer>
#include <QPushButton>
#include <QWidget>
#include <optional>

class QFrame;
class QHBoxLayout;
class QVBoxLayout;

namespace jietuba::clipboard {
class ClipboardController;
class ClipboardManager;
class ManageDialog;

// A group button; while checked it carries an accent line along its outer edge.
class GroupButton : public QPushButton {
    Q_OBJECT
public:
    using QPushButton::QPushButton;
    // None draws no line.
    void set_indicator(std::optional<Qt::Edge> edge, const QColor &color);
    std::optional<Qt::Edge> indicator_edge() const { return edge_; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    std::optional<Qt::Edge> edge_;
    QColor color_;
};

// The group buttons beside the clipboard list (left, right or top), with keyboard
// navigation through them and the group context menu.
class GroupBar : public QWidget {
    Q_OBJECT
public:
    static QString tr_text(const char *text) { return i18n_tr(text, "GroupBar"); }
    GroupBar(ClipboardController *controller, ClipboardManager *manager, const Theme &theme, const QString &position = "right", QWidget *parent = nullptr);

    bool sidebar_mode() const { return sidebar_mode_; }
    // Builds the bar once and inserts it into content_layout.
    void build(QHBoxLayout *content_layout, QWidget *left_widget, QVBoxLayout *left_layout);
    void set_position(const QString &position);
    // Follows the toolbar and panel scale.
    void apply_scale();
    void set_theme(const Theme &theme);
    // Rebuilds the group buttons; what does not fit goes into an overflow menu.
    void refresh_buttons();
    void enter_sidebar_mode(std::optional<qint64> prev_item_id = std::nullopt);
    void exit_sidebar_mode();
    void move_sidebar_selection(int delta);
    // Top mode: left/right switches groups directly.
    void handle_top_group_switch(int delta);
    // Mouse back on the list clears the sidebar highlight.
    void clear_sidebar_focus();
    void switch_to_group(std::optional<qint64> group_id);
    void on_manage_data_changed();

    // GroupBar itself is the bar; kept as a name for callers.
    QWidget *bar_widget;
    QBoxLayout *bar_layout = nullptr;
    QWidget *group_buttons_widget;
    QBoxLayout *group_buttons_layout = nullptr;
    QList<GroupButton *> group_buttons;
    QPushButton *close_btn;
    GroupButton *clipboard_btn;
    QPushButton *add_group_btn;
    QString position;

signals:
    // None means the clipboard history.
    void group_switched(std::optional<qint64> group_id);
    void close_requested();
    void sidebar_entered();
    void sidebar_exited(std::optional<qint64> prev_item_id);
    void manage_groups_requested();
    void manage_items_requested();

private:
    QPushButton *create_close_btn();
    GroupButton *create_clipboard_btn();
    QPushButton *create_add_group_btn();
    GroupButton *make_group_btn(const Group &group);
    // Sizes, styles, icons and indicators of every button, for the current position and theme.
    void apply_button_look();
    void clear_bar_layout();
    void remove_from_host_layouts();
    void build_bar();
    QString icon_btn_style() const;
    QString sidebar_btn_style() const;
    QString overflow_btn_style() const;
    QString menu_style() const;
    QList<QPushButton *> sidebar_buttons() const;
    void on_sidebar_button_clicked(QPushButton *target, std::optional<qint64> group_id);
    void sync_sidebar_index();
    void set_sidebar_focus_button(QPushButton *target);
    void play_sidebar_focus_animation(QPushButton *target);
    void show_overflow_group_menu(const QList<Group> &hidden_groups);
    void clear_group_buttons_layout();
    void show_group_context_menu(QPushButton *btn, qint64 group_id, const QPoint &pos);
    void delete_group(qint64 group_id);
    void move_group_order(qint64 group_id, int direction);
    void edit_group(qint64 group_id);

    ClipboardController *controller_;
    ClipboardManager *manager_;
    Theme current_theme_;
    bool sidebar_mode_ = false;
    int sidebar_index_ = 0;
    std::optional<qint64> sidebar_prev_item_id_;
    QPointer<QFrame> separator_;
    QHBoxLayout *content_layout_ = nullptr;
    QWidget *left_widget_ = nullptr;
    QVBoxLayout *left_layout_ = nullptr;
};
}
