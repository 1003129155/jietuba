#include "core/i18n.h"
#include "core/resource_manager.h"
#include "core/ui_scale.h"
#include "settings/tool_settings.h"
#include "ui/fluent_lite/theme.h"
#include "ui/hotkey_edit.h"
#include "ui/welcome/pages.h"
#include <QEasingCurve>
#include <QHBoxLayout>
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

// A pen circle and an arrow drawn over a presentation, with the annotation toolbar below.
class AnnotationDemo : public QWidget {
public:
    static constexpr int DURATION_MS = 6000;
    static constexpr int CIRCLE_START_MS = 500;
    static constexpr int CIRCLE_END_MS = 1900;
    static constexpr int ARROW_START_MS = 2300;
    static constexpr int ARROW_END_MS = 3000;
    static constexpr int FADE_OUT_MS = 5400;
    // The toolbar's default buttons, in order, from the pass-through button to the exit button.
    static constexpr const char *TOOLBAR_ICONS[] = {"svg/穿透.svg", "svg/画笔.svg", "svg/荧光笔.svg", "svg/箭头.svg", "svg/方框.svg", "svg/文字.svg", "svg/白板.svg", "svg/结束截图.svg"};

    explicit AnnotationDemo(QWidget *parent = nullptr) : QWidget(parent) {
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
    void refresh_icons() {
        icons_.clear();
        for (const char *path : TOOLBAR_ICONS) icons_.push_back(to_qicon(ResourceManager::get_resource_path(QString::fromUtf8(path)), this));
        update();
    }
protected:
    // Runs only while the page is shown.
    void showEvent(QShowEvent *event) override {
        QWidget::showEvent(event);
        if (icons_.empty()) refresh_icons();
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
        const double toolbar_h = 30;
        const double screen_w = width() - 8;
        const double screen_h = std::min(screen_w * 0.92, height() - 6.0);
        const QRectF screen((width() - screen_w) / 2, (height() - screen_h) / 2, screen_w, screen_h);

        // The presentation underneath stays put; only the drawings fade between loops.
        p.setPen(QPen(QColor(theme.border), 1));
        p.setBrush(QColor(theme.panel));
        p.drawRoundedRect(screen, 10, 10);
        const QRectF slide = screen.adjusted(16, 16, -16, -(toolbar_h + 22));
        p.setPen(Qt::NoPen);
        p.setBrush(with_alpha(theme.text, 200));
        p.drawRoundedRect(QRectF(slide.x(), slide.y(), slide.width() * 0.5, 8), 4, 4);
        p.setBrush(with_alpha(theme.text_soft, 70));
        for (int i = 0; i < 5; ++i) p.drawRoundedRect(QRectF(slide.x(), slide.y() + slide.height() * 0.3 + i * 13, slide.width() * (0.4 - (i % 3) * 0.06), 6), 3, 3);
        // A small chart on the slide; its tallest bar gets circled.
        const QRectF chart(slide.x() + slide.width() * 0.5, slide.y() + slide.height() * 0.3, slide.width() * 0.42, slide.height() * 0.62);
        const double heights[] = {0.35, 0.55, 0.45, 0.85};
        const double bar_w = chart.width() / 7.4;
        QRectF tallest;
        for (int i = 0; i < 4; ++i) {
            const double h = chart.height() * heights[i];
            const QRectF bar(chart.x() + bar_w * (0.4 + i * 1.8), chart.bottom() - h, bar_w, h);
            p.setBrush(i == 3 ? QColor("#B18468") : with_alpha("#6F8FAB", 170));
            p.drawRoundedRect(bar, 2, 2);
            tallest = bar;
        }

        p.setOpacity(std::max(0.0, 1.0 - phase(now, FADE_OUT_MS, DURATION_MS)));
        // The pen circles the tallest bar, then an arrow points at it from the text.
        const QRectF target = tallest.adjusted(-9, -10, 9, 6);
        const double circle_t = phase(now, CIRCLE_START_MS, CIRCLE_END_MS);
        QPointF pen_tip;
        if (circle_t > 0) {
            QPainterPath circle;
            circle.arcMoveTo(target, 100);
            circle.arcTo(target, 100, -380 * QEasingCurve(QEasingCurve::InOutSine).valueForProgress(circle_t));
            pen_tip = circle.currentPosition();
            QPen pen(QColor("#E5484D"), 2.6);
            pen.setCapStyle(Qt::RoundCap);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawPath(circle);
        }
        const double arrow_t = phase(now, ARROW_START_MS, ARROW_END_MS);
        if (arrow_t > 0) {
            const QPointF from(slide.x() + slide.width() * 0.3, slide.y() + slide.height() * 0.3 + 32);
            const QPointF to(target.left() - 3, target.center().y() + 4);
            const QPointF tip = from + (to - from) * QEasingCurve(QEasingCurve::OutCubic).valueForProgress(arrow_t);
            QPen pen(QColor("#E5484D"), 2.6);
            pen.setCapStyle(Qt::RoundCap);
            p.setPen(pen);
            p.drawLine(from, tip);
            if (arrow_t >= 1.0) {
                const QPointF dir = (to - from) / std::hypot(to.x() - from.x(), to.y() - from.y());
                const QPointF normal(-dir.y(), dir.x());
                QPainterPath head(to);
                head.lineTo(to - dir * 9 + normal * 5);
                head.lineTo(to - dir * 9 - normal * 5);
                head.closeSubpath();
                p.setBrush(QColor("#E5484D"));
                p.setPen(Qt::NoPen);
                p.drawPath(head);
            }
            pen_tip = tip;
        }
        if (now >= CIRCLE_START_MS && now < ARROW_END_MS + 400 && !pen_tip.isNull()) draw_pen_cursor(p, pen_tip);
        p.setOpacity(1.0);

        // The toolbar at the bottom of the screen, with the pen selected.
        const double button = 22, gap = 3;
        const double toolbar_w = TOOLBAR_COUNT * button + (TOOLBAR_COUNT - 1) * gap + 12;
        const QRectF bar(screen.center().x() - toolbar_w / 2, screen.bottom() - toolbar_h - 10, toolbar_w, toolbar_h);
        p.setPen(QPen(QColor(theme.border), 1));
        p.setBrush(QColor(theme.panel_subtle));
        p.drawRoundedRect(bar, 8, 8);
        for (int i = 0; i < TOOLBAR_COUNT; ++i) {
            const QRectF cell(bar.x() + 6 + i * (button + gap), bar.y() + (toolbar_h - button) / 2, button, button);
            if (i == 1) {
                p.setPen(Qt::NoPen);
                p.setBrush(with_alpha(theme.accent, 60));
                p.drawRoundedRect(cell, 5, 5);
            }
            if (i < static_cast<int>(icons_.size()) && !icons_[i].isNull()) icons_[i].paint(&p, cell.adjusted(3, 3, -3, -3).toRect());
        }
    }
private:
    static constexpr int TOOLBAR_COUNT = static_cast<int>(std::size(TOOLBAR_ICONS));

    static void draw_pen_cursor(QPainter &p, const QPointF &tip) {
        p.setPen(QPen(QColor("#FFFFFF"), 1.5));
        p.setBrush(QColor("#E5484D"));
        p.drawEllipse(tip, 3.4, 3.4);
    }

    QTimer timer_;
    int elapsed_ms_ = 0;
    std::vector<QIcon> icons_;
};

class AnnotationIllus : public IllustrationArea {
public:
    using IllustrationArea::IllustrationArea;
    void retranslate() override { caption_->setText(caption_text()); }
    void apply_welcome_theme() override {
        IllustrationArea::apply_welcome_theme();
        if (demo) demo->refresh_icons();
        if (caption_) apply_welcome_label_style(caption_);
    }
    AnnotationDemo *demo = nullptr;
protected:
    void build_content() override {
        layout_->setContentsMargins(dialog_scaled(14), dialog_scaled(16), dialog_scaled(14), dialog_scaled(14));
        demo = new AnnotationDemo(this);
        layout_->addWidget(demo, 1);
        caption_ = new QLabel(caption_text(), this);
        caption_->setAlignment(Qt::AlignCenter);
        caption_->setWordWrap(true);
        set_welcome_label_style(caption_, "muted", 11, 400);
        layout_->addWidget(caption_);
    }
private:
    static QString caption_text() { return welcome_tr("讲解、演示或共享屏幕时，直接在画面上圈重点。"); }
    QLabel *caption_ = nullptr;
};

QString page_subtitle() { return welcome_tr("不用截图，直接在任何窗口上画画、写字，结束后画面恢复原样。"); }
QString hotkey_desc() { return welcome_tr("也可以从托盘菜单打开；再按一次快捷键结束标注。"); }
}

