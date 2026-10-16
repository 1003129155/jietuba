#include "core/resource_manager.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "settings/tool_settings.h"
#include "ui/dialogs.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/icons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/theme.h"
#include "ui/welcome/pages.h"
#include <QDir>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QPainter>
#include <QPropertyAnimation>
#include <QTimer>

namespace jietuba {
namespace {
// (svg path, name key, description key).
struct ToolEntry {
    const char *svg, *name_key, *desc_key;
};
const ToolEntry TOOLS[] = {
    {"svg/画笔.svg", "工具_画笔", "工具描述_画笔"},
    {"svg/荧光笔.svg", "工具_荧光笔", "工具描述_荧光笔"},
    {"svg/箭头.svg", "工具_箭头", "工具描述_箭头"},
    {"svg/序号.svg", "工具_序号", "工具描述_序号"},
    {"svg/方框.svg", "工具_矩形框", "工具描述_矩形框"},
    {"svg/圆框.svg", "工具_椭圆框", "工具描述_椭圆框"},
    {"svg/文字.svg", "工具_文字", "工具描述_文字"},
    {"svg/橡皮.svg", "工具_橡皮擦", "工具描述_橡皮擦"},
    {"svg/长截图.svg", "工具_长截图", "工具描述_长截图"},
    {"svg/gif.svg", "工具_录屏", "工具描述_录屏"},
    {"svg/翻译.svg", "工具_截图翻译", "工具描述_截图翻译"},
    {"svg/钉图.svg", "工具_钉图", "工具描述_钉图"},
    {"svg/下载.svg", "工具_保存", "工具描述_保存"},
    {"svg/确定.svg", "工具_完成截图", "工具描述_完成截图"},
};
constexpr int TOOL_COUNT = static_cast<int>(std::size(TOOLS));

// Chinese fallbacks for keys without a translation.
QString tool_text(const char *key) {
    static const QHash<QString, QString> zh{
        {"工具_画笔", "画笔"},
        {"工具_荧光笔", "荧光笔"},
        {"工具_箭头", "箭头"},
        {"工具_序号", "序号"},
        {"工具_矩形框", "矩形框"},
        {"工具_椭圆框", "椭圆框"},
        {"工具_文字", "文字"},
        {"工具_橡皮擦", "橡皮擦"},
        {"工具_长截图", "长截图"},
        {"工具_录屏", "录屏"},
        {"工具_截图翻译", "截图翻译"},
        {"工具_钉图", "钉图"},
        {"工具_保存", "保存"},
        {"工具_完成截图", "完成截图"},
        {"工具描述_画笔", "自由绘制线条，Shift 画直线"},
        {"工具描述_荧光笔", "半透明高亮，标记重点区域"},
        {"工具描述_箭头", "绘制箭头，多种样式可选"},
        {"工具描述_序号", "标注①②③…有序说明步骤"},
        {"工具描述_矩形框", "圈出矩形区域，支持填充"},
        {"工具描述_椭圆框", "圈出椭圆区域，支持填充"},
        {"工具描述_文字", "在截图上添加文字说明"},
        {"工具描述_橡皮擦", "擦除已绘制的标注内容"},
        {"工具描述_长截图", "自动滚动拼接超长页面"},
        {"工具描述_录屏", "录制屏幕区域为 GIF 动图或 MP4 视频"},
        {"工具描述_截图翻译", "OCR 识别文字并即时翻译"},
        {"工具描述_钉图", "将截图悬浮置顶在屏幕上"},
        {"工具描述_保存", "保存截图到本地文件"},
        {"工具描述_完成截图", "复制到剪贴板或保存文件"},
    };
    const QString source = QString::fromUtf8(key);
    const QString translated = welcome_tr(key);
    if (!translated.isEmpty() && translated != source) return translated;
    return zh.value(source, source);
}

// A compact tool matrix for the two-column wizard.
constexpr int ICON_NORMAL = 23, ICON_ACTIVE = 29, BTN_NORMAL = 36, BTN_ACTIVE = 44, ANIM_MS = 2000;

class IconCell : public QWidget {
public:
    IconCell(const QString &svg_rel, QWidget *parent = nullptr) : QWidget(parent), svg_rel_(svg_rel) {
        setFixedSize(dialog_scaled(BTN_ACTIVE), dialog_scaled(BTN_ACTIVE));
        label_ = new QLabel(this);
        label_->setAlignment(Qt::AlignCenter);
        label_->setAttribute(Qt::WA_TransparentForMouseEvents);
        render(false);
    }
    void render(bool active) {
        const auto theme = welcome_theme();
        const int icon_sz = dialog_scaled(active ? ICON_ACTIVE : ICON_NORMAL);
        const int btn_sz = dialog_scaled(active ? BTN_ACTIVE : BTN_NORMAL);
        const int offset = (dialog_scaled(BTN_ACTIVE) - btn_sz) / 2;
        label_->setGeometry(offset, offset, btn_sz, btn_sz);
        const QIcon icon = to_qicon(ResourceManager::get_resource_path(svg_rel_), this);
        QPixmap pix;
        if (!icon.isNull()) {
            pix = icon.pixmap(QSize(icon_sz, icon_sz));
        } else {
            pix = QPixmap(icon_sz, icon_sz);
            pix.fill(Qt::transparent);
        }
        QPixmap bg(btn_sz, btn_sz);
        bg.fill(Qt::transparent);
        QPainter painter(&bg);
        painter.setRenderHint(QPainter::Antialiasing);
        if (active) {
            painter.setPen(QPen(QColor(theme.accent), 2));
            painter.setBrush(QColor(theme.accent_soft));
        } else {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(theme.panel_subtle));
        }
        painter.drawRoundedRect(0, 0, btn_sz, btn_sz, dialog_scaled(10), dialog_scaled(10));
        const int ix = (btn_sz - icon_sz) / 2;
        painter.drawPixmap(ix, ix, pix);
        painter.end();
        label_->setPixmap(bg);
    }
    void set_active(bool active) {
        if (active_ == active) return;
        active_ = active;
        render(active);
    }
    bool active() const { return active_; }
private:
    QString svg_rel_;
    bool active_ = false;
    QLabel *label_;
};

// The compact rotating tool preview.
class ToolPreviewIllus : public IllustrationArea {
    Q_OBJECT
public:
    using IllustrationArea::IllustrationArea;
    void retranslate() override {
        if (0 <= current_ && current_ < TOOL_COUNT) {
            name_lbl_->setText(tool_text(TOOLS[current_].name_key));
            desc_lbl_->setText(tool_text(TOOLS[current_].desc_key));
        }
    }
    Q_INVOKABLE void apply_welcome_child_theme() {
        for (auto *cell : cells_) cell->render(cell->active());
        apply_welcome_label_style(name_lbl_);
        apply_welcome_label_style(desc_lbl_);
        update();
    }
protected:
    void build_content() override {
        layout_->setContentsMargins(dialog_scaled(14), dialog_scaled(16), dialog_scaled(14), dialog_scaled(12));
        layout_->setSpacing(dialog_scaled(5));
        // Three icon rows suit the narrow preview column of the horizontal wizard.
        const std::pair<int, int> rows[] = {{0, 5}, {5, 10}, {10, TOOL_COUNT}};
        for (const auto &[begin, end] : rows) {
            auto *row_w = new QWidget;
            set_own_style(row_w, "background: transparent;");
            auto *row_l = new QHBoxLayout(row_w);
            row_l->setContentsMargins(0, 0, 0, 0);
            row_l->setSpacing(dialog_scaled(3));
            row_l->addStretch();
            for (int i = begin; i < end; ++i) {
                auto *cell = new IconCell(QString::fromUtf8(TOOLS[i].svg), row_w);
                row_l->addWidget(cell);
                cells_.push_back(cell);
            }
            row_l->addStretch();
            layout_->addWidget(row_w);
        }
        // Name and description, compact.
        auto *info_w = new QWidget;
        set_own_style(info_w, "background: transparent;");
        auto *info_l = new QVBoxLayout(info_w);
        info_l->setContentsMargins(dialog_scaled(8), dialog_scaled(2), dialog_scaled(8), 0);
        info_l->setSpacing(dialog_scaled(1));
        name_lbl_ = new QLabel;
        name_lbl_->setAlignment(Qt::AlignCenter);
        set_welcome_label_style(name_lbl_, "accent", 13, 700);
        desc_lbl_ = new QLabel;
        desc_lbl_->setAlignment(Qt::AlignCenter);
        desc_lbl_->setWordWrap(true);
        set_welcome_label_style(desc_lbl_, "muted", 11, 400);
        opacity_effect_ = new QGraphicsOpacityEffect(info_w);
        info_w->setGraphicsEffect(opacity_effect_);
        fade_anim_ = new QPropertyAnimation(opacity_effect_, "opacity", this);
        fade_anim_->setDuration(300);
        fade_anim_->setEasingCurve(QEasingCurve::OutCubic);
        info_l->addWidget(name_lbl_);
        info_l->addWidget(desc_lbl_);
        layout_->addWidget(info_w);

        timer_ = new QTimer(this);
        connect(timer_, &QTimer::timeout, this, &ToolPreviewIllus::advance);
        timer_->start(ANIM_MS);
        advance();
    }
private:
    void advance() {
        if (0 <= current_ && current_ < TOOL_COUNT) cells_[current_]->set_active(false);
        current_ = (current_ + 1) % TOOL_COUNT;
        cells_[current_]->set_active(true);
        name_lbl_->setText(tool_text(TOOLS[current_].name_key));
        desc_lbl_->setText(tool_text(TOOLS[current_].desc_key));
        opacity_effect_->setOpacity(0.0);
        fade_anim_->stop();
        fade_anim_->setStartValue(0.0);
        fade_anim_->setEndValue(1.0);
        fade_anim_->start();
    }
    std::vector<IconCell *> cells_;
    int current_ = -1;
    QLabel *name_lbl_ = nullptr, *desc_lbl_ = nullptr;
    QGraphicsOpacityEffect *opacity_effect_ = nullptr;
    QPropertyAnimation *fade_anim_ = nullptr;
    QTimer *timer_ = nullptr;
};

QString page_title() { return welcome_tr("📸 截图设置").remove(QString::fromUtf8("📸")).trimmed(); }
}

