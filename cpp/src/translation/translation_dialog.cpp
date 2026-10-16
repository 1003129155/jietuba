#include "translation/translation_dialog.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "core/platform_utils.h"
#include "core/resource_manager.h"
#include "core/ui_scale.h"
#include "settings/tool_settings.h"
#include "translation/languages.h"
#include "translation/ui/widgets.h"
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QLocale>
#include <QPainter>
#include <QRegularExpression>
#include <QScreen>
#include <QStyle>
#include <QVBoxLayout>
#include <algorithm>

namespace jietuba {
namespace {
QString tr_text(const char *text) { return i18n_tr(text, "TranslationDialog"); }
}

const DashboardPalette &dashboard_dark() {
    static const DashboardPalette palette{
        "#45474e", "#2c2d33", "#555860", "#f4f6f9", "#9aa1ae", "#686d79", "#3f8cff", "#62a2ff", "rgba(63, 140, 255, 0.16)",
        "rgba(125, 132, 148, 0.22)", "rgba(125, 132, 148, 0.34)", QColor(125, 132, 148, 56), QColor(125, 132, 148, 87), "#ff5449", "#35d06a",
    };
    return palette;
}

const DashboardPalette &dashboard_light() {
    static const DashboardPalette palette{
        "#f5f5f7", "#ffffff", "#ffffff", "#1d1d1f", "#73737a", "#9999a1", "#007aff", "#218cff", "rgba(0, 122, 255, 0.12)",
        "rgba(120, 120, 128, 0.15)", "rgba(120, 120, 128, 0.25)", QColor(120, 120, 128, 38), QColor(120, 120, 128, 64), "#ff3b30", "#34c759",
    };
    return palette;
}

void set_badge_text(QLabel *badge, const QString &text, int padding) {
    const int max_width = dialog_scaled(BADGE_MAX_WIDTH);
    badge->setMaximumWidth(max_width);
    // Font size and weight come from the style sheet; polish first to measure correctly.
    badge->ensurePolished();
    const auto shown = badge->fontMetrics().elidedText(text, Qt::ElideRight, max_width - 2 * dialog_scaled(padding));
    badge->setText(shown);
    badge->setToolTip(shown != text ? text : QString());
}

QString fill_dashboard_sheet(QString sheet, const DashboardPalette &p) {
    const auto s = [](double value) { return QString::number(dialog_scaled(value)); };
    // Longest names first so "@text_2" is not consumed by "@text".
    const std::vector<std::pair<QString, QString>> values = {
        {"@surface_strong", p.surface_strong}, {"@surface_radius", s(SURFACE_CORNER_RADIUS)}, {"@control_radius", s(CONTROL_CORNER_RADIUS)},
        {"@accent_tint", p.accent_tint}, {"@fill_hover", p.fill_hover}, {"@stroke", s(1)}, {"@text_2", p.text_2}, {"@text_3", p.text_3},
        {"@accent", p.accent}, {"@danger", p.danger}, {"@field", p.field}, {"@green", p.green}, {"@fill", p.fill}, {"@text", p.text},
    };
    for (const auto &[key, value] : values) sheet.replace(key, value);
    static const QRegularExpression scaled(R"(@s(\d+))");
    QString result;
    qsizetype last = 0;
    for (auto it = scaled.globalMatch(sheet); it.hasNext();) {
        const auto match = it.next();
        result += sheet.mid(last, match.capturedStart() - last) + s(match.captured(1).toInt());
        last = match.capturedEnd();
    }
    return result + sheet.mid(last);
}

VectorToolButton::VectorToolButton(const QString &icon_name, const QString &tooltip, QWidget *parent)
    : QAbstractButton(parent), icon_name_(icon_name), palette_(&dashboard_dark()) {
    setToolTip(tooltip);
    setAccessibleName(tooltip);
    setCursor(Qt::PointingHandCursor);
    setFixedSize(dialog_scaled(32), dialog_scaled(30));
}

void VectorToolButton::set_tooltip(const QString &tooltip) {
    setToolTip(tooltip);
    setAccessibleName(tooltip);
}

void VectorToolButton::apply_palette(const DashboardPalette &palette) {
    palette_ = &palette;
    update();
}

void VectorToolButton::enterEvent(QEnterEvent *event) {
    update();
    QAbstractButton::enterEvent(event);
}

void VectorToolButton::leaveEvent(QEvent *event) {
    update();
    QAbstractButton::leaveEvent(event);
}

void VectorToolButton::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (isDown()) painter.setBrush(palette_->fill_hover_color);
    else if (underMouse()) painter.setBrush(palette_->fill_color);
    else painter.setBrush(Qt::NoBrush);
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(QRectF(rect()).adjusted(dialog_scaled(2), dialog_scaled(2), -dialog_scaled(2), -dialog_scaled(2)), dialog_scaled(9), dialog_scaled(9));

