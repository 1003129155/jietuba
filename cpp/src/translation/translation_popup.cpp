#include "translation/translation_popup.h"
#include "core/i18n.h"
#include "core/ui_scale.h"
#include "translation/languages.h"
#include "translation/speech_service.h"
#include "translation/translation_dialog.h"
#include "translation/ui/widgets.h"
#include "ui/fluent_lite/inputs.h"
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QHBoxLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QStyle>
#include <QVBoxLayout>
#include <algorithm>

namespace jietuba {
namespace {
QString tr_text(const char *text) { return i18n_tr(text, "TranslationDialog"); }

QString language_name(const QString &code) {
    for (const auto &[entry_code, name] : translation_languages())
        if (entry_code == code) return name;
    return {};
}
}

TranslationPopup::TranslationPopup(QWidget *parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint), palette_(&dashboard_dark()) {
    setObjectName("translationPopup");
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setFont(QFont("Microsoft YaHei UI", dialog_scaled(10)));
    setFixedWidth(dialog_scaled(WIDTH));
    setMinimumHeight(dialog_scaled(MIN_HEIGHT));

    build_ui();
    set_theme("dark");

    loading_dots_ = new EllipsisAnimator(this, [this](const QString &text) { result_edit->setPlainText(text); });

    manual_debounce_ = new QTimer(this);
    manual_debounce_->setSingleShot(true);
    manual_debounce_->setInterval(450);
    connect(manual_debounce_, &QTimer::timeout, this, &TranslationPopup::request_manual_translation);

    fade_ = new QPropertyAnimation(this, "windowOpacity", this);
    fade_->setDuration(140);
    refresh_speech();
}

void TranslationPopup::build_ui() {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(dialog_scaled(12), dialog_scaled(10), dialog_scaled(12), dialog_scaled(11));
    root->setSpacing(dialog_scaled(8));

    auto *header = new QHBoxLayout;
    // Left margin 0 keeps the badge pill flush with the input box below it.
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(dialog_scaled(8));

    // No window title: the engine badge identifies the popup and the space next
    // to it works as the drag handle.
    backend_badge = new QLabel(tr_text("Engine not configured"), this);
    backend_badge->setObjectName("popupBadge");
    backend_badge->setFixedHeight(dialog_scaled(21));
    header->addWidget(backend_badge);
    header->addStretch(1);

    close_button = new QPushButton(QString::fromUtf8("×"), this);
    close_button->setObjectName("popupClose");
    close_button->setCursor(Qt::PointingHandCursor);
    close_button->setFixedSize(dialog_scaled(28), dialog_scaled(26));
    close_button->setAccessibleName(tr_text("Close"));
    connect(close_button, &QPushButton::clicked, this, &TranslationPopup::hide_popup);
    header->addWidget(close_button);
    root->addLayout(header);

    source_edit = make_text_view("popupSource", true);
    source_edit->setMaximumHeight(dialog_scaled(SOURCE_MAX_HEIGHT));
    source_edit->setPlaceholderText(tr_text("Enter text to translate..."));
    connect(source_edit, &QTextEdit::textChanged, this, &TranslationPopup::on_source_changed);
    speak_source_button = new VectorToolButton("speak", tr_text("Read Aloud"), this);
    source_speech_ = new SpeechButton(speak_source_button, [this] { return source_edit->toPlainText().trimmed(); }, [this] { return source_text_lang(); });
    // Editing the text being read stops it, whether typed or filled in by a new selection.
    connect(source_edit, &QTextEdit::textChanged, source_speech_, [this] {
        source_speech_->stop();
        source_speech_->refresh();
    });
    auto *source_row = new QHBoxLayout;
    source_row->setSpacing(dialog_scaled(4));
    source_row->addWidget(source_edit, 1);
    source_row->addWidget(speak_source_button, 0, Qt::AlignTop);
    root->addLayout(source_row);

    divider = new QFrame(this);
    divider->setObjectName("popupDivider");
    divider->setFixedHeight(dialog_scaled(1));
    root->addWidget(divider);

    result_edit = make_text_view("popupResult");
    result_edit->setMaximumHeight(dialog_scaled(RESULT_MAX_HEIGHT));
    speak_result_button = new VectorToolButton("speak", tr_text("Read Aloud"), this);
    result_speech_ = new SpeechButton(speak_result_button, [this] { return speakable_result(); }, [this] { return result_lang_; });
    auto *result_row = new QHBoxLayout;
    result_row->setSpacing(dialog_scaled(4));
    result_row->addWidget(result_edit, 1);
    result_row->addWidget(speak_result_button, 0, Qt::AlignTop);
    root->addLayout(result_row);

    auto *footer = new QHBoxLayout;
    footer->setContentsMargins(0, dialog_scaled(1), 0, 0);
    footer->setSpacing(dialog_scaled(7));

    auto *lang_label = new QLabel(tr_text("Target") + ": ", this);
    lang_label->setObjectName("popupLangLabel");
    footer->addWidget(lang_label);

    lang_button = new QPushButton(QString::fromUtf8("中文"), this);
    lang_button->setObjectName("popupLangButton");
    lang_button->setCursor(Qt::PointingHandCursor);
    lang_button->setToolTip(tr_text("Switch target language"));
    connect(lang_button, &QPushButton::clicked, this, &TranslationPopup::show_lang_menu);
    footer->addWidget(lang_button);

    footer->addStretch(1);

    copy_button = new QPushButton(tr_text("Copy Translation"), this);
    copy_button->setObjectName("popupChip");
    copy_button->setCursor(Qt::PointingHandCursor);
    connect(copy_button, &QPushButton::clicked, this, &TranslationPopup::copy_translation);
    footer->addWidget(copy_button);

    full_button = new QPushButton(tr_text("Open full window"), this);
    full_button->setObjectName("popupPrimary");
    full_button->setCursor(Qt::PointingHandCursor);
    connect(full_button, &QPushButton::clicked, this, &TranslationPopup::open_full);
    footer->addWidget(full_button);
    root->addLayout(footer);
}

TextEdit *TranslationPopup::make_text_view(const QString &object_name, bool editable) {
    auto *view = new TextEdit(this);
    view->setObjectName(object_name);
    view->setReadOnly(!editable);
    view->setAcceptRichText(false);
    view->setFrameShape(QFrame::NoFrame);
    view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    view->setTextInteractionFlags(editable ? Qt::TextEditorInteraction : Qt::TextSelectableByMouse);
    return view;
}

void TranslationPopup::show_lang_menu() {
    QMenu menu(this);
    menu.setObjectName("popupLangMenu");
    for (const auto &[code, name] : translation_languages()) {
        auto *action = menu.addAction(name);
        action->setData(code);
        if (code == target_lang_) {
            action->setCheckable(true);
            action->setChecked(true);
        }
    }
    // Pops up below the button.
    const auto pos = lang_button->mapToGlobal(lang_button->rect().bottomLeft());
    auto *action = menu.exec(pos);
    if (!action) return;
    const auto new_lang = action->data().toString();
    if (new_lang != target_lang_) {
        target_lang_ = new_lang;
        lang_button->setText(language_name(new_lang));
        emit target_lang_changed(new_lang);
        // Retranslate into the new language when there is source text.
        if (!source_text_.isEmpty()) {
            enter_loading();
            emit manual_translate_requested(source_text_);
        }
    }
}

void TranslationPopup::set_target_lang(const QString &lang_code) {
    const auto name = language_name(lang_code);
    if (!name.isEmpty()) {
        target_lang_ = lang_code;
        lang_button->setText(name);
    }
}

void TranslationPopup::show_popup(const QString &source_text, std::optional<QPoint> position, bool activate) {
    loading_dots_->stop();
    manual_debounce_->stop();
    source_text_ = source_text.trimmed();
    translated_text_.clear();
    error_text_.clear();
    setAttribute(Qt::WA_ShowWithoutActivating, !activate);
    set_source_text(source_text_);
    // The caller starts the request right after showing the popup.
    if (!source_text_.isEmpty()) enter_loading();
    else hide_result();
    fit_content();
    place_near(position.value_or(QCursor::pos()));
    show_animated();
    if (activate) {
        activateWindow();
        QTimer::singleShot(0, this, &TranslationPopup::focus_source_input);
    }
}

void TranslationPopup::set_source_text(const QString &text) {
    suppress_auto_ = true;
    source_edit->setPlainText(text);
    suppress_auto_ = false;
}

void TranslationPopup::enter_loading() {
    divider->show();
    result_edit->show();
    copy_button->show();
    copy_button->setEnabled(false);
    result_edit->setProperty("error", false);
    result_edit->setPlainText(tr_text("Translating..."));
    refresh_result_style();
    loading_dots_->start();
    result_speech_->stop();
    refresh_speech();
}

void TranslationPopup::hide_result() {
    loading_dots_->stop();
    result_edit->clear();
    result_edit->setProperty("error", false);
    refresh_result_style();
    divider->hide();
    result_edit->hide();
    copy_button->hide();
    result_speech_->stop();
    refresh_speech();
}

void TranslationPopup::focus_source_input() {
    if (!isVisible()) return;
    source_edit->setFocus();
}

void TranslationPopup::show_result(const QString &translated_text, const QString &detected_lang) {
    loading_dots_->stop();
    result_speech_->stop();
    translated_text_ = translated_text;
    result_lang_ = target_lang_;
    if (!detected_lang.isEmpty()) {
        detected_lang_ = detected_lang;
        detected_for_text_ = source_edit->toPlainText();
    }
    error_text_ = translated_text.isEmpty() ? tr_text("No translation result") : QString();
    result_edit->setProperty("error", false);
    result_edit->setPlainText(translated_text.isEmpty() ? tr_text("No translation result") : translated_text);
    divider->show();
    result_edit->show();
    copy_button->show();
    copy_button->setEnabled(!translated_text.isEmpty());
    refresh_result_style();
    refresh_speech();
    fit_content();
}

void TranslationPopup::show_error(const QString &message) {
    loading_dots_->stop();
    result_speech_->stop();
    translated_text_.clear();
    error_text_ = message;
    result_edit->setProperty("error", true);
    result_edit->setPlainText(message);
    divider->show();
    result_edit->show();
    copy_button->show();
    copy_button->setEnabled(false);
    refresh_result_style();
    refresh_speech();
    fit_content();
}

quint64 TranslationPopup::source_speech_token() const { return source_speech_->token(); }

quint64 TranslationPopup::result_speech_token() const { return result_speech_->token(); }

QString TranslationPopup::source_text_lang() const {
    const auto text = source_edit->toPlainText();
    if (!detected_lang_.isEmpty() && text == detected_for_text_) return detected_lang_;
    return guess_text_language(text);
}

QString TranslationPopup::speakable_result() const {
    if (result_edit->isHidden() || result_edit->property("error").toBool() || loading_dots_->is_running()) return {};
    return translated_text_.trimmed();
}

void TranslationPopup::refresh_speech() {
    speak_result_button->setVisible(!result_edit->isHidden());
    source_speech_->refresh();
    result_speech_->refresh();
}

void TranslationPopup::set_backend_status(const QString &name, bool ready) {
    backend_ready_ = ready;
    backend_badge->setProperty("ready", ready);
    backend_badge->style()->unpolish(backend_badge);
    backend_badge->style()->polish(backend_badge);
    set_badge_text(backend_badge, ready ? name : tr_text("Engine not configured"), 8);
}

void TranslationPopup::set_theme(const QString &theme_name) {
    palette_ = theme_name == "light" ? &dashboard_light() : &dashboard_dark();
    for (auto *button : {speak_source_button, speak_result_button}) button->apply_palette(*palette_);
    setStyleSheet(fill_dashboard_sheet(R"(
            QWidget#translationPopup { background: transparent; color: @text; }
            QLabel#popupBadge {
                color: @green; background: @accent_tint; border-radius: @s7px;
                padding: 0 @s8px; font-size: @s11px; font-weight: 600;
            }
            QLabel#popupBadge[ready="false"] { color: @text_2; background: @fill; }
            QPushButton#popupClose {
                color: @text_2; background: transparent; border: none;
                border-radius: @control_radiuspx; font-size: @s19px;
            }
            QPushButton#popupClose:hover { color: white; background: @danger; }
            QTextEdit#popupSource {
                color: @text; background: @field;
                border: @strokepx solid @fill_hover;
                border-radius: @control_radiuspx;
                padding: @s7px @s9px; font-size: @s14px;
            }
            QTextEdit#popupSource:focus { border-color: @accent; }
            QTextEdit#popupResult {
                color: @text; background: transparent; border: none;
                padding: @s2px @s3px; font-size: @s14px; font-weight: 550;
            }
            QTextEdit#popupResult[error="true"] { color: @danger; font-weight: 500; }
            QFrame#popupDivider { background: @fill_hover; border: none; }
            QLabel#popupLangLabel {
                color: @text_3; font-size: @s11px; padding-left: @s3px;
            }
            QPushButton#popupLangButton {
                color: @text_2; background: transparent; border: none;
                border-radius: @control_radiuspx; padding: @s4px @s8px; font-size: @s12px; font-weight: 600;
                text-align: left;
            }
            QPushButton#popupLangButton:hover {
                color: @text; background: @fill;
            }
            QPushButton#popupChip, QPushButton#popupPrimary {
                border: none; border-radius: @control_radiuspx; padding: @s6px @s10px;
                background: transparent; font-size: @s12px; font-weight: 600;
            }
            QPushButton#popupChip { color: @text_2; }
            QPushButton#popupChip:hover { color: @text; background: @fill; }
            QPushButton#popupChip:disabled { color: @text_3; background: transparent; }
            QPushButton#popupPrimary { color: @accent; }
            QPushButton#popupPrimary:hover { background: @accent_tint; }
            QScrollBar:vertical { background: transparent; width: @s5px; margin: @s2px 0; }
            QScrollBar::handle:vertical { background: @fill_hover; border-radius: @s2px; min-height: @s20px; }
            QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
            QMenu#popupLangMenu {
                background: @surface_strong; border: @strokepx solid @fill_hover;
                border-radius: @control_radiuspx; padding: @s5px;
            }
            QMenu#popupLangMenu::item {
                color: @text; background: transparent; padding: @s6px @s12px;
                border-radius: @control_radiuspx; font-size: @s12px;
            }
            QMenu#popupLangMenu::item:selected { background: @accent; color: white; }
            QMenu#popupLangMenu::item:checked { font-weight: 600; }
            )",
                                       *palette_));
    update();
}

