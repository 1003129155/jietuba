#include "ui/welcome/wizard.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "core/resource_manager.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "main_app.h"
#include "settings/tool_settings.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/titlebar.h"
#include "ui/welcome/pages.h"
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace jietuba {
namespace {
QString wizard_title() { return brand_text(welcome_tr("欢迎使用截图吧")); }
}

StepItem::StepItem(int index, QWidget *parent) : QFrame(parent), index_(index) {
    setObjectName("StepItem");
    setFixedHeight(dialog_scaled(52));
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(dialog_scaled(10), dialog_scaled(7), dialog_scaled(10), dialog_scaled(7));
    row->setSpacing(dialog_scaled(11));
    number_ = new QLabel(QString::number(index + 1), this);
    number_->setObjectName("StepNumber");
    number_->setFixedSize(dialog_scaled(28), dialog_scaled(28));
    number_->setAlignment(Qt::AlignCenter);
    number_->setAttribute(Qt::WA_TransparentForMouseEvents);
    text_ = new QLabel("", this);
    text_->setObjectName("StepText");
    text_->setWordWrap(false);
    text_->setAttribute(Qt::WA_TransparentForMouseEvents);
    row->addWidget(number_);
    row->addWidget(text_, 1);
    set_state(false, false);
}

void StepItem::set_text(const QString &text) {
    text_->setText(text);
    text_->setToolTip(text);
    setAccessibleName(text);
}

void StepItem::set_state(bool active, bool completed) {
    active_ = active;
    completed_ = completed;
    apply_welcome_theme();
}

void StepItem::apply_welcome_theme() {
    const auto theme = welcome_theme();
    const bool interactive = hovered_ || hasFocus();
    const auto s = [](int base) { return dialog_scaled(base); };
    QString item_bg, item_border, number_bg, number_color, number_border, text_color;
    int number_weight = 700, text_weight = 700;
    if (active_) {
        item_bg = theme.accent_soft, item_border = theme.border_strong;
        number_bg = theme.accent, number_color = "#FFFFFF", number_border = "none";
        text_color = theme.text;
    } else if (completed_) {
        item_bg = interactive ? theme.panel_subtle : "transparent";
        item_border = interactive ? theme.border : "transparent";
        number_bg = theme.panel, number_color = theme.accent, number_border = "1px solid " + theme.border_strong;
        text_color = theme.text, text_weight = 600;
    } else {
        item_bg = interactive ? theme.panel_subtle : "transparent";
        item_border = interactive ? theme.border : "transparent";
        number_bg = "transparent", number_color = theme.text_soft, number_border = "1px solid " + theme.border_strong;
        number_weight = 600;
        text_color = theme.text_muted, text_weight = 500;
    }
    setStyleSheet(QString(R"(
        #StepItem {
            background: %1;
            border: 1px solid %2;
            border-radius: %3px;
        }
        #StepNumber {
            background: %4;
            color: %5;
            border: %6;
            border-radius: %7px;
            font-size: %8px;
            font-weight: %9;
        }
        #StepText {
            color: %10;
            background: transparent;
            border: none;
            font-size: %11px;
            font-weight: %12;
        }
    )")
                      .arg(item_bg, item_border)
                      .arg(s(10))
                      .arg(number_bg, number_color, number_border)
                      .arg(s(14))
                      .arg(s(12))
                      .arg(number_weight)
                      .arg(text_color)
                      .arg(s(13))
                      .arg(text_weight));
}

void StepItem::enterEvent(QEnterEvent *event) {
    hovered_ = true;
    apply_welcome_theme();
    QFrame::enterEvent(event);
}

void StepItem::leaveEvent(QEvent *event) {
    hovered_ = false;
    apply_welcome_theme();
    QFrame::leaveEvent(event);
}

void StepItem::focusInEvent(QFocusEvent *event) {
    apply_welcome_theme();
    QFrame::focusInEvent(event);
}

void StepItem::focusOutEvent(QFocusEvent *event) {
    apply_welcome_theme();
    QFrame::focusOutEvent(event);
}

void StepItem::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) emit clicked(index_);
    QFrame::mouseReleaseEvent(event);
}