    const QColor color(underMouse() ? palette_->text : palette_->text_2);
    QPen pen(color, 1.8);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    // The icon is a set of interlocking hand-drawn coordinates; scaling the whole
    // painter keeps their proportions, rounding each point would not.
    painter.save();
    painter.scale(get_dialog_scale().factor(), get_dialog_scale().factor());
    if (icon_name_ == "copy") {
        painter.drawRoundedRect(QRectF(7.5, 6.5, 11, 12), 2, 2);
        painter.drawRoundedRect(QRectF(11.5, 10.5, 11, 12), 2, 2);
    } else {
        painter.drawLine(QPointF(8.5, 9.0), QPointF(21.5, 9.0));
        painter.drawLine(QPointF(12.0, 6.0), QPointF(18.0, 6.0));
        painter.drawRoundedRect(QRectF(10.0, 11.0, 10.0, 12.0), 2, 2);
        painter.drawLine(QPointF(13.5, 14.0), QPointF(13.5, 20.0));
        painter.drawLine(QPointF(16.5, 14.0), QPointF(16.5, 20.0));
    }
    painter.restore();
}

TranslateButton::TranslateButton(QWidget *parent) : QAbstractButton(parent), palette_(&dashboard_dark()), idle_text_(tr_text("Translate")) {
    setText(idle_text_);
    setToolTip(tr_text("Translate text"));
    setCursor(Qt::PointingHandCursor);
    setFixedSize(dialog_scaled(96), dialog_scaled(34));
}

void TranslateButton::retranslate(bool busy) {
    idle_text_ = tr_text("Translate");
    setText(busy ? tr_text("Translating...") : idle_text_);
    setToolTip(tr_text("Translate text"));
    setAccessibleName(tr_text("Translate text"));
    update();
}

void TranslateButton::apply_palette(const DashboardPalette &palette) {
    palette_ = &palette;
    update();
}

void TranslateButton::enterEvent(QEnterEvent *event) {
    update();
    QAbstractButton::enterEvent(event);
}

void TranslateButton::leaveEvent(QEvent *event) {
    update();
    QAbstractButton::leaveEvent(event);
}

void TranslateButton::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QString color = underMouse() ? palette_->accent_hover : palette_->accent;
    if (!isEnabled()) color = "#6689bb";
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(color));
    painter.drawRoundedRect(QRectF(rect()), dialog_scaled(17), dialog_scaled(17));
    painter.setPen(QColor("white"));
    painter.setFont(QFont("Microsoft YaHei UI", dialog_scaled(10), QFont::DemiBold));
    painter.drawText(rect(), Qt::AlignCenter, text());
}

void DashboardPinButton::apply_palette(const DashboardPalette &palette) {
    palette_ = &palette;
    update();
}

std::pair<QColor, QColor> DashboardPinButton::colors() const {
    const auto &palette = *palette_;
    if (isChecked()) {
        QColor tint(palette.accent);
        tint.setAlpha(41);  // matches accent_tint's alpha
        return {QColor(palette.accent), tint};
    }
    if (isDown()) return {QColor(palette.text), palette.fill_hover_color};
    if (underMouse()) return {QColor(palette.text), palette.fill_color};
    return {QColor(palette.text_2), QColor(0, 0, 0, 0)};
}

