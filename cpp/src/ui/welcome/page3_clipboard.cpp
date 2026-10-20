#include "clipboard/ui/theme/themes.h"
#include "core/i18n.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "settings/tool_settings.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/welcome/pages.h"
#include <QFontMetrics>
#include <algorithm>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTimer>

namespace jietuba {
namespace {
QString settings_tr(const char *text) { return i18n_tr(text, "SettingsDialog"); }
QString clipboard_tr(const char *text) { return i18n_tr(text, "ClipboardWindow"); }

// Reuses the existing translation without forcing the description onto two lines.
QString clipboard_subtitle() { return welcome_tr("自动记录每一次复制，随时召唤历史内容。\n支持文本、图片、文件，还能分组管理。").replace("\n", " "); }

QString page_title() { return welcome_tr("剪贴板管理"); }

// The illustration's semantic colours, taken from the real clipboard theme so the preview
// cannot drift from the clipboard window.
struct Palette {
    QColor primary, secondary, alternate, hover, selected, text, muted, border, accent, shortcut, danger, success;
};

Palette current_palette() {
    const auto &colors = clipboard::get_theme_manager()->get_current_theme().colors;
    return {QColor(colors.bg_primary),   QColor(colors.bg_secondary), QColor(colors.bg_tertiary),        QColor(colors.bg_hover),
            QColor(colors.bg_selected),  QColor(colors.text_primary), QColor(colors.text_tertiary),      QColor(colors.border_primary),
            QColor(colors.accent_primary), QColor(colors.shortcut_key_color), QColor(colors.error), QColor(colors.success)};
}

QColor with_alpha(const QColor &color, int alpha) {
    QColor result(color);
    result.setAlpha(std::max(0, std::min(255, alpha)));
    return result;
}

struct Row {
    QString kind, text, metadata;
};

// Content types, groups and quick launch, laid out like the real clipboard window.
class ClipboardFeatureAnimation : public QWidget {
    Q_OBJECT
public:
    static constexpr int DURATION_MS = 18200;
    // Font sizes are drawn in proportion to the real window's default.
    static constexpr int BASE_FONT_SIZE = 17;
    static constexpr int TYPE_SCAN_END_MS = 6000;
    static constexpr int CONTENT_GROUP_START_MS = 7200;
    static constexpr int QUICK_FOCUS_START_MS = 9000;
    static constexpr int QUICK_GROUP_START_MS = 10200;
    static constexpr int FILTER_FOCUS_START_MS = 13000;
    static constexpr int FILTER_GROUP_START_MS = 14200;

    explicit ClipboardFeatureAnimation(QWidget *parent = nullptr) : QWidget(parent) {
        set_own_style(this, "background: transparent;");
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMinimumHeight(dialog_scaled(280));
        refresh_text();
        connect_clipboard_appearance();
        timer_ = new QTimer(this);
        connect(timer_, &QTimer::timeout, this, &ClipboardFeatureAnimation::tick);
        timer_->start(30);
    }

    void retranslate() {
        refresh_text();
        update();
    }