ScreenAnnotationPage::ScreenAnnotationPage(ToolSettingsManager *config, QWidget *parent) : BasePage(settings_text("Screen Annotation"), page_subtitle(), parent), config_(config) {
    init();
}

IllustrationArea *ScreenAnnotationPage::create_illustration() { return new AnnotationIllus(this); }

void ScreenAnnotationPage::build_controls(QVBoxLayout *layout) {
    // Laid out like the hotkey page, whose duplicate check it joins.
    hotkey_lbl_ = new QLabel(settings_text("Global Hotkey"));
    set_welcome_label_style(hotkey_lbl_, "primary", 14, 600);
    layout->addWidget(hotkey_lbl_);
    hotkey_desc_ = new QLabel(hotkey_desc());
    hotkey_desc_->setWordWrap(true);
    set_welcome_label_style(hotkey_desc_, "muted", 12, 400);
    layout->addWidget(hotkey_desc_);
    const QStringList stored = config_->get_global_hotkeys("screen_annotation");
    for (int slot = 0; slot < 2; ++slot) {
        auto *edit = new HotkeyEdit;
        edit->setText(stored.value(slot));
        edit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto *slot_lbl = new QLabel(welcome_tr(slot ? "备用" : "主快捷键"));
        slot_lbl->setFixedWidth(dialog_scaled(62));
        set_welcome_label_style(slot_lbl, "muted", 12, 400);
        auto *row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(dialog_scaled(8));
        row->addWidget(slot_lbl);
        row->addWidget(edit, 1);
        layout->addLayout(row);
        (slot ? hotkey_edit_2_ : hotkey_edit_) = edit;
        (slot ? alt_lbl_ : main_lbl_) = slot_lbl;
    }
}

void ScreenAnnotationPage::retranslate() {
    title_label->setText(settings_text("Screen Annotation"));
    subtitle_label->setText(page_subtitle());
    hotkey_lbl_->setText(settings_text("Global Hotkey"));
    hotkey_desc_->setText(hotkey_desc());
    main_lbl_->setText(welcome_tr("主快捷键"));
    alt_lbl_->setText(welcome_tr("备用"));
    illus_area->retranslate();
}

void ScreenAnnotationPage::save() {
    const QString setters[] = {"screen_annotation_hotkey", "screen_annotation_hotkey_2"};
    const HotkeyEdit *edits[] = {hotkey_edit_, hotkey_edit_2_};
    for (int slot = 0; slot < 2; ++slot) {
        if (!edits[slot]->validation_error().isEmpty()) continue;
        config_->set_app_setting(setters[slot], edits[slot]->text().trimmed());
    }
}

void ScreenAnnotationPage::set_animation_time(int elapsed_ms) {
    if (auto *illus = static_cast<AnnotationIllus *>(illus_area); illus->demo) illus->demo->set_time(elapsed_ms);
}
}