DashboardTitleBar::DashboardTitleBar(QWidget *parent) : FluentTitleBar(parent) {
    // The base constructor applied the theme's caption colours; use the palette's.
    apply_caption_colors();
    hBoxLayout->setContentsMargins(dialog_scaled(12), 0, 0, 0);
    hBoxLayout->setSpacing(0);

    logo = iconLabel;
    logo->setObjectName("appLogo");
    logo->setAccessibleName(tr_text("Translation"));
    const auto logo_icon = ResourceManager::get_icon(ResourceManager::get_icon_path(QString::fromUtf8("翻译.svg")), dialog_scaled(22));
    if (logo_icon.isNull()) {
        logo->setText(tr_text("Translation"));
    } else {
        auto logo_pixmap = logo_icon.pixmap(dialog_scaled(16), dialog_scaled(16));
        QPainter logo_painter(&logo_pixmap);
        logo_painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        logo_painter.fillRect(logo_pixmap.rect(), QColor("white"));
        logo_painter.end();
        logo->setPixmap(logo_pixmap);
    }

    app_name = titleLabel;
    app_name->setText("jietuba");
    app_name->setObjectName("appName");

    pin_button = new DashboardPinButton(this);
    configure_dialog_control(*pin_button);
    pin_button->setObjectName("pinButton");
    pin_button->setAccessibleName(tr_text("Toggle always on top"));
    update_translations();

    hBoxLayout->insertWidget(hBoxLayout->indexOf(minBtn), pin_button);
}

void DashboardTitleBar::update_translations() {
    pin_button->setAccessibleName(tr_text("Toggle always on top"));
    // Caption symbols are universally understood; keep their names for
    // accessibility without redundant hover bubbles.
    const std::pair<QAbstractButton *, QString> buttons[] = {{minBtn, tr_text("Minimize")}, {maxBtn, tr_text("Maximize")}, {closeBtn, tr_text("Close")}};
    for (const auto &[button, name] : buttons) {
        button->setToolTip({});
        button->setAccessibleName(name);
    }
}

void DashboardTitleBar::apply_palette(const DashboardPalette &palette) {
    palette_ = &palette;
    pin_button->apply_palette(palette);
    apply_caption_colors();
}

void DashboardTitleBar::apply_caption_colors() {
    const auto &palette = *palette_;
    for (auto *button : {minBtn, maxBtn, closeBtn}) {
        button->setNormalColor(QColor(palette.text_2));
        button->setHoverColor(QColor(palette.text));
        button->setPressedColor(QColor(palette.text));
        button->setNormalBackgroundColor(QColor(0, 0, 0, 0));
        button->setHoverBackgroundColor(palette.fill_hover_color);
        button->setPressedBackgroundColor(palette.fill_color);
    }
    closeBtn->setHoverColor(QColor("white"));
    closeBtn->setHoverBackgroundColor(QColor(palette.danger));
}

void SourceEdit::keyPressEvent(QKeyEvent *event) {
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && (event->modifiers() & Qt::ControlModifier)) {
        emit translate_shortcut();
        return;
    }
    TextEdit::keyPressEvent(event);
}

bool FocusPane::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut) {
        setProperty("focused", event->type() == QEvent::FocusIn);
        style()->unpolish(this);
        style()->polish(this);
    }
    return QFrame::eventFilter(watched, event);
}