void TranslationPopup::on_source_changed() {
    if (suppress_auto_) return;
    source_text_ = source_edit->toPlainText().trimmed();
    translated_text_.clear();
    error_text_.clear();
    emit manual_input_changed();
    manual_debounce_->stop();
    if (source_text_.isEmpty()) {
        hide_result();
        fit_content();
        return;
    }
    if (!backend_ready_) {
        show_error(tr_text("API key not configured"));
        return;
    }
    manual_debounce_->start();
    fit_content();
}

void TranslationPopup::request_manual_translation() {
    const auto text = source_edit->toPlainText().trimmed();
    if (text.isEmpty()) return;
    source_text_ = text;
    translated_text_.clear();
    error_text_.clear();
    enter_loading();
    fit_content();
    emit manual_translate_requested(text);
}

void TranslationPopup::refresh_result_style() {
    result_edit->style()->unpolish(result_edit);
    result_edit->style()->polish(result_edit);
}

void TranslationPopup::fit_text_view(TextEdit *view, int minimum, int maximum) {
    const int doc_height = static_cast<int>(view->document()->size().height()) + dialog_scaled(10);
    view->setFixedHeight(std::max(minimum, std::min(doc_height, maximum)));
}

void TranslationPopup::fit_content() {
    fit_text_view(source_edit, dialog_scaled(SOURCE_MIN_HEIGHT), dialog_scaled(SOURCE_MAX_HEIGHT));
    // show_popup sizes the window before showing it, when isVisible() is always
    // false, so only the widget's own hidden flag counts.
    if (!result_edit->isHidden()) fit_text_view(result_edit, dialog_scaled(RESULT_MIN_HEIGHT), dialog_scaled(RESULT_MAX_HEIGHT));
    layout()->activate();
    const int desired = sizeHint().height();
    setFixedHeight(std::max(dialog_scaled(MIN_HEIGHT), std::min(desired, dialog_scaled(MAX_HEIGHT))));
}

