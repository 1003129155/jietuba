#include "core/i18n.h"
#include "core/ui_scale.h"
#include "settings/tool_settings.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/settings_ui/mouse_binding_editor.h"
#include "ui/welcome/demo_paint.h"
#include "ui/welcome/pages.h"
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>

namespace jietuba {
namespace {
QString settings_text(const char *text) { return i18n_tr(text, "SettingsDialog"); }
QString page_hint() { return welcome_tr("截图、钉图和剪贴板窗口里的鼠标操作，可以在设置的「鼠标快捷键」页调整。"); }

using namespace welcome_demo;

// Win + left drag over a window, then the region floats as a pinned image.
class GestureDemo : public LoopingDemo {
public:
    static constexpr int DURATION_MS = 5600;
    static constexpr int KEY_DOWN_MS = 450;
    static constexpr int DRAG_START_MS = 750;
    static constexpr int DRAG_END_MS = 2050;
    static constexpr int PIN_END_MS = 2400;

    explicit GestureDemo(QWidget *parent = nullptr) : LoopingDemo(DURATION_MS, parent) { setMinimumHeight(dialog_scaled(220)); }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const auto theme = welcome_theme();
        const int now = time();
        p.setOpacity(loop_opacity());

        // The screen and the keys below it, centred as one block.
        const double keys_h = 30;
        const double scale = demo_scale();
        p.scale(scale, scale);
        const double w = width() / scale, h = height() / scale;
        const double screen_w = w - 8;
        const double screen_h = std::min(screen_w * 0.86, h - keys_h - 18.0);
        const double top = (h - screen_h - keys_h - 14) / 2;
        const QRectF screen((w - screen_w) / 2, top, screen_w, screen_h);
        draw_screen(p, theme, screen);

        // The chart block is what gets selected and pinned.
        const QRectF chart = chart_rect(screen);
        const QPointF drag_from = chart.topLeft() - QPointF(6, 6);
        const QPointF drag_to = chart.bottomRight() + QPointF(6, 6);
        const QPointF drag_now = lerp(drag_from, drag_to, ease(QEasingCurve::InOutCubic, phase(now, DRAG_START_MS, DRAG_END_MS)));
        const bool dragging = now >= DRAG_START_MS && now < DRAG_END_MS;
        const bool pinned = now >= DRAG_END_MS;