TranslationDialog::TranslationDialog(const QString &original_text, const QString &translated_text, QWidget *parent, std::optional<QPoint> position,
                                     const QString &source_lang, const QString &target_lang)
    : FramelessWindow(parent), original_text(original_text), translated_text(translated_text), source_lang(source_lang.isEmpty() ? "auto" : source_lang) {
    auto *config = get_tool_settings_manager();
    const auto saved_target_lang = config->get_app_setting("translation_target_lang", "").toString();
    this->target_lang = !saved_target_lang.isEmpty() ? saved_target_lang : !target_lang.isEmpty() ? target_lang : QString("ZH");
    is_on_top_ = config->get_translation_on_top();

    setObjectName("dashboardWindow");
    setWindowTitle("jietuba");
    setMinimumSize(dialog_scaled(MINIMUM_WIDTH), dialog_scaled(MINIMUM_HEIGHT));
    resize(dialog_scaled(DEFAULT_WIDTH), dialog_scaled(DEFAULT_HEIGHT));
    setFont(QFont("Microsoft YaHei UI", dialog_scaled(10)));
    setAttribute(Qt::WA_DeleteOnClose, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAutoFillBackground(false);

    dashboard_title_bar = new DashboardTitleBar(this);
    configure_dialog_control(*dashboard_title_bar);
    setTitleBar(dashboard_title_bar);
    dashboard_title_bar->pin_button->setChecked(is_on_top_);
    connect(dashboard_title_bar->pin_button, &QAbstractButton::clicked, this, &TranslationDialog::on_toggle_pin);
    update_pin_tooltip();

    build_ui();
    set_theme("dark");

    connect(I18nManager::instance().language_signals(), &I18nSignals::language_changed, this, [this](const QString &) { retranslate_ui(); });

    // TranslationManager supplies the selected provider and readiness.
    set_backend_badge(tr_text("Engine not configured"), false);
    retranslate_ui();

    place_initial_window(position);

    log_debug("Translation dashboard created", "Translation");
}

std::pair<int, int> TranslationDialog::initial_size_for_available_geometry(const QRect &available_geometry) {
    const int minimum_width = dialog_scaled(MINIMUM_WIDTH);
    const int minimum_height = dialog_scaled(MINIMUM_HEIGHT);
    const int default_width = dialog_scaled(DEFAULT_WIDTH);
    const int default_height = dialog_scaled(DEFAULT_HEIGHT);
    const int width = std::max(minimum_width, std::min(default_width, available_geometry.width()));
    const int height = std::max(minimum_height, std::min(default_height, available_geometry.height()));
    return {width, height};
}

void TranslationDialog::place_initial_window(std::optional<QPoint> position) {
    // Places a new window on the requested screen, or centred on the cursor's screen.
    const auto reference = position.value_or(QCursor::pos());
    auto *screen = QApplication::screenAt(reference);
    if (!screen) screen = QApplication::primaryScreen();
    if (!screen) {
        if (position) move(*position);
        return;
    }
    const auto available = screen->availableGeometry();
    const auto [width, height] = initial_size_for_available_geometry(available);
    resize(width, height);

    int desired_x, desired_y;
    if (!position) {
        desired_x = available.left() + python_floordiv(available.width() - width, 2);
        desired_y = available.top() + python_floordiv(available.height() - height, 2);
    } else {
        desired_x = position->x();
        desired_y = position->y();
    }
    const int max_x = std::max(available.left(), available.left() + available.width() - width);
    const int max_y = std::max(available.top(), available.top() + available.height() - height);
    move(std::min(std::max(desired_x, available.left()), max_x), std::min(std::max(desired_y, available.top()), max_y));
}

void TranslationDialog::build_ui() {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(dialog_scaled(18), dashboard_title_bar->height() + dialog_scaled(6), dialog_scaled(18), dialog_scaled(15));
    root->setSpacing(0);

    auto *language_bar = new QHBoxLayout;
    language_bar->setContentsMargins(0, dialog_scaled(2), 0, dialog_scaled(14));
    language_bar->setSpacing(dialog_scaled(12));
    language_bar->addStretch();

    source_language = language_combo(true);
    target_language = language_combo(false);
    set_combo_data(source_language, source_lang, "auto");
    set_combo_data(target_language, target_lang, "ZH");
    connect(target_language, &QComboBox::currentIndexChanged, this, &TranslationDialog::on_target_lang_changed);

    swap_button = new QPushButton(QString::fromUtf8("⇄"));
    swap_button->setObjectName("swapButton");
    swap_button->setToolTip(tr_text("Swap languages"));
    swap_button->setCursor(Qt::PointingHandCursor);
    swap_button->setFixedSize(dialog_scaled(36), dialog_scaled(36));
    connect(swap_button, &QPushButton::clicked, this, &TranslationDialog::swap_languages);

    language_bar->addWidget(source_language);
    language_bar->addWidget(swap_button);
    language_bar->addWidget(target_language);
    language_bar->addStretch();
    root->addLayout(language_bar);

    auto *panes = new QHBoxLayout;
    panes->setSpacing(dialog_scaled(13));

    source_pane = new FocusPane;
    source_pane->setObjectName("sourcePane");
    auto *source_layout = new QVBoxLayout(source_pane);
    source_layout->setContentsMargins(dialog_scaled(10), dialog_scaled(10), dialog_scaled(10), dialog_scaled(8));
    source_layout->setSpacing(dialog_scaled(2));

    source_edit = new SourceEdit;
    source_edit->setObjectName("sourceEdit");
    source_edit->setAcceptRichText(false);
    source_edit->setPlaceholderText(source_placeholder());
    source_edit->setPlainText(original_text);
    connect(source_edit, &SourceEdit::translate_shortcut, this, &TranslationDialog::request_translation);
    connect(source_edit, &QTextEdit::textChanged, this, &TranslationDialog::update_count);
    source_pane->watch_focus(source_edit);
    source_layout->addWidget(source_edit, 1);

    auto *source_tools = new QHBoxLayout;
    source_tools->setSpacing(dialog_scaled(3));
    copy_source_button = tool_button("copy", tr_text("Copy Original"));
    connect(copy_source_button, &QAbstractButton::clicked, this, &TranslationDialog::copy_source);
    clear_source_button = tool_button("trash", tr_text("Clear Original"));
    connect(clear_source_button, &QAbstractButton::clicked, source_edit, &QTextEdit::clear);
    character_count = new QLabel("");
    character_count->setObjectName("characterCount");
    source_tools->addWidget(copy_source_button);
    source_tools->addWidget(clear_source_button);
    source_tools->addStretch();
    source_tools->addWidget(character_count);
    source_layout->addLayout(source_tools);

    target_pane = new QFrame;
    target_pane->setObjectName("targetPane");
    auto *target_layout = new QVBoxLayout(target_pane);
    target_layout->setContentsMargins(dialog_scaled(10), dialog_scaled(10), dialog_scaled(10), dialog_scaled(8));
    target_layout->setSpacing(dialog_scaled(2));

    target_edit = new TextEdit;
    target_edit->setObjectName("targetEdit");
    target_edit->setAcceptRichText(false);
    target_edit->setReadOnly(true);
    target_edit->setPlaceholderText(tr_text("Translation will appear here..."));
    target_edit->setPlainText(translated_text);
    loading_dots_ = new EllipsisAnimator(this, [this](const QString &text) { target_edit->setPlainText(text); });
    target_layout->addWidget(target_edit, 1);

    auto *target_tools = new QHBoxLayout;
    target_tools->setSpacing(dialog_scaled(3));
    copy_target_button = tool_button("copy", tr_text("Copy Translation"));
    connect(copy_target_button, &QAbstractButton::clicked, this, &TranslationDialog::copy_target);
    target_tools->addWidget(copy_target_button);
    target_tools->addStretch();
    target_layout->addLayout(target_tools);

    panes->addWidget(source_pane, 1);
    panes->addWidget(target_pane, 1);
    root->addLayout(panes, 1);

    auto *footer = new QHBoxLayout;
    footer->setContentsMargins(dialog_scaled(2), dialog_scaled(13), dialog_scaled(2), 0);
    backend_badge = new QLabel(tr_text("Engine not configured"), this);
    backend_badge->setObjectName("backendBadge");
    backend_badge->setAlignment(Qt::AlignCenter);
    backend_badge->setFixedHeight(dialog_scaled(22));
    footer->addWidget(backend_badge);
    footer->addStretch();
    translate_button = new TranslateButton;
    connect(translate_button, &QAbstractButton::clicked, this, &TranslationDialog::request_translation);
    footer->addWidget(translate_button);
    root->addLayout(footer);

    update_count();
}

VectorToolButton *TranslationDialog::tool_button(const QString &icon_name, const QString &tooltip) {
    auto *button = new VectorToolButton(icon_name, tooltip);
    tool_buttons_.push_back(button);
    return button;
}

void TranslationDialog::set_combo_data(QComboBox *combo, const QString &value, const QString &fallback) {
    int index = combo->findData(value);
    if (index < 0) index = combo->findData(fallback);
    if (index >= 0) combo->setCurrentIndex(index);
}

QComboBox *TranslationDialog::language_combo(bool include_auto) {
    auto *combo = new QComboBox;
    combo->setObjectName("languageCombo");
    if (include_auto) combo->addItem(tr_text("Auto Detect"), "auto");
    for (const auto &[code, name] : translation_languages()) combo->addItem(name, code);
    combo->setFixedWidth(dialog_scaled(150));
    combo->setFixedHeight(dialog_scaled(36));
    return combo;
}

void TranslationDialog::swap_languages() {
    auto source_code = source_language->currentData().toString();
    const auto target_code = target_language->currentData().toString();
    if (source_code == "auto") source_code = detected_source_lang_;
    if (source_code.isEmpty()) return;

    const int source_index = source_language->findData(target_code);
    const int target_index = target_language->findData(source_code);
    if (source_index < 0 || target_index < 0) return;

    source_language->setCurrentIndex(source_index);
    target_language->setCurrentIndex(target_index);

    const auto source_text = source_edit->toPlainText();
    const auto target_text = target_edit->toPlainText();
    if (!target_text.isEmpty()) {
        source_edit->setPlainText(target_text);
        target_edit->setPlainText(source_text);
        set_target_state(false, false);
    }
}

void TranslationDialog::request_translation() {
    const auto text = source_edit->toPlainText().trimmed();
    if (text.isEmpty()) {
        source_edit->setFocus();
        return;
    }
    const auto source = source_language->currentData().toString();
    const auto target = target_language->currentData().toString();
    set_loading();
    emit translate_requested(text, source, target);
}

void TranslationDialog::update_count() {
    const auto count = source_edit->toPlainText().toUcs4().size();
    QLocale c_locale(QLocale::C);
    c_locale.setNumberOptions(QLocale::DefaultNumberOptions);
    const auto count_text = tr_text("%1 characters").replace("%1", c_locale.toString(static_cast<qlonglong>(count)));
    character_count->setText(count ? count_text : QString());
}

QString TranslationDialog::source_placeholder() {
    return tr_text("Enter or paste text...") + QString::fromUtf8("\n\n⌃ Enter ") + tr_text("Translate");
}

void TranslationDialog::retranslate_ui() {
    dashboard_title_bar->update_translations();
    update_pin_tooltip();
    swap_button->setToolTip(tr_text("Swap languages"));
    source_edit->setPlaceholderText(source_placeholder());
    target_edit->setPlaceholderText(tr_text("Translation will appear here..."));
    copy_source_button->set_tooltip(tr_text("Copy Original"));
    clear_source_button->set_tooltip(tr_text("Clear Original"));
    copy_target_button->set_tooltip(tr_text("Copy Translation"));
    source_language->setItemText(0, tr_text("Auto Detect"));
    translate_button->retranslate(!translate_button->isEnabled());
    update_count();
    if (!backend_configured_) backend_badge->setText(tr_text("Engine not configured"));
    if (target_edit->property("loading").toBool()) target_edit->setPlainText(tr_text("Translating..."));
    else if (target_edit->property("error").toBool() && last_error_message_)
        target_edit->setPlainText(tr_text("Translation failed:") + " " + *last_error_message_);
}

void TranslationDialog::on_target_lang_changed(int) {
    const auto lang = get_target_lang();
    if (!lang.isEmpty()) {
        get_tool_settings_manager()->set_app_setting("translation_target_lang", lang);
        target_lang = lang;
        log_debug("Target language saved: " + lang, "Translation");
    }
}

void TranslationDialog::on_toggle_pin(bool checked) {
    is_on_top_ = checked;
    get_tool_settings_manager()->set_translation_on_top(checked);
    update_pin_tooltip();
    set_window_topmost(static_cast<quintptr>(winId()), checked);
    log_debug(QString("Always on top: %1").arg(checked ? "True" : "False"), "Translation");
}

void TranslationDialog::showEvent(QShowEvent *event) {
    FramelessWindow::showEvent(event);
    // Topmost only changes the z-order, without WindowStaysOnTopHint (see
    // set_window_topmost); apply the current state on show.
    set_window_topmost(static_cast<quintptr>(winId()), is_on_top_);
}

void TranslationDialog::update_pin_tooltip() {
    dashboard_title_bar->pin_button->setToolTip(is_on_top_ ? tr_text("Always on top: ON") : tr_text("Always on top: OFF"));
}

void TranslationDialog::copy_source() {
    const auto text = source_edit->toPlainText();
    if (!text.isEmpty()) {
        QApplication::clipboard()->setText(text);
        log_info("Copied original text", "Translation");
    }
}

void TranslationDialog::copy_target() {
    const auto text = target_edit->toPlainText();
    if (!text.isEmpty()) {
        QApplication::clipboard()->setText(text);
        log_info("Copied translation", "Translation");
    }
}

void TranslationDialog::set_target_state(bool error, bool loading) {
    target_edit->setProperty("error", error);
    target_edit->setProperty("loading", loading);
    target_edit->style()->unpolish(target_edit);
    target_edit->style()->polish(target_edit);
}

void TranslationDialog::set_translation_result(const QString &text, const QString &detected_lang) {
    loading_dots_->stop();
    set_target_state(false, false);
    target_edit->setPlainText(text);
    translated_text = text;
    if (!detected_lang.isEmpty()) {
        detected_source_lang_ = detected_lang;
        set_combo_data(source_language, detected_lang, "auto");
    }
    set_busy(false);
}

void TranslationDialog::set_translation_error(const QString &error_msg) {
    loading_dots_->stop();
    last_error_message_ = error_msg;
    set_target_state(true, false);
    target_edit->setPlainText(tr_text("Translation failed:") + " " + error_msg);
    set_busy(false);
}

void TranslationDialog::set_loading() {
    set_target_state(false, true);
    target_edit->setPlainText(tr_text("Translating..."));
    loading_dots_->start();
    set_busy(true);
}

void TranslationDialog::set_busy(bool busy) {
    translate_button->setDisabled(busy);
    translate_button->retranslate(busy);
}

void TranslationDialog::on_translation_finished(bool success, const QString &text, const QString &error, const QString &detected_lang) {
    if (success) set_translation_result(text, detected_lang);
    else set_translation_error(error);
}

QString TranslationDialog::get_source_lang() const { return source_language->currentData().toString(); }
QString TranslationDialog::get_target_lang() const { return target_language->currentData().toString(); }

void TranslationDialog::update_content(const QString &text, const QString &new_target_lang, const QString &new_source_lang) {
    original_text = text;
    source_edit->setPlainText(text);
    if (!new_source_lang.isEmpty()) {
        source_lang = new_source_lang;
        set_combo_data(source_language, new_source_lang, "auto");
    }
    if (!new_target_lang.isEmpty()) set_combo_data(target_language, new_target_lang, get_target_lang());
}

void TranslationDialog::set_backend_badge(const QString &text, bool configured) {
    backend_configured_ = configured;
    backend_badge->setProperty("configured", configured);
    backend_badge->style()->unpolish(backend_badge);
    backend_badge->style()->polish(backend_badge);
    set_badge_text(backend_badge, text, 10);
}

void TranslationDialog::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(), Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    const double radius = isMaximized() ? 0.0 : dialog_scaled(WINDOW_CORNER_RADIUS);
    QRectF area(rect());
    if (!isMaximized()) area.adjust(0.5, 0.5, -0.5, -0.5);
    if (theme_name_ == "dark") {
        QLinearGradient background(0, 0, 0, std::min(dialog_scaled(190), height()));
        background.setColorAt(0.0, QColor("#4d4f56"));
        background.setColorAt(1.0, QColor("#41434a"));
        painter.setBrush(background);
    } else {
        painter.setBrush(QColor(palette_->window));
    }
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(area, radius, radius);
}

