#include "core/i18n.h"
#include "core/logger.h"
#include "core/resource_manager.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "settings/tool_settings.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/theme.h"
#include "ui/welcome/pages.h"
#include "ui/welcome/wizard.h"
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QPainter>

namespace jietuba {
namespace {
QString appearance_tr(const char *text) { return i18n_tr(text, "SettingsDialog"); }
QString theme_label_tr(const char *text) { return i18n_tr(text, "ClipboardWindow"); }

QString welcome_title() { return brand_text(welcome_tr("欢迎使用截图吧 👋")).remove(QString::fromUtf8("👋")).trimmed(); }

// The first screen's product preview: the main capabilities rather than one capture editor.
class ProductCanvas : public QWidget {
    Q_OBJECT
public:
    // (title, meta, accent colour, icon); the titles are decorative and stay in English.
    struct Module {
        const char *title, *meta, *color, *icon;
    };
    static constexpr Module MODULES[] = {
        {"Capture", "SCREEN", "#6F8FAB", "svg/选择.svg"},       {"Recognize", "OCR", "#6FA18F", "svg/文字识别.svg"},
        {"Pin", "ON TOP", "#8A7FA7", "svg/钉图.svg"},            {"Record", "GIF · MP4", "#B18468", "svg/gif.svg"},
        {"Long Shot", "SCROLL", "#6F8FAB", "svg/长截图.svg"},    {"Draw", "ON SCREEN", "#6FA18F", "svg/画笔.svg"},
        {"Clipboard", "HISTORY", "#8A7FA7", "svg/copy.svg"},     {"Translate", "LANGUAGE", "#B18468", "svg/翻译.svg"},
    };
    explicit ProductCanvas(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumHeight(dialog_scaled(220));
        set_own_style(this, "background: transparent;");
        load_icons();
    }
    Q_INVOKABLE void apply_welcome_child_theme() {
        load_icons();
        update();
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const int w = width(), h = height();
        const auto theme = welcome_theme();

        // Restrained background shapes give the overview some depth.
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(111, 143, 171, theme.is_dark ? 34 : 24));
        p.drawEllipse(static_cast<int>(w * 0.60), 2, static_cast<int>(w * 0.40), static_cast<int>(w * 0.40));
        p.setBrush(QColor(96, 168, 151, theme.is_dark ? 28 : 18));
        p.drawEllipse(-18, static_cast<int>(h * 0.62), static_cast<int>(w * 0.42), static_cast<int>(w * 0.42));

        const QRectF card(12, 14, std::max(80, w - 24), std::max(120, h - 28));
        p.setPen(QPen(QColor(theme.border), 1));
        p.setBrush(QColor(theme.panel));
        p.drawRoundedRect(card, 12, 12);

        // The workspace title bar.
        const int top_h = 30;
        p.setBrush(QColor(theme.panel_subtle));
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(QRectF(card.x() + 1, card.y() + 1, card.width() - 2, top_h), 11, 11);
        p.fillRect(QRectF(card.x() + 1, card.y() + top_h - 10, card.width() - 2, 10), QColor(theme.panel_subtle));
        const QString dots[] = {theme.text_soft, theme.border_strong, theme.border};
        for (int i = 0; i < 3; ++i) {
            p.setBrush(QColor(dots[i]));
            p.drawEllipse(static_cast<int>(card.x() + 13 + i * 12), static_cast<int>(card.y() + 12), 5, 5);
        }

        const QRectF content = card.adjusted(10, top_h + 9, -10, -10);

        // The value summary on top.
        const QRectF summary(content.x(), content.y(), content.width(), 46);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(theme.accent_soft));
        p.drawRoundedRect(summary, 9, 9);
        p.setPen(QColor(theme.accent));
        p.setFont(QFont("Segoe UI", 7, QFont::DemiBold));
        p.drawText(summary.adjusted(12, 6, -10, -26), Qt::AlignLeft | Qt::AlignVCenter, "ONE WORKSPACE");
        p.setPen(QColor(theme.text));
        p.setFont(QFont("Segoe UI", 9, QFont::DemiBold));
        p.drawText(summary.adjusted(12, 19, -8, -5), Qt::AlignLeft | Qt::AlignVCenter, "Capture. Recognize. Organize.");

