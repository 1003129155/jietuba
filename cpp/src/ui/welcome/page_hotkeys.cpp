#include "core/logger.h"
#include "core/ui_scale.h"
#include "settings/tool_settings.h"
#include "ui/hotkey_edit.h"
#include "ui/welcome/pages.h"
#include <QApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QPainter>
#include <QSet>

namespace jietuba {
namespace {
// Whether a key can be part of a global hotkey.
constexpr bool BINDABLE = true;
constexpr bool UNBOUND = false;

// Key cap label -> the token the editor writes (lower case); only these differ.
QString label_token(const QString &label) {
    static const QHash<QString, QString> overrides{{QString::fromUtf8("⌫"), "backspace"}, {QString::fromUtf8("↵"), "enter"}, {"Caps", "capslock"}};
    return overrides.value(label, label.toLower());
}

// "ctrl+shift+a" as key cap tokens. Halfway through a combination the editor holds "ctrl+",
// whose split leaves an empty part, dropped here.
QSet<QString> pressed_tokens(const QString &hotkey_text) {
    QSet<QString> result;
    for (const auto &part : hotkey_text.trimmed().toLower().split('+'))
        if (!part.isEmpty()) result.insert(part);
    return result;
}

struct Key {
    QString label;
    double width;
    bool bindable;
};

// A static availability map: bindable keys highlighted, others greyed. It follows the key
// table in core/global_hotkey: every key but Caps can be the main key of a combination.
// Change it with the table.
class KeyboardMap : public QWidget {
public:
    // Rows of (label, width in letter-key units, bindable).
    static const std::vector<std::vector<Key>> &rows() {
        static const std::vector<std::vector<Key>> keyboard = [] {
            std::vector<std::vector<Key>> rows;
            std::vector<Key> row{{"Esc", 2, BINDABLE}};
            for (int n = 1; n <= 12; ++n) row.push_back({QString("F%1").arg(n), 1, BINDABLE});
            rows.push_back(row);
            row = {{"`", 1, BINDABLE}};
            for (QChar c : QString("1234567890")) row.push_back({c, 1, BINDABLE});
            row.insert(row.end(), {{"-", 1, BINDABLE}, {"=", 1, BINDABLE}, {QString::fromUtf8("⌫"), 2, BINDABLE}});
            rows.push_back(row);
            row = {{"Tab", 1.5, BINDABLE}};
            for (QChar c : QString("QWERTYUIOP")) row.push_back({c, 1, BINDABLE});
            row.insert(row.end(), {{"[", 1, BINDABLE}, {"]", 1, BINDABLE}, {"\\", 1.5, BINDABLE}});
            rows.push_back(row);
            row = {{"Caps", 1.75, UNBOUND}};
            for (QChar c : QString("ASDFGHJKL")) row.push_back({c, 1, BINDABLE});
            row.insert(row.end(), {{";", 1, BINDABLE}, {"'", 1, BINDABLE}, {QString::fromUtf8("↵"), 2.25, BINDABLE}});
            rows.push_back(row);
            row = {{"Shift", 2.25, BINDABLE}};
            for (QChar c : QString("ZXCVBNM")) row.push_back({c, 1, BINDABLE});
            row.insert(row.end(), {{",", 1, BINDABLE}, {".", 1, BINDABLE}, {"/", 1, BINDABLE}, {"Shift", 2.75, BINDABLE}});
            rows.push_back(row);
            rows.push_back({{"Ctrl", 2, BINDABLE}, {"Win", 1.5, BINDABLE}, {"Alt", 1.5, BINDABLE}, {"Space", 6.5, BINDABLE}, {"Alt", 1.5, BINDABLE}, {"Ctrl", 2, BINDABLE}});
            return rows;
        }();
        return keyboard;
    }
    // The widest row's units.
    static constexpr double GRID_UNITS = 15;
    static constexpr double KEY_GAP = 1.6;
    static constexpr double ROW_GAP = 2.4;
    static constexpr double LEGEND_SWATCH_W = 14;
    static constexpr double LEGEND_TEXT_GAP = 5;
    static constexpr double LEGEND_ITEM_GAP = 14;
    static constexpr double LEGEND_LINE_H = 16;