    // Sets the demo time, for previews and tests that check each phase.
    void set_animation_time(int elapsed_ms) {
        elapsed_ms_ = std::max(0, elapsed_ms) % DURATION_MS;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        double fade = 1.0;
        if (elapsed_ms_ < 350)
            fade = std::max(0.05, elapsed_ms_ / 350.0);
        else if (elapsed_ms_ > DURATION_MS - 500)
            fade = std::max(0.05, (DURATION_MS - elapsed_ms_) / 500.0);
        // The window opacity is the clipboard window's own, so the preview shows it too.
        p.setOpacity(fade * opacity_factor_);
        const Palette palette = current_palette();

        // A label band is kept free above the window, so labels never cover its content.
        const double label_band = 34.0;
        const double max_canvas_height = std::max(120.0, height() - label_band - 4.0);
        const double canvas_width = std::min(std::max(80.0, width() - 12.0), max_canvas_height / 1.34);
        const double canvas_height = std::min(max_canvas_height, canvas_width * 1.34);
        const QRectF canvas((width() - canvas_width) / 2, label_band + (height() - label_band - canvas_height) / 2, canvas_width, canvas_height);

        // The real window has only a 4px radius and a 1px border.
        const QRectF shadow = canvas.translated(0, 3);
        p.setBrush(with_alpha(QColor("#000000"), 20));
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(shadow, 4, 4);
        p.setBrush(palette.primary);
        p.setPen(QPen(palette.border, 1));
        p.drawRoundedRect(canvas, 4, 4);

        // The group bar across the top: history, content groups, quick launch, add, close.
        const double toolbar_h = 38.0;
        const QRectF toolbar(canvas.x() + 1, canvas.y() + 1, canvas.width() - 2, toolbar_h);
        p.fillRect(toolbar, palette.secondary);
        p.setPen(QPen(palette.border, 1));
        p.drawLine(static_cast<int>(toolbar.left()), static_cast<int>(toolbar.bottom()), static_cast<int>(toolbar.right()), static_cast<int>(toolbar.bottom()));

        const double button_y = toolbar.y() + 5;
        const double button_size = 28.0;
        const QRectF clipboard_btn(toolbar.x() + 5, button_y, button_size, button_size);
        const double separator_x = clipboard_btn.right() + 5;
        p.setPen(QPen(palette.border, 1));
        p.drawLine(static_cast<int>(separator_x), static_cast<int>(toolbar.y() + 7), static_cast<int>(separator_x), static_cast<int>(toolbar.bottom() - 7));
        const QRectF content_btn(separator_x + 6, button_y, button_size, button_size);
        const QRectF quick_btn(content_btn.right() + 5, button_y, button_size, button_size);
        const QRectF filter_btn(quick_btn.right() + 5, button_y, button_size, button_size);
        const QRectF add_btn(filter_btn.right() + 5, button_y, button_size, button_size);
        const QRectF close_btn(toolbar.right() - button_size - 5, button_y, button_size, button_size);

        const int elapsed = elapsed_ms_;
        const bool content_selected = CONTENT_GROUP_START_MS <= elapsed && elapsed < QUICK_GROUP_START_MS;
        const bool quick_selected = QUICK_GROUP_START_MS <= elapsed && elapsed < FILTER_GROUP_START_MS;
        const bool filter_selected = elapsed >= FILTER_GROUP_START_MS;
        const bool content_focused = TYPE_SCAN_END_MS <= elapsed && elapsed < CONTENT_GROUP_START_MS;
        const bool quick_focused = QUICK_FOCUS_START_MS <= elapsed && elapsed < QUICK_GROUP_START_MS;
        const bool filter_focused = FILTER_FOCUS_START_MS <= elapsed && elapsed < FILTER_GROUP_START_MS;
        draw_toolbar_button(p, clipboard_btn, "clipboard", palette, !content_selected && !quick_selected && !filter_selected);
        draw_toolbar_button(p, content_btn, "folder", palette, content_selected, content_focused);
        draw_toolbar_button(p, quick_btn, "bolt", palette, quick_selected, quick_focused);
        draw_toolbar_button(p, filter_btn, "funnel", palette, filter_selected, filter_focused);
        draw_toolbar_button(p, add_btn, "plus", palette);
        draw_toolbar_button(p, close_btn, "close", palette);

        const QList<Row> *rows = &type_rows_;
        bool quick_group = false;
        if (elapsed < CONTENT_GROUP_START_MS) {
            rows = &type_rows_;
        } else if (elapsed < QUICK_GROUP_START_MS) {
            rows = &content_rows_;
        } else if (elapsed < FILTER_GROUP_START_MS) {
            rows = &quick_rows_;
            quick_group = true;
        } else {
            rows = &filter_rows_;
        }

        const double bottom_h = 36.0;
        const QRectF bottom_bar(canvas.x() + 1, canvas.bottom() - bottom_h, canvas.width() - 2, bottom_h - 1);
        const double list_top = toolbar.bottom() + 1;
        const double list_bottom = bottom_bar.top();
        p.fillRect(QRectF(canvas.x() + 1, list_top, canvas.width() - 2, list_bottom - list_top), palette.primary);

        const double row_height = std::min(58.0, std::max(46.0, (list_bottom - list_top) / 4.15));
        int scan_index = -1;
        double scan_progress = 0.0;
        if (elapsed < TYPE_SCAN_END_MS) {
            scan_index = std::min(2, elapsed / 2000);
            scan_progress = (elapsed % 2000) / 2000.0;
        }

        const int transition_ms = 260;
        if (CONTENT_GROUP_START_MS <= elapsed && elapsed < CONTENT_GROUP_START_MS + transition_ms) {
            const double mix = static_cast<double>(elapsed - CONTENT_GROUP_START_MS) / transition_ms;
            draw_rows(p, type_rows_, canvas, list_top, row_height, palette, 1.0 - mix);
            draw_rows(p, content_rows_, canvas, list_top, row_height, palette, mix);
        } else if (QUICK_GROUP_START_MS <= elapsed && elapsed < QUICK_GROUP_START_MS + transition_ms) {
            const double mix = static_cast<double>(elapsed - QUICK_GROUP_START_MS) / transition_ms;
            draw_rows(p, content_rows_, canvas, list_top, row_height, palette, 1.0 - mix);
            draw_rows(p, quick_rows_, canvas, list_top, row_height, palette, mix, -1, 0.0, true);
        } else if (FILTER_GROUP_START_MS <= elapsed && elapsed < FILTER_GROUP_START_MS + transition_ms) {
            const double mix = static_cast<double>(elapsed - FILTER_GROUP_START_MS) / transition_ms;
            draw_rows(p, quick_rows_, canvas, list_top, row_height, palette, 1.0 - mix, -1, 0.0, true);
            draw_rows(p, filter_rows_, canvas, list_top, row_height, palette, mix);
        } else {
            draw_rows(p, *rows, canvas, list_top, row_height, palette, 1.0, scan_index, scan_progress, quick_group);
        }

        draw_bottom_bar(p, bottom_bar, palette);

        // A label above the window always names the current area, in the same place and style.
        QRectF label_target;
        QString label_text;
        if (elapsed < TYPE_SCAN_END_MS) {
            label_target = clipboard_btn;
            label_text = history_group_;
        } else if (elapsed < QUICK_FOCUS_START_MS) {
            label_target = content_btn;
            label_text = content_group_;
        } else if (elapsed < FILTER_FOCUS_START_MS) {
            label_target = quick_btn;
            label_text = quick_group_;
        } else {
            label_target = filter_btn;
            label_text = filter_group_;
        }
        draw_group_label(p, label_target, canvas, label_text, palette);
    }

private:
    void refresh_text() {
        history_group_ = welcome_tr("历史记录");
        content_group_ = welcome_tr("内容分组");
        quick_group_ = welcome_tr("快速启动");
        filter_group_ = welcome_tr("筛选分组");
        search_ = clipboard_tr("Search");
        type_labels_ = {welcome_tr("文字"), welcome_tr("图片"), welcome_tr("文件")};
        type_rows_ = {
            {"text", welcome_tr("会议时间改为 14:00"), QString::fromUtf8("WeChat · 14:20")},
            {"image", welcome_tr("产品封面.png"), QString::fromUtf8("Photos · 14:18")},
            {"file", welcome_tr("项目说明.pdf"), QString::fromUtf8("Explorer · 14:15")},
        };
        content_rows_ = {
            {"text", welcome_tr("感谢您的联系"), QString::fromUtf8("Edge · 10:32")},
            {"image", welcome_tr("设计预览.png"), QString::fromUtf8("Photos · 10:18")},
            {"text", welcome_tr("下周一 10:00 开会"), QString::fromUtf8("Notepad · 09:45")},
        };
        // A filter group fills itself: here every image, wherever it was copied from.
        filter_rows_ = {
            {"image", welcome_tr("产品封面.png"), QString::fromUtf8("Photos · 14:18")},
            {"image", welcome_tr("设计预览.png"), QString::fromUtf8("Photos · 10:18")},
            {"image", welcome_tr("活动海报.png"), QString::fromUtf8("Edge · 09:12")},
        };
        quick_rows_ = {
            {"app_edge", "Microsoft Edge", ""},
            {"app_code", "Visual Studio Code", ""},
            {"app_note", "Notepad", ""},
        };
    }

