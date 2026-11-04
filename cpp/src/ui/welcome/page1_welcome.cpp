#include "core/i18n.h"
#include "core/logger.h"
#include "core/resource_manager.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "settings/tool_settings.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/cards.h"
#include "ui/welcome/demo_paint.h"
#include "ui/welcome/pages.h"
#include "ui/welcome/wizard.h"
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <cmath>

namespace jietuba {
namespace {
QString appearance_tr(const char *text) { return i18n_tr(text, "SettingsDialog"); }
QString theme_label_tr(const char *text) { return i18n_tr(text, "ClipboardWindow"); }

QString welcome_title() { return brand_text(welcome_tr("欢迎使用截图吧 👋")).remove(QString::fromUtf8("👋")).trimmed(); }
QString language_title() { return welcome_tr("🌐 界面语言").remove(QString::fromUtf8("🌐")).trimmed(); }

using namespace welcome_demo;

// Cartoon fireworks behind the icon and name, flat like the camera: a spark rises with two
// dots behind it, bursts with a white star into round-capped rays and an inner ring of dots in
// the program's feature colours, and a few small stars twinkle as it fades. Positions come from
// fixed seeds, so every loop is the same. Drawn in 100% units under the window scale.
class Fireworks : public LoopingDemo {
public:
    static constexpr int LOOP_MS = 6400;
    static constexpr int RISE_MS = 520, BURST_MS = 1700;
    static constexpr int RAYS = 10, SPARKLES = 4;
    // (start, x and peak y as fractions of the stage, colour index).
    struct Burst {
        int start;
        double x, y;
        int color;
    };
    static constexpr Burst BURSTS[] = {
        {0, 0.28, 0.24, 0}, {700, 0.74, 0.18, 1}, {1500, 0.52, 0.12, 2}, {2400, 0.20, 0.48, 3},
        {3200, 0.78, 0.74, 4}, {4100, 0.36, 0.14, 5}, {4900, 0.70, 0.34, 0}, {5700, 0.28, 0.80, 2},
    };

    explicit Fireworks(QWidget *parent = nullptr) : LoopingDemo(LOOP_MS, parent) {}
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const double scale = demo_scale();
        p.scale(scale, scale);
        const double w = width() / scale, h = height() / scale;
        const auto palette = colors();
        for (int b = 0; b < static_cast<int>(std::size(BURSTS)); ++b) {
            const auto &burst = BURSTS[b];
            // Each burst wraps round the loop, so the last ones carry on into the next.
            const int local = (time() - burst.start + LOOP_MS) % LOOP_MS;
            if (local >= RISE_MS + BURST_MS) continue;
            const QPointF peak(w * burst.x, h * burst.y);
            const QColor color = palette[burst.color % palette.size()];
            if (local < RISE_MS) draw_rise(p, color, QPointF(peak.x(), h), peak, local / double(RISE_MS));
            else draw_burst(p, color, peak, b, (local - RISE_MS) / double(BURST_MS));
        }
    }
private:
    static constexpr double PI = 3.14159265358979323846;

    // A fixed pseudo-random number in [0, 1) for a burst and an index.
    static double random(int burst, int index) {
        quint32 v = static_cast<quint32>(burst * 7919 + index * 104729 + 12345);
        v ^= v << 13;
        v ^= v >> 17;
        v ^= v << 5;
        return (v % 10000) / 10000.0;
    }

    // The theme colour and the feature colours, a little richer than on the cards: deep on a
    // light stage, bright on a dark one.
    static QList<QColor> colors() {
        const bool dark = welcome_theme().is_dark;
        QList<QColor> list{theme_color()};
        for (const char *tone : {"capture", "pin", "clipboard", "translate"}) {
            const auto &colors = feature_tone(tone);
            list << QColor(dark ? colors.dark_fg : colors.light_fg);
        }
        list << QColor(welcome_theme().accent);
        for (auto &color : list) {
            float h, s, v, a;
            color.getHsvF(&h, &s, &v, &a);
            color = QColor::fromHsvF(h, std::min(1.0f, s * (dark ? 1.15f : 1.45f)), dark ? std::min(1.0f, v * 1.05f) : v * 0.92f, a);
        }
        return list;
    }

