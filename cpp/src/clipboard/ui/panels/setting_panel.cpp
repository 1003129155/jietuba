#include "clipboard/ui/panels/setting_panel.h"
#include "clipboard/ui/theme/themes.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/submenu_position.h"
#include <QHBoxLayout>
#include <QMenu>
#include <QPushButton>
#include <QWidgetAction>
#include <memory>

namespace jietuba::clipboard {
const std::vector<std::pair<const char *, QString>> &MULTI_PASTE_SEPARATORS() {
    static const std::vector<std::pair<const char *, QString>> separators{
        {"New Line", "\n"}, {"Blank Line", "\n\n"}, {"Space", " "}, {"Tab", "\t"}, {"Comma", ", "}, {"No Separator", ""},
    };
    return separators;
}

QString escape_separator(const QString &text) {
    QString escaped = text;
    escaped.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t");
    return escaped;
}

QString unescape_separator(const QString &text) {
    QString out;
    qsizetype index = 0;
    while (index < text.size()) {
        const QString pair = text.mid(index, 2);
        if (pair == "\\n" || pair == "\\t" || pair == "\\\\") {
            out += pair == "\\n" ? "\n" : pair == "\\t" ? "\t" : "\\";
            index += 2;
        } else {
            out += text[index];
            index += 1;
        }
    }
    return out;
}

namespace {
void add_multi_paste_menu(QMenu *menu, const SettingMenuOptions &o) {
    const auto &tr = o.tr;
    QMenu *multi_menu = menu->addMenu(tr("Multi-Select Paste"));
    multi_menu->setStyleSheet(o.menu_style);

    QMenu *separator_menu = multi_menu->addMenu(tr("Separator"));
    separator_menu->setStyleSheet(o.menu_style);
    bool is_preset = false;
    for (const auto &[label, value] : MULTI_PASTE_SEPARATORS()) {
        QAction *act = separator_menu->addAction(tr(label));
        act->setCheckable(true);
        act->setChecked(o.multi_paste_separator == value);
        is_preset = is_preset || o.multi_paste_separator == value;
        const auto on_set = o.on_set_multi_paste_separator;
        QObject::connect(act, &QAction::triggered, separator_menu, [on_set, v = value] { on_set(v); });
    }

    // Custom: in the field \n is a line break and \t a tab; Enter applies.
    auto *custom = new QWidget(separator_menu);
    auto *layout = new QHBoxLayout(custom);
    layout->setContentsMargins(scaled(8), scaled(4), scaled(8), scaled(4));
    auto *custom_edit = new LineEdit(custom);
    custom_edit->setPlaceholderText(tr("Custom, \\n for a new line"));
    custom_edit->setMinimumWidth(scaled(150));
    if (!is_preset) custom_edit->setText(escape_separator(o.multi_paste_separator));
    const auto on_set = o.on_set_multi_paste_separator;
    QObject::connect(custom_edit, &QLineEdit::returnPressed, menu, [on_set, custom_edit, menu] {
        on_set(unescape_separator(custom_edit->text()));
        menu->close();
    });
    layout->addWidget(custom_edit);
    auto *custom_action = new QWidgetAction(separator_menu);
    custom_action->setDefaultWidget(custom);
    separator_menu->addAction(custom_action);

    QMenu *order_menu = multi_menu->addMenu(tr("Order"));
    order_menu->setStyleSheet(o.menu_style);
    for (const auto &[key, label] : {std::make_pair(QString("oldest_first"), tr("First Selected First")), std::make_pair(QString("newest_first"), tr("Last Selected First"))}) {
        QAction *act = order_menu->addAction(label);
        act->setCheckable(true);
        act->setChecked(o.multi_paste_order == key);
        const auto on_order = o.on_set_multi_paste_order;
        QObject::connect(act, &QAction::triggered, order_menu, [on_order, k = key] { on_order(k); });
    }

    QAction *keep_act = multi_menu->addAction(tr("Record Content"));
    keep_act->setCheckable(true);
    keep_act->setChecked(o.multi_paste_keep_merged);
    QObject::connect(keep_act, &QAction::triggered, multi_menu, o.on_toggle_multi_paste_keep_merged);
}
}

QMenu *show_setting_menu(QWidget *parent, const SettingMenuOptions &o) {
    const auto &tr = o.tr;
    auto *menu = new QMenu(parent);
    // The reference keeps every closed menu as a child of the window; it is freed here once closed.
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setStyleSheet(o.menu_style);

    auto toggle_action = [&](const char *label, bool is_checked, const std::function<void(bool)> &callback) {
        QAction *act = menu->addAction(tr(label));
        act->setCheckable(true);
        act->setChecked(is_checked);
        QObject::connect(act, &QAction::triggered, menu, callback);
    };
    toggle_action("Paste with Format", o.paste_with_html, o.on_toggle_paste_html);
    toggle_action("Auto Paste After Selection", o.auto_paste, o.on_toggle_auto_paste);
    toggle_action("Close After Paste", o.close_after_paste, o.on_toggle_close_after_paste);
    toggle_action("Move to Top After Paste", o.move_to_top, o.on_toggle_move_to_top);
    toggle_action("Show Time and Source", o.show_metadata, o.on_toggle_show_metadata);
    toggle_action("Preserve Search on Reopen", o.preserve_search, o.on_toggle_preserve_search);

    if (o.on_set_multi_paste_separator) add_multi_paste_menu(menu, o);
    menu->addSeparator();

    // ── Opacity submenu ──
    QMenu *opacity_menu = menu->addMenu(tr("Window Opacity"));
    opacity_menu->setStyleSheet(o.menu_style);
    for (const auto &option : o.opacity_options) {
        const int percent = option.toInt();
        QAction *act = opacity_menu->addAction(percent == 0 ? tr("Opaque") : QString("%1%").arg(percent));
        act->setCheckable(true);
        act->setChecked(o.window_opacity == percent);
        const auto on_set = o.on_set_opacity;
        QObject::connect(act, &QAction::triggered, opacity_menu, [on_set, percent] { on_set(percent); });
    }

    // ── Font size submenu ──
    QMenu *font_menu = menu->addMenu(tr("Font Size"));
    font_menu->setStyleSheet(o.menu_style);
    for (const auto &option : o.font_size_options) {
        const int size = option.toInt();
        QAction *act = font_menu->addAction(QString("%1px").arg(size));
        act->setCheckable(true);
        act->setChecked(o.current_font_size == size);
        const auto on_set = o.on_set_font_size;
        QObject::connect(act, &QAction::triggered, font_menu, [on_set, size] { on_set(size); });
    }

    // ── Image size submenu ──
    if (o.on_set_image_size) {
        QMenu *image_menu = menu->addMenu(tr("Image Size"));
        image_menu->setStyleSheet(o.menu_style);
        for (const auto &[key, label] : {std::make_pair(QString("small"), tr("Small")), std::make_pair(QString("medium"), tr("Medium")), std::make_pair(QString("large"), tr("Large"))}) {
            QAction *act = image_menu->addAction(label);
            act->setCheckable(true);
            act->setChecked(o.current_image_size == key);
            const auto on_set = o.on_set_image_size;
            QObject::connect(act, &QAction::triggered, image_menu, [on_set, k = key] { on_set(k); });
        }
    }

    // ── Theme submenu ──
    QMenu *theme_menu = menu->addMenu(tr("Theme"));
    theme_menu->setStyleSheet(o.menu_style);
    struct ThemeButton {
        QString name;
        QPushButton *button;
        QString accent;
        QString bg;
    };
    auto theme_buttons = std::make_shared<std::vector<ThemeButton>>();
    auto update_theme_btns = [theme_buttons](const QString &selected) {
        for (const auto &[name, btn, accent, bg] : *theme_buttons) {
            const bool is_sel = name == selected;
            btn->setText(is_sel ? QString::fromUtf8("✓") : QString());
            const QString text_color = name == "dark" ? "#FFFFFF" : "#333333";
            const int bw = is_sel ? scaled(2) : 1;
            const QString bc = is_sel ? accent : "#CCCCCC";
            btn->setStyleSheet(QString(R"(
                QPushButton {
                    background: qlineargradient(x1:0,y1:0,x2:1,y2:0,
                        stop:0 %1, stop:0.5 %1,
                        stop:0.5 %2, stop:1 %2);
                    border: %3px solid %4;
                    border-radius: %5px;
                    color: %6;
                    font-size: %7px; font-weight: bold;
                    text-align: left; padding-left: %8px;
                }
                QPushButton:hover { border: %9px solid %2; }
            )")
                                   .arg(bg, accent)
                                   .arg(bw)
                                   .arg(bc)
                                   .arg(scaled(4))
                                   .arg(text_color)
                                   .arg(scaled(12))
                                   .arg(scaled(6))
                                   .arg(scaled(2)));
        }
    };
    for (const auto &[theme_name, colors] : PRESET_THEME_SWATCHES()) {
        auto *wa = new QWidgetAction(theme_menu);
        auto *btn = new QPushButton;
        btn->setFixedSize(scaled(120), scaled(28));
        btn->setCursor(Qt::PointingHandCursor);
        const auto on_set_theme = o.on_set_theme;
        QObject::connect(btn, &QPushButton::clicked, theme_menu, [on_set_theme, update_theme_btns, theme_menu, name = theme_name] {
            on_set_theme(name);
            update_theme_btns(name);
            theme_menu->close();
        });
        wa->setDefaultWidget(btn);
        theme_menu->addAction(wa);
        theme_buttons->push_back({theme_name, btn, colors.first, colors.second});
    }
    update_theme_btns(o.current_theme_name);

    // ── Group bar position submenu ──
    if (o.on_set_group_bar_position) {
        QMenu *bar_pos_menu = menu->addMenu(tr("Group Bar Position"));
        bar_pos_menu->setStyleSheet(o.menu_style);
        for (const auto &[key, label] : {std::make_pair(QString("right"), tr("▶ Right")), std::make_pair(QString("left"), tr("◀ Left")), std::make_pair(QString("top"), tr("▲ Top"))}) {
            QAction *act = bar_pos_menu->addAction(label);
            act->setCheckable(true);
            act->setChecked(o.current_group_bar_position == key);
            const auto on_set = o.on_set_group_bar_position;
            QObject::connect(act, &QAction::triggered, bar_pos_menu, [on_set, k = key] { on_set(k); });
        }
    }

    menu->addSeparator();
    QAction *add_act = menu->addAction(tr("Add Content"));
    QObject::connect(add_act, &QAction::triggered, menu, o.on_add_item);

    // Pops up above the anchor (non-blocking; clicking again closes it).
    const QPoint popup_pos(o.anchor_pos.x(), o.anchor_pos.y() - menu->sizeHint().height());
    avoid_submenu_overlap(menu);
    menu->popup(popup_pos);
    return menu;
}
}