ScreenshotHotkeyPage::ScreenshotHotkeyPage(ToolSettingsManager *config, QWidget *parent)
    : BasePage(page_title(), welcome_tr("设置截图的保存方式与选区方式。"), parent), config_(config) {
    init();
}

IllustrationArea *ScreenshotHotkeyPage::create_illustration() { return new ToolPreviewIllus(this); }

void ScreenshotHotkeyPage::build_controls(QVBoxLayout *layout) {
    // ── Auto-save ── before the folder: path and format only matter with auto-save on, so
    // first whether to save, then where and how.
    autosave_toggle_ = new WelcomeToggleSwitch;
    autosave_toggle_->setChecked(config_->get_screenshot_save_enabled());
    connect(autosave_toggle_, &SwitchButton::checkedChanged, this, &ScreenshotHotkeyPage::sync_save_section_enabled);
    QWidget *autosave_row = nullptr;
    std::tie(autosave_row, autosave_lbl_, autosave_desc_) = make_setting_row_with_refs(welcome_tr("自动保存截图"), autosave_toggle_, welcome_tr("关闭时截图只复制到剪贴板，不落盘。"));
    layout->addWidget(autosave_row);
    layout->addSpacing(dialog_scaled(12));

    // ── Save folder ──
    save_lbl_ = new QLabel(QString::fromUtf8("截图保存位置"));
    set_welcome_label_style(save_lbl_, "primary", 14, 600);
    layout->addWidget(save_lbl_);
    save_desc_ = new QLabel(QString::fromUtf8("确认截图后自动保存到该文件夹。"));
    save_desc_->setWordWrap(true);
    set_welcome_label_style(save_desc_, "muted", 12, 400);
    layout->addWidget(save_desc_);
    layout->addSpacing(dialog_scaled(4));
    // Path input and browse button side by side.
    auto *path_row = new QHBoxLayout;
    path_row->setSpacing(dialog_scaled(6));
    path_edit_ = new LineEdit;
    path_edit_->setText(config_->get_screenshot_save_path());
    path_edit_->setPlaceholderText("...");
    path_edit_->setFixedHeight(dialog_scaled(36));
    path_edit_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    browse_btn_ = new PushButton(welcome_tr("浏览"), nullptr, FluentIcon::Folder);
    browse_btn_->setFixedHeight(dialog_scaled(36));
    browse_btn_->setMinimumWidth(dialog_scaled(92));
    browse_btn_->setCursor(Qt::PointingHandCursor);
    connect(browse_btn_, &QPushButton::clicked, this, &ScreenshotHotkeyPage::browse_path);
    path_row->addWidget(path_edit_);
    path_row->addWidget(browse_btn_);
    layout->addLayout(path_row);

    // ── Save format ── (the same list as the settings window)
    layout->addSpacing(dialog_scaled(12));
    format_combo_ = new ComboBox;
    format_combo_->setFixedWidth(dialog_scaled(112));
    format_combo_->setFixedHeight(dialog_scaled(32));
    format_combo_->setCursor(Qt::PointingHandCursor);
    for (const auto &[display, value] : {std::make_pair("PNG", "PNG"), std::make_pair("JPG", "JPG"), std::make_pair("BMP", "BMP"), std::make_pair("WebP", "WEBP"), std::make_pair("PDF", "PDF")})
        format_combo_->addItem(display, QString(value));
    const QString current = config_->get_screenshot_format().isEmpty() ? QString("PNG") : config_->get_screenshot_format().toUpper();
    const int index = format_combo_->findData(current);
    format_combo_->setCurrentIndex(index >= 0 ? index : 0);
    QWidget *format_row = nullptr;
    QLabel *unused = nullptr;
    std::tie(format_row, format_lbl_, unused) = make_setting_row_with_refs(welcome_tr("保存格式"), format_combo_);
    layout->addWidget(format_row);

    // ── Smart selection ──
    layout->addSpacing(dialog_scaled(12));
    smart_toggle_ = new WelcomeToggleSwitch;
    smart_toggle_->setChecked(config_->get_smart_selection());
    QWidget *smart_row = nullptr;
    std::tie(smart_row, smart_lbl_, smart_desc_) = make_setting_row_with_refs(welcome_tr("智能选区"), smart_toggle_, welcome_tr("悬停时自动识别窗口边界，单击即可精准截取。"));
    layout->addWidget(smart_row);

    sync_save_section_enabled(autosave_toggle_->isChecked());
}