    // A four-pointed star, the cartoon sparkle.
    static void draw_star(QPainter &p, const QColor &color, const QPointF &centre, double radius) {
        if (radius <= 0.2) return;
        const double waist = radius * 0.28;
        QPainterPath star(centre + QPointF(0, -radius));
        star.quadTo(centre + QPointF(waist, -waist), centre + QPointF(radius, 0));
        star.quadTo(centre + QPointF(waist, waist), centre + QPointF(0, radius));
        star.quadTo(centre + QPointF(-waist, waist), centre + QPointF(-radius, 0));
        star.quadTo(centre + QPointF(-waist, -waist), centre + QPointF(0, -radius));
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawPath(star);
    }

    // The spark climbing to its burst, two dots behind it.
    static void draw_rise(QPainter &p, const QColor &color, const QPointF &from, const QPointF &to, double k) {
        const auto at = [&](double t) { return lerp(from, to, ease(QEasingCurve::OutCubic, std::max(0.0, t))); };
        p.setPen(Qt::NoPen);
        p.setBrush(with_alpha(color, 110));
        p.drawEllipse(at(k - 0.16), 1.6, 1.6);
        p.setBrush(with_alpha(color, 180));
        p.drawEllipse(at(k - 0.08), 2.2, 2.2);
        p.setBrush(color);
        p.drawEllipse(at(k), 3, 3);
    }

    // The burst: a white star, then round-capped rays flying out and shortening, an inner ring
    // of dots, and small stars twinkling as it fades.
    static void draw_burst(QPainter &p, const QColor &color, const QPointF &centre, int burst, double k) {
        if (k < 0.22) draw_star(p, QColor("#FFFFFF"), centre, 18 * std::sin(k / 0.22 * PI));
        const double reach = 1.0 - std::pow(1.0 - k, 2.5);
        const double radius = 62 + 22 * random(burst, 77);
        const double drop = 20 * k * k;
        const double fade = 1.0 - ease(QEasingCurve::InQuad, std::max(0.0, (k - 0.55) / 0.45));
        // A ray's length: long as it leaves, a dot by the end.
        const double length = std::max(0.0, 16 * (1.0 - k * 1.2));
        const double turn = random(burst, 31) * PI / RAYS;
        QPen pen(with_alpha(color, static_cast<int>(255 * fade)), 4);
        pen.setCapStyle(Qt::RoundCap);
        for (int ray = 0; ray < RAYS; ++ray) {
            const double angle = turn + ray * 2 * PI / RAYS;
            const QPointF dir(std::cos(angle), std::sin(angle));
            const QPointF head = centre + dir * radius * reach + QPointF(0, drop);
            p.setPen(pen);
            p.drawLine(head - dir * length, head);
        }
        // The inner ring: lighter dots half as far, turned between the rays.
        p.setPen(Qt::NoPen);
        p.setBrush(with_alpha(color.lighter(130), static_cast<int>(255 * fade)));
        for (int ray = 0; ray < RAYS; ++ray) {
            const double angle = turn + (ray + 0.5) * 2 * PI / RAYS;
            p.drawEllipse(centre + QPointF(std::cos(angle), std::sin(angle)) * radius * 0.5 * reach + QPointF(0, drop * 0.8), 2.8 * fade + 0.4, 2.8 * fade + 0.4);
        }
        // Sparkles round the edge, each twinkling once.
        for (int i = 0; i < SPARKLES; ++i) {
            const double at = 0.4 + 0.12 * i;
            const double twinkle = std::sin(std::clamp((k - at) / 0.25, 0.0, 1.0) * PI);
            const double angle = random(burst, 200 + i) * 2 * PI;
            const double distance = radius * (0.75 + 0.3 * random(burst, 300 + i));
            draw_star(p, i % 2 ? QColor("#FFFFFF") : color, centre + QPointF(std::cos(angle), std::sin(angle)) * distance + QPointF(0, drop), 7 * twinkle);
        }
    }
};

// The stage holds the program's icon and name over the fireworks.
class WelcomeIllus : public IllustrationArea {
public:
    using IllustrationArea::IllustrationArea;
    void apply_welcome_theme() override {
        IllustrationArea::apply_welcome_theme();
        if (fireworks) fireworks->update();
    }
    Fireworks *fireworks = nullptr;
protected:
    void build_content() override {
        // Under the icon and name, filling the stage rather than taking a place in the layout.
        fireworks = new Fireworks(this);
        fireworks->lower();
        auto *icon = new QLabel(this);
        icon->setAlignment(Qt::AlignCenter);
        const QIcon logo(ResourceManager::get_resource_path(QString::fromUtf8("svg/托盘.svg")));
        if (!logo.isNull()) icon->setPixmap(logo.pixmap(dialog_scaled(96), dialog_scaled(96)));
        auto *name = new QLabel(PRODUCT_NAME, this);
        name->setAlignment(Qt::AlignCenter);
        set_welcome_label_style(name, "primary", 30, 700);
        layout_->addStretch();
        layout_->addWidget(icon);
        layout_->addSpacing(dialog_scaled(14));
        layout_->addWidget(name);
        layout_->addStretch();
    }
    void resizeEvent(QResizeEvent *event) override {
        IllustrationArea::resizeEvent(event);
        if (fireworks) fireworks->setGeometry(rect());
    }
};
}

