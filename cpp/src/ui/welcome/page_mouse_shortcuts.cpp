#include "core/i18n.h"
#include "core/ui_scale.h"
#include "settings/tool_settings.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/settings_ui/mouse_binding_editor.h"
#include "ui/welcome/pages.h"
#include <QEasingCurve>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <algorithm>

namespace jietuba {
namespace {
QString settings_text(const char *text) { return i18n_tr(text, "SettingsDialog"); }

QColor with_alpha(const QString &color, int alpha) {
    QColor result(color);
    result.setAlpha(std::clamp(alpha, 0, 255));
    return result;
}

double phase(int now, int start, int end) { return std::clamp((now - start) / double(end - start), 0.0, 1.0); }
double ease(QEasingCurve::Type curve, double t) { return QEasingCurve(curve).valueForProgress(t); }

// Win + left drag over a window, then the region floats as a pinned image.
class GestureDemo : public QWidget {
public:
    static constexpr int DURATION_MS = 5600;
    static constexpr int KEY_DOWN_MS = 450;
    static constexpr int DRAG_START_MS = 750;
    static constexpr int DRAG_END_MS = 2050;
    static constexpr int PIN_END_MS = 2400;
    static constexpr int FADE_OUT_MS = 5100;

    explicit GestureDemo(QWidget *parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMinimumHeight(dialog_scaled(220));
        timer_.setInterval(30);
        connect(&timer_, &QTimer::timeout, this, [this] { set_time(elapsed_ms_ + timer_.interval()); });
    }
    void set_time(int elapsed_ms) {
        elapsed_ms_ = std::max(0, elapsed_ms) % DURATION_MS;
        update();
    }
protected:
    // Runs only while the page is shown.
    void showEvent(QShowEvent *event) override {
        QWidget::showEvent(event);
        timer_.start();
    }
    void hideEvent(QHideEvent *event) override {
        timer_.stop();
        QWidget::hideEvent(event);
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const auto theme = welcome_theme();
        const int now = elapsed_ms_;
        p.setOpacity(std::max(0.05, std::min(phase(now, 0, 300), 1.0 - phase(now, FADE_OUT_MS, DURATION_MS))));

        // The screen and the keys below it, centred as one block.
        const double keys_h = 34;
        const double screen_w = width() - 8;
        const double screen_h = std::min(screen_w * 0.86, height() - keys_h - 18.0);
        const double top = (height() - screen_h - keys_h - 14) / 2;
        const QRectF screen((width() - screen_w) / 2, top, screen_w, screen_h);
        draw_screen(p, theme, screen);

        // The chart block is what gets selected and pinned.
        const QRectF window = window_rect(screen);
        const QRectF chart(window.x() + window.width() * 0.08, window.y() + window.height() * 0.42, window.width() * 0.56, window.height() * 0.48);
        const QPointF drag_from = chart.topLeft() - QPointF(6, 6);
        const QPointF drag_to = chart.bottomRight() + QPointF(6, 6);
        const double drag_t = ease(QEasingCurve::InOutCubic, phase(now, DRAG_START_MS, DRAG_END_MS));
        const QPointF drag_now = drag_from + (drag_to - drag_from) * drag_t;
        const QRectF selection = QRectF(drag_from, drag_now).normalized();
        const bool dragging = now >= DRAG_START_MS && now < DRAG_END_MS;
        const bool pinned = now >= DRAG_END_MS;

        if (dragging) {
            p.setPen(QPen(QColor(theme.accent), 1.4, Qt::DashLine));
            p.setBrush(with_alpha(theme.accent, 28));
            p.drawRect(selection);
        }
        QPointF cursor = now < DRAG_START_MS ? drag_from : drag_now;
        if (pinned) {
            // The pin rises a little off the screen with a shadow, as pinned windows do.
            const double rise = ease(QEasingCurve::OutBack, phase(now, DRAG_END_MS, PIN_END_MS));
            const QRectF region(drag_from, drag_to);
            const QRectF pin = region.translated(-10 * rise, -12 * rise);
            for (int i = 4; i >= 1; --i) {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(0, 0, 0, theme.is_dark ? 26 : 14));
                p.drawRoundedRect(pin.adjusted(-i, -i + 2, i, i + 2), 4 + i, 4 + i);
            }
            p.setBrush(QColor(theme.panel));
            p.setPen(QPen(with_alpha(theme.accent, 150 + static_cast<int>(70 * (1.0 - rise))), 1.4));
            p.drawRoundedRect(pin, 4, 4);
            draw_chart(p, theme, pin.adjusted(6, 6, -6, -6));
            // The pointer leaves the pin after release.
            cursor = drag_to + QPointF(18, 10) * ease(QEasingCurve::OutCubic, phase(now, PIN_END_MS, PIN_END_MS + 600));
        }
        draw_cursor(p, cursor);

