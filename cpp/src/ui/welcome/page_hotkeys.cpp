#include "core/i18n.h"
#include "core/logger.h"
#include "core/ui_scale.h"
#include "settings/tool_settings.h"
#include "ui/hotkey_edit.h"
#include "core/resource_manager.h"
#include "ui/fluent_lite/icons.h"
#include "ui/fluent_lite/theme.h"
#include "ui/welcome/demo_paint.h"
#include "ui/welcome/pages.h"
#include <QApplication>
#include <QHBoxLayout>
#include <QPainter>
#include <QFontMetricsF>
#include <QPainterPath>
#include <cmath>
#include <functional>

namespace jietuba {
namespace {
using namespace welcome_demo;

// What each hotkey does, on the mouse shortcut page's desktop, as the real windows look at a
// small size; the row beside it names the hotkey. While no slot is being edited the set
// hotkeys take turns; the edited slot repeats alone. Drawn in 100% units under the window scale.
class HotkeyScene : public LoopingDemo {
public:
    // The functions in the hotkey page's order.
    enum Scene { SCREENSHOT, PIN, CLIPBOARD, TRANSLATION, ANNOTATION, SCENE_COUNT };
    static constexpr int SLOT_MS = 2600;
    static constexpr int PLAY_MS = 300;

    explicit HotkeyScene(QWidget *parent = nullptr) : LoopingDemo(SLOT_MS, parent) { setMinimumHeight(dialog_scaled(260)); }
    // The slots' hotkeys in the page's order; the empty ones are skipped.
    void set_hotkeys(const QStringList &hotkeys) {
        shown_.clear();
        for (int i = 0; i < std::min<int>(SCENE_COUNT, static_cast<int>(hotkeys.size())); ++i)
            if (!hotkeys[i].trimmed().isEmpty()) shown_.push_back(i);
        set_duration(SLOT_MS * std::max<int>(1, static_cast<int>(shown_.size())));
        time_changed();
        update();
    }
    // The slot being edited, or -1.
    void set_editing(int index) {
        editing_ = index;
        time_changed();
        update();
    }
    // The entry playing, or -1.
    int current_index() const {
        if (editing_ >= 0) return editing_;
        return shown_.empty() ? -1 : shown_[time() / SLOT_MS % shown_.size()];
    }
    // The entry playing changed.
    std::function<void(int)> on_current;
protected:
    void time_changed() override {
        if (const int now = current_index(); now != reported_) {
            reported_ = now;
            if (on_current) on_current(now);
        }
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const double scale = demo_scale();
        p.scale(scale, scale);
        const double w = width() / scale, h = height() / scale;
        const auto theme = welcome_theme();

        const double screen_w = w - 8;
        const double screen_h = std::min(screen_w * 0.95, h - 8);
        const QRectF screen((w - screen_w) / 2, (h - screen_h) / 2, screen_w, screen_h);
        draw_screen(p, theme, screen);

        const int index = current_index();
        if (index < 0 || index >= SCENE_COUNT) return;
        const int t = time() % SLOT_MS;
        // The function's windows leave before the next one plays.
        p.save();
        p.setOpacity(1.0 - phase(t, SLOT_MS - 350, SLOT_MS - 50));
        QPainterPath clip;
        clip.addRoundedRect(screen, 10, 10);
        p.setClipPath(clip);
        switch (index) {
        case SCREENSHOT: play_screenshot(p, theme, screen, t); break;
        case PIN: play_pin(p, theme, screen, t); break;
        case CLIPBOARD: play_clipboard(p, theme, screen, t); break;
        case TRANSLATION: play_translation(p, theme, screen, t); break;
        case ANNOTATION: play_annotation(p, theme, screen, t); break;
        default: break;
        }
        p.restore();
    }
private:
    static double move(int t, int start, int end) { return ease(QEasingCurve::InOutCubic, phase(t, start, end)); }

    // Ctrl+1: a screenshot of the chart.
    static void play_screenshot(QPainter &p, const WelcomeTheme &theme, const QRectF &screen, int t) { draw_capture(p, theme, screen, t - PLAY_MS); }