        // Two columns of capability tiles: icon on the left, name and meta beside it.
        constexpr int count = static_cast<int>(std::size(MODULES));
        constexpr int rows = (count + 1) / 2;
        const double grid_y = summary.bottom() + 8;
        const double gap = 7;
        const double tile_w = (content.width() - gap) / 2;
        const double tile_h = std::max(30.0, (content.bottom() - grid_y - gap * (rows - 1)) / rows);
        for (int idx = 0; idx < count; ++idx) {
            const auto &module = MODULES[idx];
            const int col = idx % 2, row = idx / 2;
            const QRectF tile(content.x() + col * (tile_w + gap), grid_y + row * (tile_h + gap), tile_w, tile_h);
            p.setPen(QPen(QColor(theme.border), 1));
            p.setBrush(QColor(theme.panel));
            p.drawRoundedRect(tile, 8, 8);
            const double icon_size = std::min(26.0, tile_h - 12);
            const QRectF icon(tile.x() + 8, tile.center().y() - icon_size / 2, icon_size, icon_size);
            p.setPen(Qt::NoPen);
            QColor icon_color(module.color);
            icon_color.setAlpha(theme.is_dark ? 52 : 36);
            p.setBrush(icon_color);
            p.drawRoundedRect(icon, 7, 7);
            if (!icons_[idx].isNull()) icons_[idx].paint(&p, icon.adjusted(5, 5, -5, -5).toRect());
            const QRectF text_box(icon.right() + 8, tile.y(), tile.right() - icon.right() - 12, tile.height());
            p.setPen(QColor(theme.text));
            p.setFont(QFont("Segoe UI", 8, QFont::DemiBold));
            p.drawText(QRectF(text_box.x(), text_box.y(), text_box.width(), text_box.height() / 2 + 2), Qt::AlignLeft | Qt::AlignBottom, module.title);
            p.setPen(QColor(theme.text_soft));
            p.setFont(QFont("Segoe UI", 6, QFont::Medium));
            p.drawText(QRectF(text_box.x(), text_box.center().y() + 3, text_box.width(), text_box.height() / 2 - 3), Qt::AlignLeft | Qt::AlignTop, module.meta);
        }
    }
private:
    void load_icons() {
        for (size_t i = 0; i < std::size(MODULES); ++i) icons_[i] = to_qicon(ResourceManager::get_resource_path(QString::fromUtf8(MODULES[i].icon)), this);
    }
    QIcon icons_[std::size(MODULES)];
};