WelcomePage::WelcomePage(ToolSettingsManager *config, WelcomeWizard *wizard, QWidget *parent)
    : BasePage(welcome_title(), welcome_tr("截图、文字识别、钉图、录屏、屏幕标注、剪贴板和翻译，一站搞定"), parent), config_(config), wizard_(wizard) {
    // The wizard loaded the language before building any page.
    init_lang_ = I18nManager::get_current_language();
    init();
}

IllustrationArea *WelcomePage::create_illustration() { return new WelcomeIllus(this); }

void WelcomePage::build_controls(QVBoxLayout *layout) {
    layout->setSpacing(0);
    const auto combo = [] {
        auto *box = new ComboBox;
        box->setFixedSize(dialog_scaled(168), dialog_scaled(34));
        box->setCursor(Qt::PointingHandCursor);
        return box;
    };
    lang_combo_ = combo();
    for (const auto &[code, name] : I18nManager::language_items()) lang_combo_->addItem(name, code);
    if (const int idx = lang_combo_->findData(init_lang_); idx >= 0) lang_combo_->setCurrentIndex(idx);
    connect(lang_combo_, &QComboBox::currentIndexChanged, this, &WelcomePage::on_lang_changed);
    row_lang_lbl_ = add_list_row(layout, language_title(), lang_combo_, true);

    theme_combo_ = combo();
    populate_theme_combo();
    connect(theme_combo_, &QComboBox::currentIndexChanged, this, &WelcomePage::on_theme_changed);
    theme_lbl_ = add_list_row(layout, theme_label_tr("Theme"), theme_combo_, false);
}

void WelcomePage::retranslate() {
    title_label->setText(welcome_title());
    subtitle_label->setText(welcome_tr("截图、文字识别、钉图、录屏、屏幕标注、剪贴板和翻译，一站搞定"));
    if (row_lang_lbl_) row_lang_lbl_->setText(language_title());
    if (theme_lbl_) theme_lbl_->setText(theme_label_tr("Theme"));
    if (theme_combo_) populate_theme_combo();
}

void WelcomePage::on_lang_changed() {
    const QString code = lang_combo_->currentData().toString();
    if (code.isEmpty()) return;
    I18nManager::load_language(code);
    config_->set_language(code);
    if (wizard_) wizard_->retranslate_ui();
}

void WelcomePage::populate_theme_combo() {
    QString current = theme_combo_->count() ? theme_combo_->currentData().toString() : QString();
    if (current.isEmpty()) current = ui_theme_mode_value(get_ui_theme()->mode());
    theme_combo_->blockSignals(true);
    theme_combo_->clear();
    theme_combo_->addItem(appearance_tr("System"), "system");
    theme_combo_->addItem(appearance_tr("Light"), "light");
    theme_combo_->addItem(appearance_tr("Dark"), "dark");
    theme_combo_->setCurrentIndex(std::max(0, theme_combo_->findData(current)));
    theme_combo_->blockSignals(false);
}

void WelcomePage::on_theme_changed() {
    const QString mode = theme_combo_->currentData().toString();
    if (mode.isEmpty()) return;
    try {
        // This page holds the real configuration; the theme manager only applies and signals.
        config_->set_ui_theme_mode(mode);
        get_ui_theme()->set_mode(mode, false);
    } catch (const std::exception &e) {
        log_exception(e, T("切换界面主题"));
    }
}
}