        const bool key_held = now >= KEY_DOWN_MS && now < DRAG_END_MS;
        draw_keys(p, theme, QRectF(screen.x(), screen.bottom() + 14, screen.width(), keys_h), key_held, dragging);
    }
private:
    static QRectF window_rect(const QRectF &screen) { return screen.adjusted(screen.width() * 0.07, screen.height() * 0.1, -screen.width() * 0.07, -screen.height() * 0.08); }

    static void draw_screen(QPainter &p, const WelcomeTheme &theme, const QRectF &screen) {
        p.setPen(QPen(QColor(theme.border), 1));
        p.setBrush(QColor(theme.panel_subtle));
        p.drawRoundedRect(screen, 10, 10);
        // A document window: title bar, two text lines and the chart.
        const QRectF window = window_rect(screen);
        p.setBrush(QColor(theme.panel));
        p.drawRoundedRect(window, 6, 6);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(theme.border));
        for (int i = 0; i < 3; ++i) p.drawEllipse(QPointF(window.x() + 10 + i * 8, window.y() + 9), 2.4, 2.4);
        p.setBrush(with_alpha(theme.text_soft, 70));
        p.drawRoundedRect(QRectF(window.x() + window.width() * 0.08, window.y() + window.height() * 0.2, window.width() * 0.62, 6), 3, 3);
        p.drawRoundedRect(QRectF(window.x() + window.width() * 0.08, window.y() + window.height() * 0.3, window.width() * 0.44, 6), 3, 3);
        const QRectF chart(window.x() + window.width() * 0.08, window.y() + window.height() * 0.42, window.width() * 0.56, window.height() * 0.48);
        draw_chart(p, theme, chart);
        p.setBrush(with_alpha(theme.text_soft, 50));
        for (int i = 0; i < 4; ++i) p.drawRoundedRect(QRectF(chart.right() + 10, chart.y() + 4 + i * 13, window.right() - chart.right() - 20, 6), 3, 3);
    }

    static void draw_chart(QPainter &p, const WelcomeTheme &theme, const QRectF &rect) {
        p.setPen(Qt::NoPen);
        p.setBrush(with_alpha(theme.accent, 22));
        p.drawRoundedRect(rect, 4, 4);
        const char *colors[] = {"#6F8FAB", "#6FA18F", "#8A7FA7", "#B18468", "#6F8FAB"};
        const double heights[] = {0.45, 0.7, 0.55, 0.85, 0.62};
        const double gap = rect.width() * 0.06;
        const double bar_w = (rect.width() - gap * 6) / 5;
        for (int i = 0; i < 5; ++i) {
            const double h = (rect.height() - 10) * heights[i];
            p.setBrush(QColor(colors[i]));
            p.drawRoundedRect(QRectF(rect.x() + gap + i * (bar_w + gap), rect.bottom() - 5 - h, bar_w, h), 2, 2);
        }
    }

    static void draw_cursor(QPainter &p, const QPointF &tip) {
        QPainterPath arrow(tip);
        arrow.lineTo(tip + QPointF(0, 15));
        arrow.lineTo(tip + QPointF(4, 11.5));
        arrow.lineTo(tip + QPointF(7, 17.5));
        arrow.lineTo(tip + QPointF(9.5, 16.3));
        arrow.lineTo(tip + QPointF(6.6, 10.5));
        arrow.lineTo(tip + QPointF(11, 10.5));
        arrow.closeSubpath();
        p.setPen(QPen(QColor("#FFFFFF"), 1.2));
        p.setBrush(QColor("#1B1F24"));
        p.drawPath(arrow);
    }