class WelcomeIllus : public IllustrationArea {
    Q_OBJECT
public:
    using IllustrationArea::IllustrationArea;
    void retranslate() override { update(); }
    Q_INVOKABLE void apply_welcome_child_theme() {
        const auto theme = welcome_theme();
        const QString fallback_style = icon_lbl_->property("welcomeFallbackIcon").toBool()
                                           ? QString(" font-size: %1px; font-weight: 700; color: %2;").arg(dialog_scaled(16)).arg(theme.accent)
                                           : QString();
        set_own_style(icon_lbl_, QString("background: %1; border: 1px solid %2; border-radius: %3px;%4").arg(theme.panel, theme.border).arg(dialog_scaled(10)).arg(fallback_style));
        apply_welcome_label_style(name_lbl_);
        apply_welcome_label_style(meta_lbl_);
        preview_->update();
    }
protected:
    void build_content() override {
        // The brand lock-up.
        auto *brand = new QHBoxLayout;
        brand->setSpacing(dialog_scaled(10));
        icon_lbl_ = new QLabel(this);
        icon_lbl_->setFixedSize(dialog_scaled(42), dialog_scaled(42));
        icon_lbl_->setAlignment(Qt::AlignCenter);
        try_load_icon();
        auto *text_box = new QVBoxLayout;
        text_box->setSpacing(0);
        name_lbl_ = new QLabel(PRODUCT_NAME, this);
        set_welcome_label_style(name_lbl_, "primary", 18, 700);
        meta_lbl_ = new QLabel("PRODUCTIVITY SUITE", this);
        set_welcome_label_style(meta_lbl_, "accent", 9, 600, "letter-spacing: 1px;");
        text_box->addWidget(name_lbl_);
        text_box->addWidget(meta_lbl_);
        brand->addWidget(icon_lbl_);
        brand->addLayout(text_box, 1);
        layout_->addLayout(brand);
        preview_ = new ProductCanvas(this);
        layout_->addWidget(preview_, 1);
    }
private:
    void try_load_icon() {
        const QString path = ResourceManager::get_resource_path(QString::fromUtf8("svg/托盘.svg"));
        if (QFileInfo::exists(path)) {
            icon_lbl_->setPixmap(QIcon(path).pixmap(dialog_scaled(25), dialog_scaled(25)));
            return;
        }
        icon_lbl_->setText("J");
        icon_lbl_->setProperty("welcomeFallbackIcon", true);
    }
    QLabel *icon_lbl_ = nullptr, *name_lbl_ = nullptr, *meta_lbl_ = nullptr;
    ProductCanvas *preview_ = nullptr;
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
    // Language.
    row_lang_lbl_ = new QLabel(welcome_tr("🌐 界面语言").remove(QString::fromUtf8("🌐")).trimmed());
    set_welcome_label_style(row_lang_lbl_, "primary", 14, 600);
    layout->addWidget(row_lang_lbl_);
    lang_combo_ = new ComboBox;
    lang_combo_->setFixedWidth(dialog_scaled(200));
    lang_combo_->setFixedHeight(dialog_scaled(36));
    lang_combo_->setCursor(Qt::PointingHandCursor);
    for (const auto &[code, name] : I18nManager::language_items()) lang_combo_->addItem(name, code);
    if (const int idx = lang_combo_->findData(init_lang_); idx >= 0) lang_combo_->setCurrentIndex(idx);
    connect(lang_combo_, &QComboBox::currentIndexChanged, this, &WelcomePage::on_lang_changed);
    // The combo stays left; the stretch takes the rest.
    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(lang_combo_);
    row->addStretch();
    layout->addLayout(row);

    // Global theme.
    layout->addSpacing(dialog_scaled(20));
    theme_lbl_ = new QLabel(theme_label_tr("Theme"));
    set_welcome_label_style(theme_lbl_, "primary", 14, 600);
    layout->addWidget(theme_lbl_);
    theme_combo_ = new ComboBox;
    theme_combo_->setFixedWidth(dialog_scaled(200));
    theme_combo_->setFixedHeight(dialog_scaled(36));
    theme_combo_->setCursor(Qt::PointingHandCursor);
    populate_theme_combo();
    connect(theme_combo_, &QComboBox::currentIndexChanged, this, &WelcomePage::on_theme_changed);
    auto *theme_row = new QHBoxLayout;
    theme_row->setContentsMargins(0, 0, 0, 0);
    theme_row->addWidget(theme_combo_);
    theme_row->addStretch();
    layout->addLayout(theme_row);
}

void WelcomePage::retranslate() {
    title_label->setText(welcome_title());
    subtitle_label->setText(welcome_tr("截图、文字识别、钉图、录屏、屏幕标注、剪贴板和翻译，一站搞定"));
    if (row_lang_lbl_) row_lang_lbl_->setText(welcome_tr("🌐 界面语言").remove(QString::fromUtf8("🌐")).trimmed());
    if (theme_lbl_) theme_lbl_->setText(theme_label_tr("Theme"));
    if (theme_combo_) populate_theme_combo();
    illus_area->retranslate();
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

#include "page1_welcome.moc"