    // Ctrl+2: the clipboard's image comes out as a pinned image with the theme-coloured edge.
    static void play_pin(QPainter &p, const WelcomeTheme &theme, const QRectF &screen, int t) {
        const double rise = ease(QEasingCurve::OutBack, phase(t, PLAY_MS, PLAY_MS + 380));
        if (rise <= 0) return;
        const QRectF pin = chart_region(screen).translated(-14 * rise, -16 * rise);
        p.save();
        p.setOpacity(p.opacity() * std::min(1.0, rise));
        draw_shadow(p, theme, pin, 1);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(theme.panel));
        p.drawRect(pin);
        draw_chart(p, theme, pin.adjusted(6, 6, -6, -6));
        p.setPen(QPen(theme_color(), 1.6));
        p.setBrush(Qt::NoBrush);
        p.drawRect(pin);
        p.restore();
    }

    // Ctrl+3: the clipboard window opens.
    static void play_clipboard(QPainter &p, const WelcomeTheme &theme, const QRectF &screen, int t) {
        const double in = ease(QEasingCurve::OutCubic, phase(t, PLAY_MS, PLAY_MS + 300));
        if (in <= 0) return;
        p.save();
        p.setOpacity(p.opacity() * in);
        draw_clipboard_window(p, theme, clipboard_window_rect(screen).translated(0, 8 * (1 - in)));
        p.restore();
    }

    // Ctrl+4: the text lines are picked up and the translation window opens: the two languages
    // with the swap between them, the source and the translation side by side.
    static void play_translation(QPainter &p, const WelcomeTheme &theme, const QRectF &screen, int t) {
        const QRectF window = window_rect(screen);
        const QRectF source(window.x() + window.width() * 0.08 - 3, window.y() + window.height() * 0.2 - 3, window.width() * 0.62 + 6, window.height() * 0.1 + 12);
        const double mark = phase(t, PLAY_MS, PLAY_MS + 150);
        if (mark <= 0) return;
        QColor select = theme_color();
        select.setAlpha(static_cast<int>(70 * mark));
        p.setPen(Qt::NoPen);
        p.setBrush(select);
        p.drawRoundedRect(source, 2, 2);
        const double in = ease(QEasingCurve::OutCubic, phase(t, PLAY_MS + 200, PLAY_MS + 480));
        if (in <= 0) return;
        const double win_w = screen.width() * 0.8, win_h = screen.height() * 0.56;
        const QRectF win(screen.center().x() - win_w / 2, screen.bottom() - win_h - screen.height() * 0.08 + 8 * (1 - in), win_w, win_h);
        p.save();
        p.setOpacity(p.opacity() * in);
        draw_shadow(p, theme, win, 6);
        p.setPen(QPen(QColor(theme.border), 0.8));
        p.setBrush(QColor(theme.panel));
        p.drawRoundedRect(win, 6, 6);
        // Title bar: the translate icon, the pin button and the window buttons.
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#3B82F6"));
        p.drawRoundedRect(QRectF(win.x() + 6, win.y() + 5, 8, 8), 2, 2);
        p.setBrush(with_alpha(theme.text_soft, 110));
        p.drawRoundedRect(QRectF(win.x() + 17, win.y() + 8, 18, 2.5), 1.2, 1.2);
        for (int i = 0; i < 3; ++i) p.drawRoundedRect(QRectF(win.right() - 10 - i * 10, win.y() + 8, 5, 2), 1, 1);
        // The language pills with the swap button.
        const double pill_w = win.width() * 0.24, pills_y = win.y() + 18;
        const QRectF left_pill(win.center().x() - 9 - pill_w, pills_y, pill_w, 11), right_pill(win.center().x() + 9, pills_y, pill_w, 11);
        for (const QRectF &pill : {left_pill, right_pill}) {
            p.setBrush(QColor(theme.panel_subtle));
            p.drawRoundedRect(pill, 5.5, 5.5);
            p.setBrush(with_alpha(theme.text, 110));
            p.drawRoundedRect(QRectF(pill.x() + 6, pill.center().y() - 1.2, 10, 2.4), 1.2, 1.2);
        }
        p.setBrush(with_alpha(theme.accent, theme.is_dark ? 70 : 40));
        p.drawEllipse(QPointF(win.center().x(), pills_y + 5.5), 6, 6);
        demo_icon("swap").paint(&p, QRectF(win.center().x() - 3.5, pills_y + 2, 7, 7).toRect());
        // The source and the translation, which types itself in.
        const double pane_top = pills_y + 17, pane_h = win.bottom() - 20 - pane_top;
        const double pane_w = (win.width() - 18) / 2;
        const QRectF left_pane(win.x() + 6, pane_top, pane_w, pane_h), right_pane(left_pane.right() + 6, pane_top, pane_w, pane_h);
        for (const QRectF &pane : {left_pane, right_pane}) {
            p.setBrush(QColor(theme.panel_subtle));
            p.drawRoundedRect(pane, 4, 4);
        }
        p.setBrush(with_alpha(theme.text, 120));
        p.drawRoundedRect(QRectF(left_pane.x() + 5, left_pane.y() + 6, pane_w * 0.75, 2.6), 1.3, 1.3);
        p.drawRoundedRect(QRectF(left_pane.x() + 5, left_pane.y() + 12, pane_w * 0.5, 2.6), 1.3, 1.3);
        const double typed = phase(t, PLAY_MS + 520, PLAY_MS + 1100);
        p.drawRoundedRect(QRectF(right_pane.x() + 5, right_pane.y() + 6, pane_w * 0.7 * std::min(1.0, typed * 1.6), 2.6), 1.3, 1.3);
        if (typed > 0.6) p.drawRoundedRect(QRectF(right_pane.x() + 5, right_pane.y() + 12, pane_w * 0.4 * (typed - 0.6) / 0.4, 2.6), 1.3, 1.3);
        demo_icon("copy").paint(&p, QRectF(left_pane.x() + 4, left_pane.bottom() - 9, 6, 6).toRect());
        demo_icon("copy").paint(&p, QRectF(right_pane.x() + 4, right_pane.bottom() - 9, 6, 6).toRect());
        // The translate button.
        const QRectF button(win.right() - 34, win.bottom() - 15, 28, 10);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#3B82F6"));
        p.drawRoundedRect(button, 5, 5);
        p.setBrush(QColor(255, 255, 255, 220));
        p.drawRoundedRect(QRectF(button.center().x() - 7, button.center().y() - 1, 14, 2), 1, 1);
        p.restore();
    }