    void tick() {
        elapsed_ms_ = (elapsed_ms_ + timer_->interval()) % DURATION_MS;
        update();
    }

    // The preview is derived state: it follows the theme manager's signals, which the settings
    // page sends too, so both stay in step. Connected with this widget as the receiver, so the
    // global manager drops the connections when the page goes.
    void connect_clipboard_appearance() {
        const auto *config = get_tool_settings_manager();
        on_font_size_changed(static_cast<int>(config->get_clipboard_font_size()));
        on_opacity_changed(static_cast<int>(config->get_clipboard_window_opacity()));
        auto *manager = clipboard::get_theme_manager();
        connect(manager, &clipboard::ThemeManager::theme_changed, this, [this] { update(); });
        connect(manager, &clipboard::ThemeManager::font_size_changed, this, &ClipboardFeatureAnimation::on_font_size_changed);
        connect(manager, &clipboard::ThemeManager::opacity_changed, this, &ClipboardFeatureAnimation::on_opacity_changed);
    }

    void on_font_size_changed(int size) {
        font_scale_ = std::max(0.5, std::min(2.0, static_cast<double>(size) / BASE_FONT_SIZE));
        update();
    }

    void on_opacity_changed(int percent) {
        // Larger means more transparent, 0 is opaque. A floor keeps the highest step from
        // painting an empty preview.
        percent = std::max(0, std::min(100, percent));
        opacity_factor_ = std::max(0.3, 1.0 - percent / 100.0);
        update();
    }

