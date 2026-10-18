#include "clipboard/ui/theme/theme_styles.h"
#include "clipboard/ui/theme/chevron.h"
#include "core/logger.h"
#include "core/ui_scale.h"

namespace jietuba::clipboard {
namespace {
QString px(int base) { return QString::number(scaled(base)) + "px"; }
}

QString ThemeStyleGenerator::alpha_text(int opacity) {
    const int alpha = 255 - static_cast<int>(opacity * 2.55);
    return python_str(QVariant(alpha / 255.0));
}

QString ThemeStyleGenerator::hex_to_rgb(const QString &hex_color) {
    QString hex = hex_color;
    while (hex.startsWith(u'#')) hex.remove(0, 1);
    if (hex.size() == 3) hex = QString(hex[0]) + hex[0] + hex[1] + hex[1] + hex[2] + hex[2];
    return QString("%1, %2, %3").arg(hex.mid(0, 2).toInt(nullptr, 16)).arg(hex.mid(2, 2).toInt(nullptr, 16)).arg(hex.mid(4, 2).toInt(nullptr, 16));
}

QString ThemeStyleGenerator::generate_window_style(int opacity) const {
    const auto &c = theme_.colors;
    return QString(R"(
            QFrame#mainContainer {
                background: rgba(%1, %2);
                border: 1px solid %3;
                border-radius: %4;
            }
            QToolTip {
                background: %5;
                color: %6;
                border: 1px solid %3;
                border-radius: %4;
                padding: %7 %8;
                font-size: %9;
            }
        )")
        .arg(hex_to_rgb(c.bg_primary), alpha_text(opacity), c.border_primary, px(4), c.bg_primary, c.text_primary, px(4), px(8), px(12));
}

QString ThemeStyleGenerator::generate_list_widget_style(int opacity) const {
    const auto &c = theme_.colors;
    return QString(R"(
            QListWidget {
                background: rgba(%1, %2);
                border: none;
                outline: none;
                color: %3;
            }
            QListWidget::item {
                padding: 0px;
                border: none;
                background: transparent;
            }
            QListWidget::item:selected {
                background: transparent;
            }
            QListWidget::item:hover {
                background: transparent;
            }
        )")
        .arg(hex_to_rgb(c.bg_primary), alpha_text(opacity), c.text_primary);
}

QString ThemeStyleGenerator::generate_search_bar_style(int opacity) const {
    const auto &c = theme_.colors;
    return QString(R"(
            QWidget {
                background: rgba(%1, %2);
                border-top: 1px solid %3;
            }
        )")
        .arg(hex_to_rgb(c.bg_secondary), alpha_text(opacity), c.border_primary);
}

QString ThemeStyleGenerator::generate_time_filter_bar_style(int opacity) const { return generate_search_bar_style(opacity); }

QString ThemeStyleGenerator::generate_quick_edit_style() const {
    const auto &c = theme_.colors;
    return QString(R"(
            QuickEditPopup {
                background: %1;
                border: 1px solid %2;
                border-radius: %3;
            }
            QTextEdit {
                background: %1;
                border: 1px solid %4;
                border-radius: %5;
                padding: %5 %6;
                font-size: %7;
                color: %8;
            }
            QTextEdit:focus {
                border-color: %2;
            }
            QLabel {
                color: %9;
                font-size: %10;
            }
            QPushButton {
                background: %11;
                color: %8;
                border: 1px solid %4;
                border-radius: %5;
                padding: %5 %12;
                font-size: %10;
            }
            QPushButton:hover {
                background: %13;
            }
            QPushButton#primary {
                background: %14;
                color: white;
                border: none;
            }
            QPushButton#primary:hover {
                background: %15;
            }
        )")
        .arg(c.bg_primary, c.border_accent, px(8), c.border_primary, px(4), px(6), px(13), c.text_primary, c.text_tertiary)
        .arg(px(12), c.bg_secondary, px(12), c.bg_hover, c.accent_primary, c.accent_hover);
}

QString ThemeStyleGenerator::generate_menu_style() const {
    const auto &c = theme_.colors;
    return QString(R"(
            QMenu {
                background: %1;
                border: 1px solid %2;
                border-radius: %3;
                padding: %3;
            }
            QMenu::item {
                padding: %4 %5;
                color: %6;
            }
            QMenu::item:selected {
                background: %7;
            }
            QMenu::separator {
                height: 1px;
                background: %2;
                margin: %3 %4;
            }
        )")
        .arg(c.bg_primary, c.border_primary, px(4), px(8), px(20), c.text_primary, c.bg_hover);
}

QString ThemeStyleGenerator::generate_search_input_style(bool has_text) const {
    const auto &c = theme_.colors;
    return QString(R"(
            QLineEdit {
                background: %1;
                border: none;
                color: %2;
                font-size: %3;
                padding: %4;
            }
        )")
        .arg(has_text ? c.bg_hover : "transparent", c.text_primary, px(13), px(4));
}

QString ThemeStyleGenerator::generate_clear_search_btn_style() const {
    const auto &c = theme_.colors;
    return QString(R"(
            QPushButton {
                background: transparent;
                color: %1;
                border: none;
                font-size: %2;
                padding: 0px;
            }
            QPushButton:hover {
                background: %3;
                color: %4;
                border-radius: %5;
            }
        )")
        .arg(c.text_tertiary, px(16), c.bg_hover, c.text_primary, px(12));
}

QString ThemeStyleGenerator::generate_filter_toggle_btn_style() const {
    const auto &c = theme_.colors;
    return QString(R"(
            QToolButton {
                background: transparent;
                color: %1;
                border: none;
                border-radius: %2;
                font-size: %3;
                padding: 0px;
            }
            QToolButton:hover {
                background: %4;
                color: %5;
            }
            QToolButton:checked {
                background: %4;
                color: %5;
            }
        )")
        .arg(c.text_secondary, px(4), px(17), c.bg_hover, c.text_primary);
}