void ScreenshotHotkeyPage::sync_save_section_enabled(bool enabled) {
    for (QWidget *widget : std::initializer_list<QWidget *>{save_lbl_, save_desc_, path_edit_, browse_btn_, format_combo_, format_lbl_}) widget->setEnabled(enabled);
}

void ScreenshotHotkeyPage::browse_path() {
    const QString current = path_edit_->text().trimmed();
    const QString folder = get_existing_directory(this, welcome_tr("选择保存文件夹"), current);
    if (!folder.isEmpty()) path_edit_->setText(folder);
}

void ScreenshotHotkeyPage::retranslate() {
    title_label->setText(page_title());
    subtitle_label->setText(welcome_tr("设置截图的保存方式与选区方式。"));
    autosave_lbl_->setText(welcome_tr("自动保存截图"));
    if (autosave_desc_) autosave_desc_->setText(welcome_tr("关闭时截图只复制到剪贴板，不落盘。"));
    format_lbl_->setText(welcome_tr("保存格式"));
    smart_lbl_->setText(welcome_tr("智能选区"));
    if (smart_desc_) smart_desc_->setText(welcome_tr("悬停时自动识别窗口边界，单击即可精准截取。"));
    save_lbl_->setText(welcome_tr("截图保存位置"));
    save_desc_->setText(welcome_tr("确认截图后自动保存到该文件夹。"));
    browse_btn_->setText(welcome_tr("浏览"));
    illus_area->retranslate();
}

void ScreenshotHotkeyPage::save() {
    config_->set_screenshot_save_enabled(autosave_toggle_->isChecked());
    config_->set_smart_selection(smart_toggle_->isChecked());
    const QString fmt = format_combo_->currentData().toString();
    if (!fmt.isEmpty()) config_->set_screenshot_format(fmt);
    const QString path = path_edit_->text().trimmed();
    if (!path.isEmpty()) {
        config_->set_screenshot_save_path(path);
        QDir().mkpath(path);
    }
}
}

#include "page2_screenshot.moc"