    // System font fallback keeps CJK text from turning into boxes; sizes follow the font size setting.
    QFont font(int pixel_size, bool bold = false) const {
        QFont result;
        result.setPixelSize(static_cast<int>(std::max<qint64>(7, python_round(pixel_size * font_scale_))));
        result.setBold(bold);
        return result;
    }

    void draw_centered_text(QPainter &p, const QRectF &rect, const QString &text, const QColor &color, int size = 12, bool bold = false) const {
        p.setPen(color);
        p.setFont(font(size, bold));
        p.drawText(rect, Qt::AlignCenter, text);
    }

    void draw_toolbar_button(QPainter &p, const QRectF &rect, const QString &icon, const Palette &palette, bool selected = false, bool focused = false) const {
        if (focused) {
            const double pulse = 0.5 + 0.5 * std::abs(((elapsed_ms_ % 900) / 450.0) - 1.0);
            const QRectF ring = rect.adjusted(-3, -3, 3, 3);
            p.setBrush(with_alpha(palette.accent, 22 + static_cast<int>(28 * pulse)));
            p.setPen(QPen(with_alpha(palette.accent, 90), 1));
            p.drawRoundedRect(ring, 5, 5);
        }
        if (selected) {
            p.setBrush(palette.selected);
            p.setPen(QPen(palette.danger, 1.5));
            p.drawRoundedRect(rect, 4, 4);
        }

        const QColor stroke = selected || focused ? palette.accent : palette.muted;
        const double cx = rect.center().x(), cy = rect.center().y();
        auto line = [&p](double x1, double y1, double x2, double y2) {
            p.drawLine(static_cast<int>(x1), static_cast<int>(y1), static_cast<int>(x2), static_cast<int>(y2));
        };
        if (icon == "clipboard") {
            const QRectF body(cx - 7, cy - 8, 14, 17);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(stroke, 1.5));
            p.drawRoundedRect(body, 2, 2);
            p.drawRoundedRect(QRectF(cx - 4, cy - 10, 8, 4), 1.5, 1.5);
            line(cx - 4, cy - 2, cx + 4, cy - 2);
            line(cx - 4, cy + 2, cx + 3, cy + 2);
        } else if (icon == "folder") {
            QPainterPath path;
            path.moveTo(cx - 9, cy - 6);
            path.lineTo(cx - 2, cy - 6);
            path.lineTo(cx + 1, cy - 3);
            path.lineTo(cx + 9, cy - 3);
            path.lineTo(cx + 8, cy + 7);
            path.lineTo(cx - 9, cy + 7);
            path.closeSubpath();
            p.setBrush(with_alpha(stroke, 45));
            p.setPen(QPen(stroke, 1.5));
            p.drawPath(path);
        } else if (icon == "bolt") {
            const QPolygonF bolt({QPointF(cx + 1, cy - 10), QPointF(cx - 7, cy + 1), QPointF(cx - 1, cy + 1), QPointF(cx - 3, cy + 10), QPointF(cx + 8, cy - 3),
                                  QPointF(cx + 2, cy - 3)});
            p.setBrush(stroke);
            p.setPen(Qt::NoPen);
            p.drawPolygon(bolt);
        } else if (icon == "funnel") {
            const QPolygonF funnel({QPointF(cx - 8, cy - 7), QPointF(cx + 8, cy - 7), QPointF(cx + 2, cy), QPointF(cx + 2, cy + 7), QPointF(cx - 2, cy + 5), QPointF(cx - 2, cy)});
            p.setBrush(with_alpha(stroke, 45));
            p.setPen(QPen(stroke, 1.5));
            p.drawPolygon(funnel);
        } else if (icon == "plus") {
            p.setPen(QPen(palette.success, 1.8));
            line(cx - 5, cy, cx + 5, cy);
            line(cx, cy - 5, cx, cy + 5);
            p.setPen(QPen(palette.success, 1, Qt::DashLine));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(rect.adjusted(2, 2, -2, -2), 3, 3);
        } else if (icon == "close") {
            p.setPen(QPen(stroke, 1.5));
            line(cx - 5, cy - 5, cx + 5, cy + 5);
            line(cx + 5, cy - 5, cx - 5, cy + 5);
        }
    }

