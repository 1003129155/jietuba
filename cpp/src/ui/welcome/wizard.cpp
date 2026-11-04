#include "ui/welcome/wizard.h"
#include "core/i18n.h"
#include "core/logger.h"
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
#include <QPainter>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace jietuba {
namespace {
QString wizard_title() { return brand_text(welcome_tr("欢迎使用截图吧")); }
}

StepItem::StepItem(int index, QWidget *parent) : QWidget(parent), index_(index) {
    setFixedSize(dialog_scaled(30), dialog_scaled(20));
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
}

void StepItem::set_text(const QString &text) {
    setToolTip(text);
    setAccessibleName(text);
}

void StepItem::set_state(bool active, bool completed) {
    active_ = active;
    completed_ = completed;
    update();
}

void StepItem::paintEvent(QPaintEvent *) {
    const auto theme = welcome_theme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    // A short bar in the middle of a taller hit area; done and current steps are filled.
    const double bar_h = dialog_scaled_f(4);
    const QRectF bar(0, (height() - bar_h) / 2, width(), bar_h);
    QColor color(active_ || completed_ ? theme.accent : theme.border_strong);
    if (hovered_ && !active_) color = completed_ ? QColor(theme.accent_hover) : QColor(theme.text_soft);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawRoundedRect(bar, bar_h / 2, bar_h / 2);
    if (hasFocus()) {
        p.setPen(QPen(QColor(theme.accent), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), dialog_scaled_f(4), dialog_scaled_f(4));
    }
}

void StepItem::enterEvent(QEnterEvent *event) {
    hovered_ = true;
    update();
    QWidget::enterEvent(event);
}

void StepItem::leaveEvent(QEvent *event) {
    hovered_ = false;
    update();
    QWidget::leaveEvent(event);
}

void StepItem::focusInEvent(QFocusEvent *event) {
    update();
    QWidget::focusInEvent(event);
}

void StepItem::focusOutEvent(QFocusEvent *event) {
    update();
    QWidget::focusOutEvent(event);
}

void StepItem::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) emit clicked(index_);
    QWidget::mouseReleaseEvent(event);
}

void StepItem::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Space) {
        emit clicked(index_);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
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
    const QString saved = config_->get_language("");
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

    // Left: the page's demo stage, inset from the window edges.
    auto *stage_box = new QWidget(shell_);
    stage_box->setFixedWidth(dialog_scaled(STAGE_W));
    auto *stage_layout = new QVBoxLayout(stage_box);
    stage_layout->setContentsMargins(dialog_scaled(14), dialog_scaled(14), 0, dialog_scaled(14));
    stage_ = new QStackedWidget(stage_box);
    stage_layout->addWidget(stage_);

    // Right: progress, the page's title and settings, then the buttons.
    right_ = new QWidget(shell_);
    right_->setObjectName("WizardContent");
    auto *right_layout = new QVBoxLayout(right_);
    right_layout->setContentsMargins(dialog_scaled(34), dialog_scaled(26), dialog_scaled(34), dialog_scaled(22));
    right_layout->setSpacing(0);
    auto *progress = new QHBoxLayout;
    progress->setSpacing(dialog_scaled(6));
    for (int idx = 0; idx < PAGE_COUNT; ++idx) {
        auto *item = new StepItem(idx, right_);
        connect(item, &StepItem::clicked, this, &WelcomeWizard::go_to_page);
        step_items_.push_back(item);
        progress->addWidget(item);
    }
    progress->addStretch();
    page_count_ = new QLabel(right_);
    progress->addWidget(page_count_);
    right_layout->addLayout(progress);
    right_layout->addSpacing(dialog_scaled(26));
    stack_ = new QStackedWidget(right_);
    right_layout->addWidget(stack_, 1);
    auto *nav_layout = new QHBoxLayout;
    nav_layout->setContentsMargins(0, dialog_scaled(14), 0, 0);
    nav_layout->setSpacing(dialog_scaled(10));
    btn_skip_ = new TransparentPushButton;
    btn_skip_->setFixedHeight(dialog_scaled(36));
    btn_skip_->setCursor(Qt::PointingHandCursor);
    connect(btn_skip_, &QPushButton::clicked, this, &WelcomeWizard::finish);
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
    nav_layout->addWidget(btn_back_);
    nav_layout->addWidget(btn_next_);
    right_layout->addLayout(nav_layout);
    shell_layout->addWidget(stage_box);
    shell_layout->addWidget(right_, 1);
    root->addWidget(shell_, 1);
}

void WelcomeWizard::apply_welcome_theme() {
    const auto theme = welcome_theme();
    const auto s = [](int base) { return dialog_scaled(base); };
    setPalette(UIThemeManager::build_palette(get_ui_theme()->tokens()));
    shell_->setStyleSheet(QString("#WizardSurface { background: %1; border: 1px solid %2; border-radius: %3px; }").arg(theme.page, theme.border).arg(s(14)));
    right_->setStyleSheet(QString("#WizardContent { background: %1; border: none; }").arg(theme.page));
    set_own_style(page_count_, QString("font-size: %1px; font-weight: 600; color: %2; background: transparent;").arg(s(12)).arg(theme.text_muted));
    for (auto *item : step_items_) item->apply_welcome_theme();
    for (auto *page : pages_) page->apply_welcome_theme();
    update();
}

void WelcomeWizard::build_pages() {
    // Only what a new user decides now; everything else waits in the settings window.
    pages_ = {
        new WelcomePage(config_, this),
        new HotkeyPage(config_),
        new MouseShortcutPage(config_),
        new StoragePage(config_),
        new FinishPage(config_),
    };
    for (auto *page : pages_) {
        stack_->addWidget(page);
        stage_->addWidget(page->illus_area);
    }
    // Every page is built at once and switching only changes visibility, so one sweep covers
    // the fluent_lite controls of the pages and the buttons below them.
    configure_dialog_controls(*this);
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
    stage_->setCurrentIndex(index);
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
    // Nothing to go back to on the first page.
    btn_back_->setVisible(current_ > 0);
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