void StepItem::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Space) {
        emit clicked(index_);
        event->accept();
        return;
    }
    QFrame::keyPressEvent(event);
}

WelcomeWizard::WelcomeWizard(ToolSettingsManager *config, QWidget *parent) : FrostedFramelessDialog(parent), config_(config) {
    setup_titlebar();
    // The language is detected and loaded before any page is created.
    init_language();
    setWindowTitle(wizard_title());
    // Window flags first (they recreate the native window), then the fixed size; the other order
    // drops the size constraint and the layout keeps recomputing while dragging. The fixed-size
    // hint also locks resizing on Windows.
    setWindowFlags(windowFlags() | Qt::MSWindowsFixedSizeDialogHint);
    setFixedSize(dialog_scaled(WINDOW_W), dialog_scaled(WINDOW_H));
    // Hotkeys are unregistered and global mouse shortcuts suspended during the wizard; the whole
    // program is in setup, so other windows are blocked.
    setWindowModality(Qt::ApplicationModal);
    set_own_style(this, "background: transparent; border: none;");
    build_ui();
    build_pages();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { apply_welcome_theme(); });
    apply_welcome_theme();
    refresh_theme_scope();
    update_nav();
    // Every page exists now; one refresh shows the detected language instead of the source text.
    retranslate_ui();
}

void WelcomeWizard::setup_titlebar() {
    auto *title_bar = new FluentTitleBar(this);
    configure_dialog_control(*title_bar);
    setTitleBar(title_bar);
    if (title_bar->maxBtn) title_bar->maxBtn->hide();
    if (title_bar->minBtn) title_bar->minBtn->hide();
    title_bar->setDoubleClickEnabled(false);
}

void WelcomeWizard::init_language() {
    const QString saved = config_->get_app_setting("language", "").toString();
    const QString init_lang = !saved.isEmpty() && I18nManager::LANGUAGES().contains(saved) ? saved : I18nManager::get_system_language();
    if (I18nManager::get_current_language() != init_lang) I18nManager::load_language(init_lang);
}