    void draw_group_label(QPainter &p, const QRectF &target, const QRectF &canvas, const QString &text, const Palette &palette) const {
        const QFont label_font = font(11);
        const double label_width = QFontMetrics(label_font).horizontalAdvance(text) + 18;
        const double center_x = target.center().x();
        const double left = std::max(2.0, std::min(width() - label_width - 2.0, center_x - label_width / 2));
        const QRectF rect(left, canvas.top() - 29, label_width, 24);
        p.setBrush(palette.primary);
        p.setPen(QPen(palette.border, 1));
        p.drawRoundedRect(rect, 4, 4);
        draw_centered_text(p, rect, text, palette.text, 11);

        // The label sits in the margin outside the window; a small arrow points at the group icon.
        const double pointer_x = std::max(rect.left() + 8, std::min(rect.right() - 8, center_x));
        const QPolygonF pointer({QPointF(pointer_x - 4, rect.bottom()), QPointF(pointer_x + 4, rect.bottom()), QPointF(center_x, canvas.top() - 1)});
        p.setBrush(palette.primary);
        p.setPen(QPen(palette.border, 1));
        p.drawPolygon(pointer);
    }

    static void draw_image_thumbnail(QPainter &p, const QRectF &rect, const Palette &palette) {
        p.setBrush(QColor("#DDECF4"));
        p.setPen(Qt::NoPen);
        p.drawRect(rect);
        p.setBrush(QColor("#F3B75D"));
        p.drawEllipse(QRectF(rect.right() - 12, rect.y() + 5, 6, 6));
        const QPolygonF hill({QPointF(rect.x(), rect.bottom()), QPointF(rect.x() + 11, rect.y() + 17), QPointF(rect.x() + 18, rect.y() + 23),
                              QPointF(rect.x() + 27, rect.y() + 13), QPointF(rect.right(), rect.bottom())});
        p.setBrush(QColor("#74B98A"));
        p.drawPolygon(hill);
        p.setPen(QPen(palette.border, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(rect);
    }

    static void draw_file_icon(QPainter &p, const QRectF &rect) {
        const QColor color("#E5A23E");
        QPainterPath path;
        path.moveTo(rect.x() + 2, rect.y() + 8);
        path.lineTo(rect.x() + 10, rect.y() + 8);
        path.lineTo(rect.x() + 13, rect.y() + 11);
        path.lineTo(rect.right() - 2, rect.y() + 11);
        path.lineTo(rect.right() - 3, rect.bottom() - 3);
        path.lineTo(rect.x() + 2, rect.bottom() - 3);
        path.closeSubpath();
        p.setBrush(with_alpha(color, 65));
        p.setPen(QPen(color, 1.3));
        p.drawPath(path);
    }

    void draw_row(QPainter &p, const QRectF &rect, int index, const Row &row, const Palette &palette, bool active, const QString &type_label, double scan_progress,
                  bool quick_group) const {
        const QColor background = active ? palette.selected : (index % 2 == 0 ? palette.primary : palette.alternate);
        p.fillRect(rect, background);
        p.setPen(QPen(with_alpha(palette.border, 150), 1));
        p.drawLine(static_cast<int>(rect.left()), static_cast<int>(rect.bottom()), static_cast<int>(rect.right()), static_cast<int>(rect.bottom()));

        if (active) {
            p.fillRect(QRectF(rect.x(), rect.y(), 3, rect.height()), palette.accent);
            const double scan_x = rect.x() + rect.width() * scan_progress;
            p.setPen(QPen(with_alpha(palette.accent, 125), 1.5));
            p.drawLine(static_cast<int>(scan_x), static_cast<int>(rect.top() + 3), static_cast<int>(scan_x), static_cast<int>(rect.bottom() - 3));
        }

        const QRectF badge_rect(rect.x() + 4, rect.y(), 27, rect.height());
        draw_centered_text(p, badge_rect, QString("%1:").arg(index + 1), palette.shortcut, 12, true);

        double content_left = rect.x() + 34;
        if (row.kind == "image") {
            const double thumb_size = std::min(38.0, rect.height() - 10);
            const QRectF thumb(content_left, rect.y() + (rect.height() - thumb_size) / 2, thumb_size, thumb_size);
            draw_image_thumbnail(p, thumb, palette);
            content_left = thumb.right() + 7;
        } else if (row.kind == "file" && !quick_group) {
            const QRectF icon_rect(content_left, rect.y() + (rect.height() - 28) / 2, 28, 28);
            draw_file_icon(p, icon_rect);
            content_left = icon_rect.right() + 5;
        }

        double type_width = 0;
        if (!type_label.isEmpty()) {
            const QFont label_font = font(12, true);
            type_width = QFontMetrics(label_font).horizontalAdvance(type_label) + 14;
            const int alpha = static_cast<int>(255 * std::min({1.0, scan_progress * 5.0, (1.0 - scan_progress) * 5.0}));
            const QRectF type_rect(rect.right() - type_width - 8, rect.y() + 5, type_width, 20);
            p.setPen(with_alpha(palette.accent, alpha));
            p.setFont(label_font);
            p.drawText(type_rect, Qt::AlignRight | Qt::AlignVCenter, type_label);
        }

        const QRectF text_rect(content_left, rect.y() + 5, std::max(20.0, rect.right() - content_left - type_width - 9), 21);
        const QFont text_font = font(13);
        const QString shown = QFontMetrics(text_font).elidedText(row.text, Qt::ElideRight, static_cast<int>(text_rect.width()));
        p.setFont(text_font);
        p.setPen(palette.text);
        p.drawText(text_rect, Qt::AlignVCenter, shown);

        if (!row.metadata.isEmpty()) {
            p.setFont(font(9));
            p.setPen(palette.muted);
            const QRectF meta_rect(rect.x() + 34, rect.bottom() - 20, rect.width() - 42, 15);
            p.drawText(meta_rect, Qt::AlignRight | Qt::AlignVCenter, row.metadata);
        }
    }

    void draw_bottom_bar(QPainter &p, const QRectF &rect, const Palette &palette) const {
        p.fillRect(rect, palette.secondary);
        p.setPen(QPen(palette.border, 1));
        p.drawLine(static_cast<int>(rect.left()), static_cast<int>(rect.top()), static_cast<int>(rect.right()), static_cast<int>(rect.top()));

        // The time filter button (the up arrow on the real window's left).
        const double arrow_x = rect.x() + 17;
        const double arrow_y = rect.center().y();
        p.setPen(QPen(palette.muted, 1.5));
        p.drawLine(static_cast<int>(arrow_x - 4), static_cast<int>(arrow_y + 2), static_cast<int>(arrow_x), static_cast<int>(arrow_y - 2));
        p.drawLine(static_cast<int>(arrow_x), static_cast<int>(arrow_y - 2), static_cast<int>(arrow_x + 4), static_cast<int>(arrow_y + 2));

        // The flat search field.
        const double search_left = rect.x() + 34;
        const double search_right = rect.right() - 34;
        const double cy = rect.center().y();
        p.setPen(QPen(palette.muted, 1.2));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRectF(search_left + 3, cy - 5, 8, 8));
        p.drawLine(static_cast<int>(search_left + 10), static_cast<int>(cy + 2), static_cast<int>(search_left + 14), static_cast<int>(cy + 6));
        const QRectF search_rect(search_left + 19, rect.y(), search_right - search_left - 19, rect.height());
        p.setFont(font(11));
        p.setPen(palette.muted);
        p.drawText(search_rect, Qt::AlignVCenter, search_);

        // The settings gear at the real window's compact position, as plain rings.
        const QPointF gear_center(rect.right() - 17, cy);
        p.setPen(QPen(palette.muted, 1.3));
        p.drawEllipse(gear_center, 6, 6);
        p.drawEllipse(gear_center, 2, 2);
    }

    void draw_rows(QPainter &p, const QList<Row> &rows, const QRectF &canvas, double list_top, double row_height, const Palette &palette, double opacity = 1.0,
                   int scan_index = -1, double scan_progress = 0.0, bool quick_group = false) const {
        p.save();
        p.setOpacity(opacity);
        for (int index = 0; index < rows.size(); ++index) {
            const QRectF rect(canvas.x() + 1, list_top + index * row_height, canvas.width() - 2, row_height);
            const bool active = index == scan_index;
            draw_row(p, rect, index, rows[index], palette, active, active ? type_labels_.value(index) : QString(), scan_progress, quick_group);
        }
        p.restore();
    }

    int elapsed_ms_ = 0;
    double font_scale_ = 1.0;
    double opacity_factor_ = 1.0;
    QTimer *timer_ = nullptr;
    QString history_group_, content_group_, quick_group_, filter_group_, search_;
    QStringList type_labels_;
    QList<Row> type_rows_, content_rows_, quick_rows_, filter_rows_;
};

// The non-interactive clipboard demo on the page's left.
class ClipboardFeatureIllus : public IllustrationArea {
    Q_OBJECT
public:
    using IllustrationArea::IllustrationArea;
    void retranslate() override { animation->retranslate(); }
    void apply_welcome_theme() override {
        IllustrationArea::apply_welcome_theme();
        if (animation) animation->update();
    }
    ClipboardFeatureAnimation *animation = nullptr;
protected:
    void build_content() override {
        layout_->setContentsMargins(dialog_scaled(10), dialog_scaled(10), dialog_scaled(10), dialog_scaled(10));
        animation = new ClipboardFeatureAnimation(this);
        layout_->addWidget(animation, 1);
    }
};

// A row of theme swatches, applied on click. The settings window uses one swatch and a menu;
// here they are laid out flat beside the live preview. Both use PRESET_THEME_SWATCHES.
class ThemeSwatchRow : public QWidget {
    Q_OBJECT
public:
    static constexpr int SWATCH_W = 30;
    static constexpr int SWATCH_H = 24;