QString ThemeStyleGenerator::generate_time_filter_date_edit_style() const {
    const auto &c = theme_.colors;
    const int arrow_w = scaled(9);
    const int arrow_h = scaled(6);
    const QString arrow = chevron_image(c.text_secondary, arrow_w, arrow_h);
    const QString arrow_rule = arrow.isEmpty() ? QString("image: none;") : QString("image: url(%1);").arg(arrow);
    return QString(R"(
            QDateEdit {
                background: %1;
                color: %2;
                border: 1px solid %3;
                border-radius: %4;
                padding: 0px 0px 0px %5;
                font-size: %6;
            }
            QDateEdit:hover {
                border-color: %7;
            }
            QDateEdit:focus {
                border-color: %8;
            }
            QDateEdit[hasError="true"] {
                border-color: %9;
                background: %10;
            }
            QDateEdit::drop-down {
                subcontrol-origin: padding;
                subcontrol-position: center right;
                width: %11;
                border: none;
                background: transparent;
            }
            QDateEdit::down-arrow {
                %12
                width: %13px;
                height: %14px;
            }
        )")
        .arg(c.bg_primary, c.text_primary, c.border_primary, px(13), px(10), px(12), c.text_tertiary, c.border_accent, c.error)
        .arg(c.error_bg, px(18), arrow_rule)
        .arg(arrow_w)
        .arg(arrow_h);
}

QString ThemeStyleGenerator::generate_time_filter_calendar_style() const {
    const auto &c = theme_.colors;
    return QString(R"(
            QCalendarWidget {
                background: %1;
                color: %2;
                border: 1px solid %3;
            }
            QCalendarWidget QWidget {
                background: %1;
                color: %2;
                alternate-background-color: %4;
            }
            QCalendarWidget QToolButton {
                background: transparent;
                color: %2;
                border: none;
                border-radius: %5;
                padding: %5;
            }
            QCalendarWidget QToolButton:hover {
                background: %6;
            }
            QCalendarWidget QMenu {
                background: %1;
                color: %2;
                border: 1px solid %3;
            }
            QCalendarWidget QMenu::item:selected {
                background: %7;
                color: %2;
            }
            QCalendarWidget QSpinBox {
                background: %1;
                color: %2;
                border: 1px solid %3;
                border-radius: %5;
                padding: %8 %5;
            }
            QCalendarWidget QAbstractItemView {
                background: %1;
                color: %2;
                selection-background-color: %9;
                selection-color: white;
                outline: none;
            }
            QCalendarWidget QAbstractItemView:disabled {
                color: %10;
            }
        )")
        .arg(c.bg_primary, c.text_primary, c.border_primary, c.bg_secondary, px(4), c.bg_hover, c.bg_selected, px(2), c.accent_primary)
        .arg(c.text_disabled);
}

QString ThemeStyleGenerator::generate_time_filter_action_btn_style(bool primary) const {
    const auto &c = theme_.colors;
    QString bg = primary ? c.accent_primary : "transparent";
    const QString fg = primary ? "white" : c.text_secondary;
    QString border = primary ? c.accent_primary : c.border_primary;
    QString hover_bg = primary ? c.accent_hover : c.bg_hover;
    const QString hover_fg = primary ? "white" : c.text_primary;
    if (primary && theme_.name == "dark") {
        bg = "#4E6578";
        border = "#4E6578";
        hover_bg = "#5C7488";
    }
    return QString(R"(
            QToolButton {
                background: %1;
                color: %2;
                border: 1px solid %3;
                border-radius: %4;
                padding: 0px;
                font-size: %5;
                font-weight: 500;
            }
            QToolButton:hover {
                background: %6;
                color: %7;
            }
        )")
        .arg(bg, fg, border, px(13), px(12), hover_bg, hover_fg);
}

QString ThemeStyleGenerator::generate_time_filter_close_btn_style() const {
    const auto &c = theme_.colors;
    return QString(R"(
            QToolButton {
                background: transparent;
                color: %1;
                border: none;
                border-radius: %2;
                padding: 0px;
                font-size: %3;
            }
            QToolButton:hover {
                background: %4;
                color: %5;
            }
        )")
        .arg(c.text_tertiary, px(13), px(16), c.bg_hover, c.text_primary);
}

QString ThemeStyleGenerator::generate_time_filter_type_btn_style() const {
    const auto &c = theme_.colors;
    return QString(R"(
            QToolButton {
                background: %1;
                color: %2;
                border: 1px solid %3;
                border-radius: %4;
                padding: 0px %5;
                font-size: %6;
                text-align: center;
            }
            QToolButton:hover {
                border-color: %7;
                background: %8;
            }
            QToolButton:pressed {
                background: %9;
            }
            QToolButton:disabled {
                background: %9;
                color: %10;
                border-color: %11;
            }
        )")
        .arg(c.bg_primary, c.text_primary, c.border_primary, px(13), px(8), px(12), c.text_tertiary, c.bg_hover, c.bg_selected)
        .arg(c.text_accent, c.border_accent);
}

QString ThemeStyleGenerator::generate_menu_btn_style() const {
    const auto &c = theme_.colors;
    return QString(R"(
            QPushButton {
                background: transparent;
                color: %1;
                border: none;
                font-size: %2;
                font-weight: normal;
            }
            QPushButton:hover {
                background: %3;
                border-radius: %4;
            }
        )")
        .arg(c.text_secondary, px(18), c.bg_hover, px(4));
}
}