        if (dragging) {
            p.setPen(QPen(QColor(theme.accent), 1.4, Qt::DashLine));
            p.setBrush(with_alpha(theme.accent, 28));
            p.drawRect(QRectF(drag_from, drag_now).normalized());
        }
        QPointF cursor = now < DRAG_START_MS ? drag_from : drag_now;
        if (pinned) {
            // The pin rises a little off the screen with a shadow, as pinned windows do.
            const double rise = ease(QEasingCurve::OutBack, phase(now, DRAG_END_MS, PIN_END_MS));
            const QRectF pin = QRectF(drag_from, drag_to).translated(-10 * rise, -12 * rise);
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
    // The Win key cap and a mouse with its left button, lit while held.
    static void draw_keys(QPainter &p, const WelcomeTheme &theme, const QRectF &band, bool key_held, bool left_held) {
        const double mouse_w = 20, plus_w = 22;
        const double key_w = keycap_width(p, "Win", band.height()) + 10;
        const double x = band.center().x() - (key_w + plus_w + mouse_w) / 2;
        const QRectF key(x, band.y(), key_w, band.height());
        draw_keycap(p, theme, key, "Win", key_held);
        p.setFont(pixel_font(13));
        p.setPen(QColor(theme.text_soft));
        p.drawText(QRectF(key.right(), band.y(), plus_w, band.height()), Qt::AlignCenter, "+");
        const QRectF mouse(key.right() + plus_w, band.y(), mouse_w, band.height());
        p.setPen(QPen(QColor(theme.border_strong), 1.2));
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
        // The caption right under the demo, the two centred together.
        layout_->addStretch();
        demo = new GestureDemo(this);
        demo->setFixedHeight(dialog_scaled(300));
        layout_->addWidget(demo);
        caption_ = new QLabel(caption_text(), this);
        caption_->setAlignment(Qt::AlignCenter);
        caption_->setWordWrap(true);
        set_welcome_label_style(caption_, "muted", 11, 400);
        layout_->addWidget(caption_);
        layout_->addStretch();
    }
private:
    static QString caption_text() { return welcome_tr("按住修饰键拖出一个框，松开即可钉图、复制、识别文字或翻译。"); }
    QLabel *caption_ = nullptr;
};

QString page_subtitle() { return welcome_tr("按住修饰键拖动鼠标框选，松开就直接完成动作，不用先按截图快捷键。"); }
}

MouseShortcutPage::MouseShortcutPage(ToolSettingsManager *config, QWidget *parent) : BasePage(welcome_tr("鼠标快捷键"), page_subtitle(), parent), config_(config) {
    init();
}

IllustrationArea *MouseShortcutPage::create_illustration() { return new GestureIllus(this); }

void MouseShortcutPage::build_controls(QVBoxLayout *layout) {
    auto *grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(dialog_scaled(20));
    grid->setVerticalSpacing(dialog_scaled(4));
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    int index = 0;
    for (const auto &action : QUICK_CAPTURE_ACTIONS()) {
        auto *label = new QLabel(settings_text(action.label));
        label->setWordWrap(true);
        set_welcome_label_style(label, "primary", 13, 600);
        auto *editor = new MouseBindingEditor(action.kind);
        editor->setObjectName(QString("WizardMouse_%1").arg(action.key));
        editor->setMinimumHeight(dialog_scaled(36));
        // The combos share the row instead of asking for their widest item; gestures read longer.
        for (QComboBox *combo : {static_cast<QComboBox *>(editor->modifiers), static_cast<QComboBox *>(editor->gesture)}) {
            combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            combo->setMinimumContentsLength(3);
        }
        editor->modifiers->setFixedWidth(dialog_scaled(88));
        if (auto *row_layout = qobject_cast<QHBoxLayout *>(editor->layout())) {
            row_layout->setSpacing(dialog_scaled(6));
            row_layout->setStretchFactor(editor->modifiers, 0);
            row_layout->setStretchFactor(editor->gesture, 1);
        }
        editor->setBinding(get_quick_capture_binding(config_, action.key));
        connect(editor->modifiers, &QComboBox::currentIndexChanged, this, &MouseShortcutPage::refresh_conflict_hint);
        connect(editor->gesture, &QComboBox::currentIndexChanged, this, &MouseShortcutPage::refresh_conflict_hint);
        const int row = index / 2 * 3, column = index % 2;
        grid->addWidget(label, row, column);
        grid->addWidget(editor, row + 1, column);
        grid->setRowMinimumHeight(row + 2, dialog_scaled(4));
        ++index;
        editors_.emplace_back(action.key, editor);
        action_labels_.emplace_back(label, action.label);
    }
    layout->addLayout(grid);
    layout->addSpacing(dialog_scaled(4));

    conflict_lbl_ = new QLabel;
    conflict_lbl_->setWordWrap(true);
    set_welcome_label_style(conflict_lbl_, "primary", 12, 400, "color: #D9534F;");
    layout->addWidget(conflict_lbl_);
    layout->addSpacing(dialog_scaled(4));
    hint_ = new QLabel(page_hint());
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
    for (const auto &action : QUICK_CAPTURE_ACTIONS())
        if (conflicted.contains(QString::fromLatin1(action.key))) names.append(settings_text(action.label));
    conflict_lbl_->setText(names.isEmpty() ? QString() : welcome_tr("%1 用了同一个手势，重复的不会保存。").arg(names.join(" / ")));
    conflict_lbl_->setVisible(!names.isEmpty());
}

void MouseShortcutPage::retranslate() {
    title_label->setText(welcome_tr("鼠标快捷键"));
    subtitle_label->setText(page_subtitle());
    for (const auto &[label, text] : action_labels_) label->setText(settings_text(text));
    hint_->setText(page_hint());
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