    ThemeSwatchRow(const QString &current, std::function<void(const QString &)> on_pick, QWidget *parent = nullptr) : QWidget(parent), on_pick_(std::move(on_pick)) {
        set_own_style(this, "background: transparent;");
        const auto &swatches = clipboard::PRESET_THEME_SWATCHES();
        current_ = std::any_of(swatches.begin(), swatches.end(), [&current](const auto &entry) { return entry.first == current; }) ? current : QString("light");
        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(dialog_scaled(6));
        for (const auto &[name, swatch] : swatches) {
            auto *button = new QPushButton(this);
            button->setFixedSize(dialog_scaled(SWATCH_W), dialog_scaled(SWATCH_H));
            button->setCursor(Qt::PointingHandCursor);
            button->setToolTip(name);
            connect(button, &QPushButton::clicked, this, [this, theme_name = name] { pick(theme_name); });
            buttons_.push_back({name, button, swatch.first, swatch.second});
            row->addWidget(button);
        }
        row->addStretch();
        restyle();
    }
    QString current() const { return current_; }
    // BasePage calls this hook on its children when the theme changes.
    Q_INVOKABLE void apply_welcome_child_theme() { restyle(); }

private:
    struct Swatch {
        QString name;
        QPushButton *button;
        QString accent, background;
    };
    void pick(const QString &name) {
        if (name == current_) return;
        current_ = name;
        restyle();
        on_pick_(name);
    }
    void restyle() {
        const auto theme = welcome_theme();
        for (const auto &swatch : buttons_) {
            const bool selected = swatch.name == current_;
            const QString border = selected ? theme.accent : theme.border_strong;
            const int border_width = selected ? 2 : 1;
            swatch.button->setStyleSheet(QString(R"(
                QPushButton {
                    background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                        stop:0 %1, stop:0.5 %1,
                        stop:0.5 %2, stop:1 %2);
                    border: %3px solid %4;
                    border-radius: %5px;
                }
                QPushButton:hover { border: 2px solid %6; }
            )")
                                             .arg(swatch.background, swatch.accent)
                                             .arg(border_width)
                                             .arg(border)
                                             .arg(dialog_scaled(4))
                                             .arg(theme.accent));
        }
    }
    std::function<void(const QString &)> on_pick_;
    QString current_;
    std::vector<Swatch> buttons_;
};
}