    explicit KeyboardMap(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumHeight(dialog_scaled(230));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    // Highlights the keys of a hotkey; empty clears. It follows the editor's text rather than
    // physical key state: keyboard combinations, system hotkeys and side buttons all end as
    // that text, so one path covers all three without tracking which keys are held.
    void set_highlight(const QString &hotkey_text) {
        const auto pressed = pressed_tokens(hotkey_text);
        if (pressed == pressed_) return;
        pressed_ = pressed;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        const auto theme = welcome_theme();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF rect(this->rect());
        const double unit = (rect.width() - KEY_GAP * (GRID_UNITS - 1)) / GRID_UNITS;
        const double key_h = std::max(13.0, unit * 1.06);
        const double keyboard_h = rows().size() * key_h + (rows().size() - 1) * ROW_GAP;
        const double mouse_h = 74.0;
        const double legend_h = legend_layout(p, theme, rect.width() - 8).size() * LEGEND_LINE_H;
        const double block_h = keyboard_h + 22 + mouse_h + 8 + legend_h;
        const double top = std::max(rect.top(), rect.center().y() - block_h / 2);
        draw_keyboard(p, theme, rect, top, unit, key_h);
        draw_mouse(p, theme, rect.center().x(), top + keyboard_h + 22);
        draw_legend(p, theme, rect, top + keyboard_h + 22 + mouse_h + 8);
        p.end();
    }

private:
    // System fallback fonts, so Chinese, Japanese and Korean never show as boxes.
    static QFont font(int pixel_size, bool bold = false) {
        QFont f;
        f.setPixelSize(pixel_size);
        f.setBold(bold);
        return f;
    }
    static QColor with_alpha(const QString &color, int alpha) {
        QColor result(color);
        result.setAlpha(std::max(0, std::min(255, alpha)));
        return result;
    }
    // Key colours for pressed / bindable / unbindable. Not accent_soft against panel_subtle:
    // in the light theme they are nearly equal and the map would blur; translucent accent
    // keeps contrast in both themes, and a pressed key is solid.
    static std::tuple<QColor, QColor, QString> key_colors(const WelcomeTheme &theme, bool bindable, bool pressed = false) {
        if (pressed) return {QColor(theme.accent), QColor(theme.accent), "#FFFFFF"};
        if (bindable) return {with_alpha(theme.accent, 58), with_alpha(theme.accent, 170), theme.text};
        return {with_alpha(theme.text_soft, 26), with_alpha(theme.text_soft, 90), theme.text_soft};
    }

    // A font size that fits this key cap: F12 is twice as wide as Q, so one size never fits all.
    QFont fitted_font(QPainter &p, const QString &label, const QRectF &box) {
        const auto cache_key = qMakePair(label, static_cast<int>(std::round(box.width())));
        if (const auto it = font_cache_.find(cache_key); it != font_cache_.end()) return *it;
        int size = std::max(6, static_cast<int>(box.height() * 0.44));
        QFont f = font(size);
        const double available = box.width() - 3;
        while (size > 5) {
            f.setPixelSize(size);
            p.setFont(f);
            if (p.fontMetrics().horizontalAdvance(label) <= available) break;
            --size;
        }
        font_cache_.insert(cache_key, f);
        return f;
    }

    void draw_keyboard(QPainter &p, const WelcomeTheme &theme, const QRectF &rect, double top, double unit, double key_h) {
        for (size_t row_index = 0; row_index < rows().size(); ++row_index) {
            const auto &row = rows()[row_index];
            double row_units = 0;
            for (const auto &key : row) row_units += key.width;
            const double row_w = row_units * unit + KEY_GAP * (row.size() - 1);
            double x = rect.center().x() - row_w / 2;
            const double y = top + row_index * (key_h + ROW_GAP);
            for (const auto &key : row) {
                const double key_w = key.width * unit;
                draw_key(p, theme, QRectF(x, y, key_w, key_h), key.label, key.bindable);
                x += key_w + KEY_GAP;
            }
        }
    }

    void draw_key(QPainter &p, const WelcomeTheme &theme, const QRectF &box, const QString &label, bool bindable) {
        const bool pressed = pressed_.contains(label_token(label));
        const auto [fill, border, text] = key_colors(theme, bindable, pressed);
        p.setBrush(fill);
        p.setPen(QPen(border, pressed ? 1.6 : 1));
        p.drawRoundedRect(box, 2.5, 2.5);
        p.setFont(fitted_font(p, label, box));
        p.setPen(QColor(text));
        p.drawText(box, Qt::AlignCenter, label);
    }

    void draw_mouse(QPainter &p, const WelcomeTheme &theme, double center_x, double top) {
        const QRectF body(center_x - 25, top, 50, 74);
        p.setBrush(QColor(theme.panel_subtle));
        p.setPen(QPen(QColor(theme.border), 1.2));
        p.drawRoundedRect(body, 24, 24);
        // The button split and wheel only make it look like a mouse; neither is bindable.
        p.setPen(QPen(QColor(theme.border), 1));
        p.drawLine(static_cast<int>(body.center().x()), static_cast<int>(body.top() + 6), static_cast<int>(body.center().x()), static_cast<int>(body.top() + 27));
        p.setBrush(QColor(theme.border));
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(QRectF(body.center().x() - 3, body.top() + 10, 6, 13), 3, 3);
        // The two side buttons are bindable and use the keyboard's colours; the upper one,
        // nearer the front, is "forward", the lower one "back".
        const char *tokens[] = {"mouseforward", "mouseback"};
        for (int index = 0; index < 2; ++index) {
            const bool pressed = pressed_.contains(tokens[index]);
            const auto [fill, border, text] = key_colors(theme, true, pressed);
            Q_UNUSED(text);
            p.setBrush(fill);
            p.setPen(QPen(border, pressed ? 1.6 : 1.2));
            p.drawRoundedRect(QRectF(body.left() - 6, body.top() + 26 + index * 17, 10, 14), 3, 3);
        }
        // The label sits on the side-button side, right aligned: on the right it would read as the right button.
        p.setFont(font(10));
        p.setPen(QColor(theme.text_muted));
        const double label_right = body.left() - 12;
        p.drawText(QRectF(label_right - 110, body.top() + 26, 110, 20), Qt::AlignRight | Qt::AlignVCenter, welcome_tr("鼠标侧键"));
    }

    struct LegendEntry {
        QColor fill, border;
        QString text_color, text;
    };
    std::vector<LegendEntry> legend_entries(const WelcomeTheme &theme) const {
        const auto [bindable_fill, bindable_border, unused_text] = key_colors(theme, true);
        const auto [unbound_fill, unbound_border, unused_text_2] = key_colors(theme, false);
        Q_UNUSED(unused_text);
        Q_UNUSED(unused_text_2);
        std::vector<LegendEntry> entries{
            {bindable_fill, bindable_border, theme.text_muted, welcome_tr("可设为快捷键")},
            {unbound_fill, unbound_border, theme.text_soft, welcome_tr("不可用")},
        };
        // Without a pressed key this entry takes no space: an unseen colour needs no explanation.
        if (!pressed_.isEmpty()) {
            const auto [pressed_fill, pressed_border, unused_text_3] = key_colors(theme, true, true);
            Q_UNUSED(unused_text_3);
            entries.push_back({pressed_fill, pressed_border, theme.text_muted, welcome_tr("当前按下")});
        }
        return entries;
    }

    // Legend entries split into lines by the available width: translations differ a lot in
    // length, and one forced line would paint outside a narrow panel.
    std::vector<std::pair<std::vector<std::pair<LegendEntry, double>>, double>> legend_layout(QPainter &p, const WelcomeTheme &theme, double available_w) const {
        p.setFont(font(10));
        const auto metrics = p.fontMetrics();
        std::vector<std::pair<std::vector<std::pair<LegendEntry, double>>, double>> lines;
        std::vector<std::pair<LegendEntry, double>> current;
        double current_w = 0;
        for (const auto &entry : legend_entries(theme)) {
            const double width = LEGEND_SWATCH_W + LEGEND_TEXT_GAP + metrics.horizontalAdvance(entry.text);
            const double extra = current.empty() ? width : width + LEGEND_ITEM_GAP;
            if (!current.empty() && current_w + extra > available_w) {
                lines.emplace_back(current, current_w);
                current = {{entry, width}};
                current_w = width;
                continue;
            }
            current.emplace_back(entry, width);
            current_w += extra;
        }
        if (!current.empty()) lines.emplace_back(current, current_w);
        return lines;
    }

    void draw_legend(QPainter &p, const WelcomeTheme &theme, const QRectF &rect, double top) {
        const auto lines = legend_layout(p, theme, rect.width() - 8);
        for (size_t line_index = 0; line_index < lines.size(); ++line_index) {
            const auto &[items, line_w] = lines[line_index];
            double x = std::max(rect.left() + 4, rect.center().x() - line_w / 2);
            const double y = top + line_index * LEGEND_LINE_H;
            for (const auto &[entry, width] : items) {
                p.setBrush(entry.fill);
                p.setPen(QPen(entry.border, 1));
                p.drawRoundedRect(QRectF(x, y + 3, LEGEND_SWATCH_W, 10), 2, 2);
                p.setPen(QColor(entry.text_color));
                p.drawText(QRectF(x + LEGEND_SWATCH_W + LEGEND_TEXT_GAP, y, width, LEGEND_LINE_H), Qt::AlignLeft | Qt::AlignVCenter, entry.text);
                x += width + LEGEND_ITEM_GAP;
            }
        }
    }

    QSet<QString> pressed_;
    // Per-key fitted fonts by (label, width): the keyboard is static.
    QHash<QPair<QString, int>, QFont> font_cache_;
};

class HotkeyIllus : public IllustrationArea {
public:
    using IllustrationArea::IllustrationArea;
    void retranslate() override { keyboard_map->update(); }
    void apply_welcome_theme() override {
        IllustrationArea::apply_welcome_theme();
        if (keyboard_map) keyboard_map->update();
    }
    KeyboardMap *keyboard_map = nullptr;
protected:
    void build_content() override {
        layout_->setContentsMargins(dialog_scaled(10), dialog_scaled(10), dialog_scaled(10), dialog_scaled(10));
        keyboard_map = new KeyboardMap(this);
        layout_->addWidget(keyboard_map, 1);
    }
};

constexpr int SLOT_LABEL_W = 62;
}

const std::vector<HotkeyPage::Row> &HotkeyPage::rows() {
    static const std::vector<Row> rows = [] {
        return std::vector<Row>{
            {[](ToolSettingsManager *c) { return c->get_hotkey(); }, [](ToolSettingsManager *c, const QString &v) { c->set_hotkey(v); },
             [](ToolSettingsManager *c) { return c->get_hotkey_2(); }, [](ToolSettingsManager *c, const QString &v) { c->set_hotkey_2(v); }, "截图"},
            {[](ToolSettingsManager *c) { return c->get_clipboard_hotkey(); }, [](ToolSettingsManager *c, const QString &v) { c->set_clipboard_hotkey(v); },
             [](ToolSettingsManager *c) { return c->get_clipboard_hotkey_2(); }, [](ToolSettingsManager *c, const QString &v) { c->set_clipboard_hotkey_2(v); },
             "剪贴板"},
            {[](ToolSettingsManager *c) { return c->get_translation_hotkey(); }, [](ToolSettingsManager *c, const QString &v) { c->set_translation_hotkey(v); },
             [](ToolSettingsManager *c) { return c->get_translation_hotkey_2(); }, [](ToolSettingsManager *c, const QString &v) { c->set_translation_hotkey_2(v); },
             "智能翻译"},
        };
    }();
    return rows;
}

HotkeyPage::HotkeyPage(ToolSettingsManager *config, QWidget *parent)
    : BasePage(welcome_tr("快捷键"), welcome_tr("三个全局功能各可设置一个主快捷键和一个备用快捷键。"), parent), config_(config) {
    init();
}

HotkeyPage::~HotkeyPage() { disconnect(focus_connection_); }

IllustrationArea *HotkeyPage::create_illustration() { return new HotkeyIllus(this); }

void HotkeyPage::build_controls(QVBoxLayout *layout) {
    // Each feature has its own block with the title on its own line; a three-column grid
    // squeezed both editors beside the title and the status icon overlapped the input.
    for (size_t index = 0; index < rows().size(); ++index) {
        const auto &row_spec = rows()[index];
        if (index) layout->addSpacing(dialog_scaled(14));
        auto *section_lbl = new QLabel(welcome_tr(row_spec.title));
        set_welcome_label_style(section_lbl, "primary", 14, 600);
        layout->addWidget(section_lbl);
        section_labels_.emplace_back(section_lbl, row_spec.title);
        layout->addSpacing(dialog_scaled(6));

        std::vector<HotkeyEdit *> pair;
        for (const auto &[slot_title, getter] : {std::make_pair("主快捷键", row_spec.get), std::make_pair("备用", row_spec.get_2)}) {
            auto *edit = new HotkeyEdit;
            QString value;
            try {
                value = getter(config_);
            } catch (const std::exception &e) {
                log_exception(e, T("读取全局快捷键 {accessor}", {{"accessor", QString::fromUtf8(slot_title)}}));
            }
            edit->setText(value);
            edit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            auto *slot_lbl = new QLabel(welcome_tr(slot_title));
            slot_lbl->setFixedWidth(dialog_scaled(SLOT_LABEL_W));
            set_welcome_label_style(slot_lbl, "muted", 12, 400);
            slot_labels_.emplace_back(slot_lbl, slot_title);
            auto *row = new QHBoxLayout;
            row->setContentsMargins(0, 0, 0, 0);
            row->setSpacing(dialog_scaled(8));
            row->addWidget(slot_lbl);
            row->addWidget(edit, 1);
            layout->addLayout(row);
            if (QString::fromUtf8(slot_title) == QString::fromUtf8("主快捷键")) layout->addSpacing(dialog_scaled(6));
            edits_.push_back(edit);
            pair.push_back(edit);
        }
        row_edits_.emplace_back(pair[0], pair[1]);
    }

    layout->addSpacing(dialog_scaled(14));
    hint_ = new QLabel(welcome_tr("点击输入框后，按下快捷键或鼠标侧键即可录入。"));
    hint_->setWordWrap(true);
    set_welcome_label_style(hint_, "muted", 12, 400);
    layout->addWidget(hint_);

    // Conflicts are connected once every editor exists, never seeing half the group; a final
    // run flags old conflicts left in the configuration.
    for (auto *edit : edits_)
        connect(edit, &HotkeyEdit::textChanged, this, [this] {
            revalidate();
            sync_key_highlight();
        });
    revalidate();

    // The illustration follows the slot being edited, through the global focusChanged rather
    // than a new signal on the shared editor: this linkage belongs to this page only.
    focus_connection_ = connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) { on_focus_changed(now); });
}