    // The Win key cap and a mouse with its left button, lit while held.
    static void draw_keys(QPainter &p, const WelcomeTheme &theme, const QRectF &band, bool key_held, bool left_held) {
        const double key_w = 52, mouse_w = 22, plus_w = 22;
        const double x = band.center().x() - (key_w + plus_w + mouse_w) / 2;
        const QRectF key(x, band.y() + 3, key_w, band.height() - 6);
        p.setPen(QPen(key_held ? QColor(theme.accent) : with_alpha(theme.text_soft, 120), 1.2));
        p.setBrush(key_held ? QColor(theme.accent) : QColor(theme.panel));
        p.drawRoundedRect(key, 5, 5);
        QFont font;
        font.setPixelSize(12);
        font.setBold(true);
        p.setFont(font);
        p.setPen(key_held ? QColor("#FFFFFF") : QColor(theme.text));
        p.drawText(key, Qt::AlignCenter, "Win");
        p.setPen(QColor(theme.text_muted));
        p.drawText(QRectF(key.right(), band.y(), plus_w, band.height()), Qt::AlignCenter, "+");
        const QRectF mouse(key.right() + plus_w, band.y() + 1, mouse_w, band.height() - 2);
        p.setPen(QPen(with_alpha(theme.text_soft, 150), 1.2));
        p.setBrush(QColor(theme.panel));
        p.drawRoundedRect(mouse, 10, 10);
        QPainterPath left_button;
        left_button.moveTo(mouse.center().x(), mouse.top() + 1);
        left_button.lineTo(mouse.center().x(), mouse.top() + mouse.height() * 0.42);
        left_button.lineTo(mouse.left() + 1, mouse.top() + mouse.height() * 0.42);
        left_button.arcTo(QRectF(mouse.left() + 1, mouse.top() + 1, mouse.width() - 2, mouse.width() - 2), 180, -90);
        left_button.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(left_held ? QColor(theme.accent) : with_alpha(theme.text_soft, 40));
        p.drawPath(left_button);
    }

    QTimer timer_;
    int elapsed_ms_ = 0;
};

class GestureIllus : public IllustrationArea {
public:
    using IllustrationArea::IllustrationArea;
    void retranslate() override { caption_->setText(caption_text()); }
    void apply_welcome_theme() override {
        IllustrationArea::apply_welcome_theme();
        if (demo) demo->update();
        if (caption_) apply_welcome_label_style(caption_);
    }
    GestureDemo *demo = nullptr;
protected:
    void build_content() override {
        layout_->setContentsMargins(dialog_scaled(14), dialog_scaled(16), dialog_scaled(14), dialog_scaled(14));
        demo = new GestureDemo(this);
        layout_->addWidget(demo, 1);
        caption_ = new QLabel(caption_text(), this);
        caption_->setAlignment(Qt::AlignCenter);
        caption_->setWordWrap(true);
        set_welcome_label_style(caption_, "muted", 11, 400);
        layout_->addWidget(caption_);
    }
private:
    static QString caption_text() { return welcome_tr("按住修饰键拖出一个框，松开即可钉图、复制、识别文字或翻译。"); }
    QLabel *caption_ = nullptr;
};

QString page_subtitle() { return welcome_tr("按住修饰键拖动鼠标框选，松开就直接完成动作，不用先按截图快捷键。"); }
}

MouseShortcutPage::MouseShortcutPage(ToolSettingsManager *config, QWidget *parent) : BasePage(welcome_tr("鼠标快捷键"), page_subtitle(), parent), config_(config) {
    init();
    // 每行有两个下拉框，给快捷键设置多留一些宽度。
    illus_area->setFixedWidth(dialog_scaled(270));
}

IllustrationArea *MouseShortcutPage::create_illustration() { return new GestureIllus(this); }