void TranslationPopup::place_near(const QPoint &position) {
    auto *screen = QApplication::screenAt(position);
    if (!screen) screen = QApplication::primaryScreen();
    if (!screen) {
        move(position);
        return;
    }
    const auto area = screen->availableGeometry();
    int x = position.x() + dialog_scaled(14);
    int y = position.y() + dialog_scaled(22);
    if (x + width() > area.right() - dialog_scaled(8)) x = area.right() - width() - dialog_scaled(8);
    if (y + height() > area.bottom() - dialog_scaled(8)) y = position.y() - height() - dialog_scaled(16);
    x = std::max(area.left() + dialog_scaled(8), x);
    y = std::max(area.top() + dialog_scaled(8), y);
    move(x, y);
}

void TranslationPopup::show_animated() {
    setWindowOpacity(0.0);
    show();
    raise();
    fade_->stop();
    fade_->setStartValue(0.0);
    fade_->setEndValue(1.0);
    fade_->start();
}

void TranslationPopup::copy_translation() {
    if (translated_text_.isEmpty()) return;
    QApplication::clipboard()->setText(translated_text_);
    const auto old_text = copy_button->text();
    copy_button->setText(tr_text("Copied"));
    QTimer::singleShot(900, copy_button, [button = copy_button, old_text] { button->setText(old_text); });
}