void WelcomeWizard::build_ui() {
    auto *root = new QVBoxLayout(this);
    const int title_height = titleBar() ? titleBar()->height() : dialog_scaled(32);
    root->setContentsMargins(dialog_scaled(8), title_height + dialog_scaled(4), dialog_scaled(8), dialog_scaled(8));
    root->setSpacing(0);

    shell_ = new QFrame(this);
    shell_->setObjectName("WizardSurface");
    auto *shell_layout = new QHBoxLayout(shell_);
    shell_layout->setContentsMargins(0, 0, 0, 0);
    shell_layout->setSpacing(0);

    // Left: brand and step navigation.
    sidebar_ = new QFrame(shell_);
    sidebar_->setObjectName("WizardSidebar");
    sidebar_->setFixedWidth(dialog_scaled(224));
    auto *side_layout = new QVBoxLayout(sidebar_);
    side_layout->setContentsMargins(dialog_scaled(20), dialog_scaled(23), dialog_scaled(20), dialog_scaled(20));
    side_layout->setSpacing(dialog_scaled(4));
    auto *brand_row = new QHBoxLayout;
    brand_row->setSpacing(dialog_scaled(11));
    brand_icon_ = new QLabel(sidebar_);
    brand_icon_->setFixedSize(dialog_scaled(38), dialog_scaled(38));
    brand_icon_->setAlignment(Qt::AlignCenter);
    const QIcon icon(ResourceManager::get_resource_path(QString::fromUtf8("svg/托盘.svg")));
    if (!icon.isNull()) brand_icon_->setPixmap(icon.pixmap(dialog_scaled(24), dialog_scaled(24)));
    auto *brand_text_box = new QVBoxLayout;
    brand_text_box->setSpacing(0);
    brand_name_ = new QLabel(PRODUCT_NAME, sidebar_);
    brand_meta_ = new QLabel("PRODUCT SETUP", sidebar_);
    brand_text_box->addWidget(brand_name_);
    brand_text_box->addWidget(brand_meta_);
    brand_row->addWidget(brand_icon_);
    brand_row->addLayout(brand_text_box, 1);
    side_layout->addLayout(brand_row);
    side_layout->addSpacing(dialog_scaled(24));
    // One step per page.
    for (int idx = 0; idx < PAGE_COUNT; ++idx) {
        auto *item = new StepItem(idx, sidebar_);
        connect(item, &StepItem::clicked, this, &WelcomeWizard::go_to_page);
        step_items_.push_back(item);
        side_layout->addWidget(item);
    }
    side_layout->addStretch();
    setup_meta_ = new QLabel("v" + APP_VERSION, sidebar_);
    setup_meta_->setObjectName("SetupMeta");
    side_layout->addWidget(setup_meta_);

    // Right: pages and navigation.
    right_ = new QWidget(shell_);
    right_->setObjectName("WizardContent");
    auto *right_layout = new QVBoxLayout(right_);
    right_layout->setContentsMargins(0, 0, 0, 0);
    right_layout->setSpacing(0);
    stack_ = new QStackedWidget(right_);
    right_layout->addWidget(stack_, 1);
    nav_line_ = new QFrame(right_);
    nav_line_->setFixedHeight(dialog_scaled(1));
    right_layout->addWidget(nav_line_);
    nav_ = new QWidget(right_);
    nav_->setFixedHeight(dialog_scaled(66));
    auto *nav_layout = new QHBoxLayout(nav_);
    nav_layout->setContentsMargins(dialog_scaled(30), dialog_scaled(14), dialog_scaled(30), dialog_scaled(14));
    nav_layout->setSpacing(dialog_scaled(10));
    btn_skip_ = new TransparentPushButton;
    btn_skip_->setFixedHeight(dialog_scaled(36));
    btn_skip_->setCursor(Qt::PointingHandCursor);
    connect(btn_skip_, &QPushButton::clicked, this, &WelcomeWizard::finish);
    page_count_ = new QLabel(nav_);
    btn_back_ = new PushButton;
    btn_back_->setObjectName("btnBack");
    btn_back_->setFixedSize(dialog_scaled(96), dialog_scaled(36));
    btn_back_->setCursor(Qt::PointingHandCursor);
    connect(btn_back_, &QPushButton::clicked, this, &WelcomeWizard::go_back);
    btn_next_ = new PrimaryPushButton;
    btn_next_->setObjectName("btnNext");
    btn_next_->setFixedSize(dialog_scaled(112), dialog_scaled(36));
    btn_next_->setCursor(Qt::PointingHandCursor);
    connect(btn_next_, &QPushButton::clicked, this, &WelcomeWizard::go_next);
    nav_layout->addWidget(btn_skip_);
    nav_layout->addStretch();
    nav_layout->addWidget(page_count_);
    nav_layout->addSpacing(dialog_scaled(8));
    nav_layout->addWidget(btn_back_);
    nav_layout->addWidget(btn_next_);
    right_layout->addWidget(nav_);
    shell_layout->addWidget(sidebar_);
    shell_layout->addWidget(right_, 1);
    root->addWidget(shell_, 1);
}

void WelcomeWizard::apply_welcome_theme() {
    const auto theme = welcome_theme();
    const auto s = [](int base) { return dialog_scaled(base); };
    setPalette(UIThemeManager::build_palette(get_ui_theme()->tokens()));
    shell_->setStyleSheet(QString("#WizardSurface { background: %1; border: 1px solid %2; border-radius: %3px; }").arg(theme.page, theme.border).arg(s(14)));
    sidebar_->setStyleSheet(QString("#WizardSidebar { background: %1; border: none; border-right: 1px solid %2; border-radius: %3px; }").arg(theme.sidebar, theme.border).arg(s(14)));
    right_->setStyleSheet(QString("#WizardContent { background: %1; border: none; }").arg(theme.page));
    set_own_style(nav_, QString("background: %1; border: none;").arg(theme.page));
    set_own_style(nav_line_, QString("background: %1; border: none;").arg(theme.separator));
    set_own_style(brand_icon_, QString("background: %1; border: 1px solid %2; border-radius: %3px;").arg(theme.panel, theme.border).arg(s(10)));
    set_own_style(brand_name_, QString("font-size: %1px; font-weight: 700; color: %2; background: transparent;").arg(s(17)).arg(theme.text));
    set_own_style(brand_meta_, QString("font-size: %1px; font-weight: 600; letter-spacing: 1px; color: %2; background: transparent;").arg(s(9)).arg(theme.text_soft));
    set_own_style(setup_meta_, QString("font-size: %1px; font-weight: 600; color: %2; background: transparent; letter-spacing: 1px;").arg(s(10)).arg(theme.text_soft));
    set_own_style(page_count_, QString("font-size: %1px; font-weight: 600; color: %2; background: transparent;").arg(s(12)).arg(theme.text_muted));
    for (auto *item : step_items_) item->apply_welcome_theme();
    for (auto *page : pages_) page->apply_welcome_theme();
    update();
}