ClipboardHotkeyPage::ClipboardHotkeyPage(ToolSettingsManager *config, QWidget *parent) : BasePage(page_title(), clipboard_subtitle(), parent), config_(config) { init(); }

IllustrationArea *ClipboardHotkeyPage::create_illustration() { return new ClipboardFeatureIllus(this); }

void ClipboardHotkeyPage::build_controls(QVBoxLayout *layout) {
    history_limit_lbl_ = new QLabel(settings_tr("History Limit"));
    set_welcome_label_style(history_limit_lbl_, "primary", 14, 600);
    layout->addWidget(history_limit_lbl_);

    history_limit_desc_ = new QLabel(settings_tr("Maximum number of items to keep (0 = unlimited)"));
    history_limit_desc_->setWordWrap(true);
    set_welcome_label_style(history_limit_desc_, "muted", 12, 400);
    layout->addWidget(history_limit_desc_);
    layout->addSpacing(dialog_scaled(4));

    history_limit_spin_ = new SpinBox;
    history_limit_spin_->setRange(0, 10000);
    history_limit_spin_->setValue(static_cast<int>(config_->get_clipboard_history_limit()));
    history_limit_spin_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(history_limit_spin_, &QSpinBox::valueChanged, this, [this](int value) { config_->set_clipboard_history_limit(value); });
    layout->addWidget(history_limit_spin_);

    build_appearance_controls(layout);
}