void TranslationPopup::open_full() {
    manual_debounce_->stop();
    // Always read the box: the user may have edited the text since it loaded.
    source_text_ = source_edit->toPlainText().trimmed();
    emit open_full_requested(source_text_, translated_text_, error_text_);
}

void TranslationPopup::hide_popup() {
    manual_debounce_->stop();
    loading_dots_->stop();
    hide();
}

void TranslationPopup::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(), Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    const int stroke_width = std::max(1, dialog_scaled(1));
    const double inset = stroke_width / 2.0;
    const auto area = QRectF(rect()).adjusted(inset, inset, -inset, -inset);
    painter.setPen(QPen(palette_->fill_hover_color, stroke_width));
    painter.setBrush(QColor(palette_->window));
    const int radius = dialog_scaled(WINDOW_CORNER_RADIUS);
    painter.drawRoundedRect(area, radius, radius);
}

void TranslationPopup::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && event->position().y() <= dialog_scaled(42)) {
        drag_offset_ = event->globalPosition().toPoint() - frameGeometry().topLeft();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void TranslationPopup::mouseMoveEvent(QMouseEvent *event) {
    if (drag_offset_ && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPosition().toPoint() - *drag_offset_);
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void TranslationPopup::mouseReleaseEvent(QMouseEvent *event) {
    drag_offset_.reset();
    QWidget::mouseReleaseEvent(event);
}

void TranslationPopup::hideEvent(QHideEvent *event) {
    source_speech_->stop();
    result_speech_->stop();
    QWidget::hideEvent(event);
}
}