    // Ctrl+5: the annotation toolbar comes up at the bottom of the screen with the pen on, and
    // the pen circles the tallest bar and points at it.
    static void play_annotation(QPainter &p, const WelcomeTheme &theme, const QRectF &screen, int t) {
        const double in = ease(QEasingCurve::OutCubic, phase(t, PLAY_MS, PLAY_MS + 300));
        if (in <= 0) return;
        QRectF bar = toolbar_size(annotation_tools());
        bar.moveTo(screen.center().x() - bar.width() / 2, screen.bottom() - bar.height() - 8 + 8 * (1 - in));
        const QRectF chart_bar_rect = chart_bar(chart_rect(screen), 3);
        const QRectF ring(chart_bar_rect.center().x() - chart_bar_rect.width() * 1.3, chart_bar_rect.top() - 6, chart_bar_rect.width() * 2.6, chart_bar_rect.height() * 0.55);
        const auto ring_point = [&ring](double k) {
            const double angle = (-35 - k * 385) * 3.14159265358979323846 / 180.0;
            return QPointF(ring.center().x() + ring.width() / 2 * std::cos(angle), ring.center().y() + ring.height() / 2 * std::sin(angle));
        };
        QPen pen(QColor("#FF0000"), 2.4);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        const double drawn = ease(QEasingCurve::InOutSine, phase(t, PLAY_MS + 350, PLAY_MS + 1050));
        QPointF tip = ring_point(0);
        if (drawn > 0) {
            QPainterPath path(ring_point(0));
            const int steps = std::max(2, static_cast<int>(64 * drawn));
            for (int i = 1; i <= steps; ++i) path.lineTo(ring_point(drawn * i / steps));
            p.drawPath(path);
            tip = ring_point(drawn);
        }
        const QPointF from(screen.x() + screen.width() * 0.86, screen.y() + screen.height() * 0.24);
        const QPointF to = ring_point(0.06) + QPointF(4, -2);
        const double arrow = ease(QEasingCurve::OutCubic, phase(t, PLAY_MS + 1150, PLAY_MS + 1450));
        if (arrow > 0) {
            const QPointF end = lerp(from, to, arrow);
            p.drawLine(from, end);
            if (arrow >= 1) {
                const QPointF dir = (to - from) / std::hypot(to.x() - from.x(), to.y() - from.y());
                const QPointF side(-dir.y(), dir.x());
                p.drawLine(to, to - dir * 9 + side * 5);
                p.drawLine(to, to - dir * 9 - side * 5);
            }
            tip = end;
        }
        p.save();
        p.setOpacity(p.opacity() * in);
        draw_toolbar(p, theme, bar, annotation_tools(), 0);
        p.restore();
        if (t >= PLAY_MS + 250) draw_cursor(p, t < PLAY_MS + 350 ? lerp(from, ring_point(0), move(t, PLAY_MS + 250, PLAY_MS + 350)) : tip);
    }