void HotkeyPage::on_focus_changed(QWidget *now) {
    active_edit_ = nullptr;
    for (auto *edit : edits_)
        if (edit->edit == now) active_edit_ = edit;
    sync_key_highlight();
}

void HotkeyPage::sync_key_highlight() {
    auto *illus = static_cast<HotkeyIllus *>(illus_area);
    if (!illus->keyboard_map) return;
    illus->keyboard_map->set_highlight(active_edit_ ? active_edit_->text() : QString());
}

bool HotkeyPage::revalidate() { return validate_hotkey_group(edits_); }

void HotkeyPage::retranslate() {
    title_label->setText(welcome_tr("快捷键"));
    subtitle_label->setText(welcome_tr("三个全局功能各可设置一个主快捷键和一个备用快捷键。"));
    for (const auto &[label, text] : section_labels_) label->setText(welcome_tr(text));
    for (const auto &[label, text] : slot_labels_) label->setText(welcome_tr(text));
    if (hint_) hint_->setText(welcome_tr("点击输入框后，按下快捷键或鼠标侧键即可录入。"));
    // The conflict message follows the language too.
    revalidate();
    illus_area->retranslate();
}

void HotkeyPage::save() {
    const bool conflicted = !revalidate();
    for (size_t i = 0; i < rows().size(); ++i) {
        write(rows()[i].set, row_edits_[i].first, conflicted, true);
        write(rows()[i].set_2, row_edits_[i].second, conflicted, false);
    }
}

void HotkeyPage::write(const Setter &setter, HotkeyEdit *edit, bool conflicted, bool keep_when_blank) {
    if (conflicted && !edit->validation_error().isEmpty()) return;
    const QString value = edit->text().trimmed();
    // An empty main hotkey would leave no entry at all, so the old value stays; an empty
    // alternate is a normal choice.
    if (keep_when_blank && value.isEmpty()) return;
    try {
        setter(config_, value);
    } catch (const std::exception &e) {
        log_exception(e, T("保存全局快捷键 {accessor}", {{"accessor", QString::fromUtf8(edit->objectName().toUtf8())}}));
    }
}
}