void MouseShortcutPage::build_controls(QVBoxLayout *layout) {
    auto *grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(dialog_scaled(10));
    grid->setVerticalSpacing(dialog_scaled(8));
    grid->setColumnStretch(1, 1);
    int row = 0;
    for (const auto &action : QUICK_CAPTURE_ACTIONS()) {
        auto *label = new QLabel(settings_text(action.label));
        label->setWordWrap(true);
        label->setFixedWidth(dialog_scaled(104));
        set_welcome_label_style(label, "primary", 13, 600);
        auto *editor = new MouseBindingEditor(action.kind);
        // The combos share the row instead of asking for their widest item; gestures read longer.
        for (QComboBox *combo : {static_cast<QComboBox *>(editor->modifiers), static_cast<QComboBox *>(editor->gesture)}) {
            combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            combo->setMinimumContentsLength(3);
        }
        if (auto *row_layout = qobject_cast<QHBoxLayout *>(editor->layout())) {
            row_layout->setStretchFactor(editor->modifiers, 2);
            row_layout->setStretchFactor(editor->gesture, 3);
        }
        editor->setBinding(get_quick_capture_binding(config_, action.key));
        connect(editor->modifiers, &QComboBox::currentIndexChanged, this, &MouseShortcutPage::refresh_conflict_hint);
        connect(editor->gesture, &QComboBox::currentIndexChanged, this, &MouseShortcutPage::refresh_conflict_hint);
        grid->addWidget(label, row, 0);
        grid->addWidget(editor, row, 1);
        editors_.emplace_back(action.key, editor);
        action_labels_.emplace_back(label, action.label);
        ++row;
    }
    layout->addLayout(grid);

    conflict_lbl_ = new QLabel;
    conflict_lbl_->setWordWrap(true);
    set_welcome_label_style(conflict_lbl_, "primary", 12, 400, "color: #D9534F;");
    layout->addWidget(conflict_lbl_);
    layout->addSpacing(dialog_scaled(4));
    hint_ = new QLabel(welcome_tr("截图、钉图和剪贴板窗口里的鼠标操作，可以在设置的「鼠标快捷键」页调整。"));
    hint_->setWordWrap(true);
    set_welcome_label_style(hint_, "muted", 12, 400);
    layout->addWidget(hint_);
    refresh_conflict_hint();
}

QSet<QString> MouseShortcutPage::conflicted_actions() const {
    QHash<QString, QStringList> by_binding;
    for (const auto &[key, editor] : editors_)
        if (const QString binding = editor->currentData(); !binding.isEmpty()) by_binding[binding].append(key);
    QSet<QString> conflicted;
    for (const auto &keys : std::as_const(by_binding))
        if (keys.size() > 1) conflicted.unite(QSet<QString>(keys.begin(), keys.end()));
    return conflicted;
}

void MouseShortcutPage::refresh_conflict_hint() {
    const auto conflicted = conflicted_actions();
    QStringList names;
    for (size_t i = 0; i < editors_.size(); ++i)
        if (conflicted.contains(editors_[i].first)) names.append(settings_text(action_labels_[i].second));
    conflict_lbl_->setText(names.isEmpty() ? QString() : welcome_tr("%1 用了同一个手势，重复的不会保存。").arg(names.join(" / ")));
    conflict_lbl_->setVisible(!names.isEmpty());
}

void MouseShortcutPage::retranslate() {
    title_label->setText(welcome_tr("鼠标快捷键"));
    subtitle_label->setText(page_subtitle());
    for (const auto &[label, text] : action_labels_) label->setText(settings_text(text));
    hint_->setText(welcome_tr("截图、钉图和剪贴板窗口里的鼠标操作，可以在设置的「鼠标快捷键」页调整。"));
    refresh_conflict_hint();
    illus_area->retranslate();
}

void MouseShortcutPage::save() {
    const auto conflicted = conflicted_actions();
    for (const auto &[key, editor] : editors_)
        if (!conflicted.contains(key)) config_->set_app_setting("quick_capture_" + key, editor->currentData());
}

void MouseShortcutPage::set_animation_time(int elapsed_ms) {
    if (auto *illus = static_cast<GestureIllus *>(illus_area); illus->demo) illus->demo->set_time(elapsed_ms);
}
}