    // Indices of the slots with a hotkey, the ones taking turns.
    std::vector<int> shown_;
    int editing_ = -1;
    int reported_ = -2;
};

class HotkeyIllus : public IllustrationArea {
public:
    using IllustrationArea::IllustrationArea;
    void apply_welcome_theme() override {
        IllustrationArea::apply_welcome_theme();
        if (scene) scene->update();
    }
    HotkeyScene *scene = nullptr;
protected:
    void build_content() override {
        layout_->setContentsMargins(dialog_scaled(22), dialog_scaled(22), dialog_scaled(22), dialog_scaled(22));
        scene = new HotkeyScene(this);
        layout_->addWidget(scene, 1);
    }
};

QString settings_text(const char *text) { return i18n_tr(text, "SettingsDialog"); }
QString page_subtitle() { return welcome_tr("截图、钉图、剪贴板、翻译和屏幕标注各设一个快捷键。"); }
QString input_hint() { return welcome_tr("点击输入框后，按下快捷键或鼠标侧键即可录入。备用快捷键可以在设置的「快捷键」页添加。"); }
}

const std::vector<HotkeyPage::Row> &HotkeyPage::rows() {
    // The settings window's order and names; only the screenshot hotkey keeps its value when
    // left blank, since the others are also reached from the tray menu.
    static const std::vector<Row> rows = {
        {"screenshot", "Screenshot", true},
        {"pin_clipboard", "Pin", false},
        {"clipboard", "Clipboard", false},
        {"translation", "Translation", false},
        {"screen_annotation", "Screen Annotation", false},
    };
    return rows;
}

HotkeyPage::HotkeyPage(ToolSettingsManager *config, QWidget *parent) : BasePage(welcome_tr("快捷键"), page_subtitle(), parent), config_(config) {
    init();
}

HotkeyPage::~HotkeyPage() { disconnect(focus_connection_); }

IllustrationArea *HotkeyPage::create_illustration() { return new HotkeyIllus(this); }

void HotkeyPage::build_controls(QVBoxLayout *layout) {
    layout->setSpacing(0);
    for (size_t index = 0; index < rows().size(); ++index) {
        const auto &row = rows()[index];
        auto *edit = new HotkeyEdit;
        edit->setText(config_->get_global_hotkeys(row.function).value(0));
        edit->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        labels_.emplace_back(add_list_row(layout, settings_text(row.title), edit, index == 0), row.title);
        edits_.push_back(edit);
    }
    // The row the stage is playing takes the stage's colour.
    if (auto *scene = static_cast<HotkeyIllus *>(illus_area)->scene)
        scene->on_current = [this](int current) {
            for (size_t i = 0; i < labels_.size(); ++i) set_list_row_current(labels_[i].first, static_cast<int>(i) == current);
        };

    layout->addSpacing(dialog_scaled(16));
    hint_ = new QLabel(input_hint());
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
    sync_key_highlight();

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
    auto *scene = static_cast<HotkeyIllus *>(illus_area)->scene;
    if (!scene) return;
    QStringList hotkeys;
    int editing = -1;
    for (size_t i = 0; i < edits_.size(); ++i) {
        hotkeys << edits_[i]->text();
        if (edits_[i] == active_edit_) editing = static_cast<int>(i);
    }
    scene->set_hotkeys(hotkeys);
    scene->set_editing(editing);
}

bool HotkeyPage::revalidate() { return validate_hotkey_group(edits_); }

void HotkeyPage::retranslate() {
    title_label->setText(welcome_tr("快捷键"));
    subtitle_label->setText(page_subtitle());
    for (const auto &[label, text] : labels_) label->setText(settings_text(text));
    if (hint_) hint_->setText(input_hint());
    sync_key_highlight();
    // The conflict message follows the language too.
    revalidate();
}

void HotkeyPage::save() {
    const bool conflicted = !revalidate();
    for (size_t i = 0; i < rows().size(); ++i) {
        auto *edit = edits_[i];
        if (conflicted && !edit->validation_error().isEmpty()) continue;
        const QString value = edit->text().trimmed();
        if (rows()[i].keep_when_blank && value.isEmpty()) continue;
        QStringList values = config_->get_global_hotkeys(rows()[i].function);
        values[0] = value;
        try {
            config_->set_global_hotkeys(rows()[i].function, values);
        } catch (const std::exception &e) {
            log_exception(e, T("保存全局快捷键 {accessor}", {{"accessor", rows()[i].function}}));
        }
    }
}
}