void TranslationDialog::set_theme(const QString &theme_name) {
    theme_name_ = theme_name == "light" ? "light" : "dark";
    palette_ = theme_name_ == "light" ? &dashboard_light() : &dashboard_dark();
    setStyleSheet(style_sheet(*palette_));
    dashboard_title_bar->apply_palette(*palette_);
    for (auto *button : tool_buttons_) button->apply_palette(*palette_);
    translate_button->apply_palette(*palette_);

    for (QTextEdit *edit : {static_cast<QTextEdit *>(source_edit), static_cast<QTextEdit *>(target_edit)}) {
        auto edit_palette = edit->palette();
        edit_palette.setColor(QPalette::PlaceholderText, QColor(palette_->text_3));
        edit->setPalette(edit_palette);
    }
    update();
}

QString TranslationDialog::style_sheet(const DashboardPalette &p) {
    const QString sheet = R"(
        QWidget { color: @text; font-size: @s13px; }
        QLabel#appLogo {
            color: white; background: @accent; border: none; border-radius: @s7px;
            font-size: @s9px; font-weight: 700;
        }
        QLabel#appName { font-size: @s13px; font-weight: 600; padding-left: @s3px; }
        QLabel#backendBadge {
            color: @accent; background: @accent_tint;
            border-radius: @s11px; padding: 0 @s10px; font-size: @s11px; font-weight: 600;
        }
        QLabel#backendBadge[configured="true"] { color: @green; }
        QComboBox#languageCombo {
            color: @text; background: @fill; border: none; border-radius: @s18px;
            padding: 0 @s14px;
        }
        QComboBox#languageCombo:hover { background: @fill_hover; }
        QComboBox#languageCombo::drop-down { border: none; width: @s24px; }
        QComboBox#languageCombo QAbstractItemView {
            color: @text; background: @surface_strong;
            border: @strokepx solid @fill_hover; border-radius: @control_radiuspx;
            padding: @s5px;
            outline: none; selection-color: white; selection-background-color: @accent;
        }
        QPushButton#swapButton {
            color: @accent; background: @accent_tint; border: none;
            border-radius: @s18px; font-size: @s19px; font-weight: 500;
        }
        QPushButton#swapButton:hover { background: @fill_hover; }
        QFrame#sourcePane, QFrame#targetPane {
            background: @field; border: @strokepx solid transparent;
            border-radius: @surface_radiuspx;
        }
        QFrame#sourcePane[focused="true"] { border-color: @accent; }
        QTextEdit#sourceEdit, QTextEdit#targetEdit {
            color: @text; background: transparent; border: none;
            padding: @s1px @s3px; font-size: @s15px;
            selection-color: white; selection-background-color: @accent;
        }
        QTextEdit#targetEdit[error="true"] { color: @danger; }
        QTextEdit#targetEdit[loading="true"] { color: @text_2; font-style: italic; }
        QLabel#characterCount { color: @text_3; font-size: @s11px; padding-right: @s5px; }
        QScrollBar:vertical { background: transparent; width: @s7px; margin: @s2px; }
        QScrollBar::handle:vertical {
            background: @fill; min-height: @s28px; border-radius: @s3px;
        }
        QScrollBar::handle:vertical:hover { background: @fill_hover; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
        )";
    return fill_dashboard_sheet(sheet, p);
}

TranslationLoadingDialog::TranslationLoadingDialog(const QString &original_text, QWidget *parent, std::optional<QPoint> position, const QString &source_lang,
                                                   const QString &target_lang)
    : TranslationDialog(original_text, {}, parent, position, source_lang, target_lang) {
    if (!original_text.trimmed().isEmpty()) set_loading();
}
}