void WelcomeWizard::build_pages() {
    pages_ = {
        new WelcomePage(config_, this),
        new HotkeyPage(config_),
        new ScreenshotHotkeyPage(config_),
        new ClipboardHotkeyPage(config_),
        new TranslationPage(config_),
        new FinishPage(config_),
    };
    for (auto *page : pages_) stack_->addWidget(page);
    // Every page is built at once and switching only changes visibility, so one sweep covers
    // the fluent_lite controls (self-painted widgets keep their own base sizes).
    configure_dialog_controls(*stack_);
    refresh_step_labels();
}

void WelcomeWizard::refresh_theme_scope() {
    for (auto *widget : findChildren<QWidget *>()) {
        if (widget->metaObject()->indexOfMethod("_apply_theme()") >= 0) QMetaObject::invokeMethod(widget, "_apply_theme");
        else widget->update();
    }
}

void WelcomeWizard::go_to_page(int index) {
    if (index < 0 || index >= page_count()) return;
    current_ = index;
    stack_->setCurrentIndex(index);
    update_nav();
}

void WelcomeWizard::go_next() {
    if (current_ < page_count() - 1) go_to_page(current_ + 1);
    else finish();
}

void WelcomeWizard::go_back() {
    if (current_ > 0) go_to_page(current_ - 1);
}

void WelcomeWizard::update_nav() {
    for (int idx = 0; idx < static_cast<int>(step_items_.size()); ++idx) step_items_[idx]->set_state(idx == current_, idx < current_);
    page_count_->setText(QString("%1 / %2").arg(current_ + 1).arg(page_count()));
    btn_back_->setEnabled(current_ > 0);
    const bool is_last = current_ == page_count() - 1;
    btn_skip_->setVisible(!is_last);
    btn_back_->setText(welcome_tr("上一步"));
    btn_back_->setToolTip(welcome_tr("上一步"));
    btn_back_->setIcon(QIcon());
    const QString next = is_last ? welcome_tr("完成") : welcome_tr("下一步");
    btn_next_->setText(next);
    btn_next_->setToolTip(next);
    btn_next_->setIcon(QIcon());
}

void WelcomeWizard::retranslate_ui() {
    setWindowTitle(wizard_title());
    btn_skip_->setText(welcome_tr("跳过"));
    update_nav();
    for (auto *page : pages_) {
        try {
            page->retranslate();
        } catch (const std::exception &e) {
            log_exception(e, T("向导页面刷新翻译"));
        }
    }
    refresh_step_labels();
}

void WelcomeWizard::refresh_step_labels() {
    for (size_t i = 0; i < std::min(step_items_.size(), pages_.size()); ++i) step_items_[i]->set_text(pages_[i]->title_label ? pages_[i]->title_label->text() : QString());
}

void WelcomeWizard::save_all() {
    if (saved_) return;
    saved_ = true;
    for (auto *page : pages_) {
        try {
            page->save();
        } catch (const std::exception &e) {
            log_exception(e, LogMsg(QString("WelcomeWizard save %1").arg(page->metaObject()->className())));
        }
    }
    config_->mark_as_run();
}

void WelcomeWizard::finish() {
    save_all();
    accept();
}

void WelcomeWizard::closeEvent(QCloseEvent *event) {
    save_all();
    FrostedFramelessDialog::closeEvent(event);
}
}