// Theme, font size and opacity apply at once through the theme manager, which saves and
// broadcasts them; the preview follows the same signals, so save() does not write them again.
void ClipboardHotkeyPage::build_appearance_controls(QVBoxLayout *layout) {
    auto *theme_manager = clipboard::get_theme_manager();

    layout->addSpacing(dialog_scaled(14));
    appearance_lbl_ = new QLabel(welcome_tr("剪贴板外观"));
    set_welcome_label_style(appearance_lbl_, "primary", 14, 600);
    layout->addWidget(appearance_lbl_);
    layout->addSpacing(dialog_scaled(6));

    // The current theme comes from the theme manager, which also writes it, so the
    // highlighted swatch and the saved theme cannot disagree.
    auto *theme_row = new ThemeSwatchRow(theme_manager->get_current_theme().name, [theme_manager](const QString &name) { theme_manager->set_theme(name); });
    theme_row_ = theme_row;
    QWidget *row = nullptr;
    QLabel *unused = nullptr;
    std::tie(row, theme_lbl_, unused) = make_setting_row_with_refs(settings_tr("Theme"), theme_row);
    layout->addWidget(row);

    layout->addSpacing(dialog_scaled(8));
    font_combo_ = new ComboBox;
    font_combo_->setFixedWidth(dialog_scaled(124));
    font_combo_->setFixedHeight(dialog_scaled(32));
    font_combo_->setCursor(Qt::PointingHandCursor);
    for (const QVariant &size : config_->get_clipboard_font_size_options().toList()) font_combo_->addItem(QString("%1px").arg(size.toLongLong()), size);
    if (const int index = font_combo_->findData(config_->get_clipboard_font_size()); index >= 0) font_combo_->setCurrentIndex(index);
    connect(font_combo_, &QComboBox::currentIndexChanged, this, &ClipboardHotkeyPage::on_font_size_picked);
    std::tie(row, font_lbl_, unused) = make_setting_row_with_refs(settings_tr("Font Size"), font_combo_);
    layout->addWidget(row);

    layout->addSpacing(dialog_scaled(8));
    opacity_combo_ = new ComboBox;
    opacity_combo_->setFixedWidth(dialog_scaled(124));
    opacity_combo_->setFixedHeight(dialog_scaled(32));
    opacity_combo_->setCursor(Qt::PointingHandCursor);
    opacity_options_ = config_->get_clipboard_window_opacity_options().toList();
    populate_opacity_combo();
    if (const int index = opacity_combo_->findData(config_->get_clipboard_window_opacity()); index >= 0) opacity_combo_->setCurrentIndex(index);
    connect(opacity_combo_, &QComboBox::currentIndexChanged, this, &ClipboardHotkeyPage::on_opacity_picked);
    std::tie(row, opacity_lbl_, unused) = make_setting_row_with_refs(settings_tr("Opacity"), opacity_combo_);
    layout->addWidget(row);
}

// Step 0 reads "Opaque" rather than 0%, as in the settings window.
void ClipboardHotkeyPage::populate_opacity_combo() {
    const bool blocked = opacity_combo_->blockSignals(true);
    const QVariant current = opacity_combo_->currentData();
    opacity_combo_->clear();
    for (const QVariant &percent : opacity_options_)
        opacity_combo_->addItem(percent.toLongLong() == 0 ? settings_tr("Opaque") : QString("%1%").arg(percent.toLongLong()), percent);
    if (current.isValid())
        if (const int index = opacity_combo_->findData(current); index >= 0) opacity_combo_->setCurrentIndex(index);
    opacity_combo_->blockSignals(blocked);
}

void ClipboardHotkeyPage::on_font_size_picked(int index) {
    const QVariant size = font_combo_->itemData(index);
    if (!size.isValid()) return;
    config_->set_clipboard_font_size(size);
    clipboard::get_theme_manager()->notify_font_size_changed(size.toInt());
}

void ClipboardHotkeyPage::on_opacity_picked(int index) {
    const QVariant percent = opacity_combo_->itemData(index);
    if (!percent.isValid()) return;
    config_->set_clipboard_window_opacity(percent);
    clipboard::get_theme_manager()->notify_opacity_changed(percent.toInt());
}

void ClipboardHotkeyPage::retranslate() {
    title_label->setText(page_title());
    subtitle_label->setText(clipboard_subtitle());
    appearance_lbl_->setText(welcome_tr("剪贴板外观"));
    theme_lbl_->setText(settings_tr("Theme"));
    font_lbl_->setText(settings_tr("Font Size"));
    opacity_lbl_->setText(settings_tr("Opacity"));
    // "Opaque" is translated, so the options are rebuilt on a language change.
    populate_opacity_combo();
    history_limit_lbl_->setText(settings_tr("History Limit"));
    history_limit_desc_->setText(settings_tr("Maximum number of items to keep (0 = unlimited)"));
    // Cascades to the illustration's types, groups and sample text.
    illus_area->retranslate();
}

void ClipboardHotkeyPage::save() { config_->set_clipboard_history_limit(history_limit_spin_->value()); }

void ClipboardHotkeyPage::set_animation_time(int elapsed_ms) {
    if (auto *illus = qobject_cast<ClipboardFeatureIllus *>(illus_area); illus && illus->animation) illus->animation->set_animation_time(elapsed_ms);
}
}

#include "page3_clipboard.moc"
