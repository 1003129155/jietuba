#include "core/i18n.h"
#include "core/logger.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "settings/tool_settings.h"
#include "translation/languages.h"
#include "translation/registry.h"
#include "translation/service.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/theme.h"
#include "ui/settings_ui/pages.h"
#include "ui/welcome/pages.h"
#include <QFormLayout>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <algorithm>

namespace jietuba {
namespace {
constexpr int FORM_LABEL_WIDTH = 96;
constexpr int FORM_COLUMN_GAP = 12;

QString page_title() { return welcome_tr("🌐 翻译设置").remove(QString::fromUtf8("🌐")).trimmed(); }
QString page_subtitle() { return welcome_tr("选择翻译引擎并填写对应凭据，之后可以随时在设置中修改。"); }
}

TranslationPage::TranslationPage(ToolSettingsManager *config, QWidget *parent) : BasePage(page_title(), page_subtitle(), parent), config_(config) {
    init();
    // The form gets the full body width.
    illus_area->hide();
}

// The page is all form; the illustration slot stays empty and hidden.
IllustrationArea *TranslationPage::create_illustration() { return new IllustrationArea(this); }

std::pair<QWidget *, QLabel *> TranslationPage::config_row(const char *text, QWidget *control) {
    auto *row_widget = new QWidget;
    set_own_style(row_widget, "background: transparent;");
    auto *row = new QHBoxLayout(row_widget);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(dialog_scaled(FORM_COLUMN_GAP));
    auto *label = new QLabel(welcome_tr(text), row_widget);
    set_welcome_label_style(label, "primary", 14, 600);
    label->setFixedWidth(dialog_scaled(FORM_LABEL_WIDTH));
    control->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    row->addWidget(label);
    row->addWidget(control, 1);
    return {row_widget, label};
}

void TranslationPage::build_controls(QVBoxLayout *layout) {
    auto *settings_card = new QWidget;
    settings_card->setObjectName("SettingRow");
    settings_card->setProperty("welcomeSettingRow", true);
    auto *card_layout = new QVBoxLayout(settings_card);
    card_layout->setContentsMargins(dialog_scaled(22), dialog_scaled(18), dialog_scaled(22), dialog_scaled(18));
    card_layout->setSpacing(dialog_scaled(14));
    layout->addWidget(settings_card);

    provider_combo_ = new ComboBox;
    provider_combo_->setMinimumWidth(dialog_scaled(280));
    provider_combo_->setCursor(Qt::PointingHandCursor);
    // The combo and the credential forms come from the same metadata, so a new provider never
    // shows in one without the other.
    static const QMap<QString, int> provider_order{{"google", 0}, {"deepl", 1}, {"azure", 2}, {"amazon", 3}};
    registry_ = create_default_translation_service()->registry;
    provider_metadata_ = registry_->available_providers();
    std::stable_sort(provider_metadata_.begin(), provider_metadata_.end(),
                     [](const ProviderMetadata &a, const ProviderMetadata &b) { return provider_order.value(a.provider_id, 99) < provider_order.value(b.provider_id, 99); });
    for (const auto &metadata : provider_metadata_) provider_combo_->addItem(welcome_tr(metadata.display_name.toUtf8().constData()), metadata.provider_id);
    const QString saved_provider = config_->get_translation_provider();
    const int provider_index = provider_combo_->findData(saved_provider.isEmpty() ? QString("google") : saved_provider);
    provider_combo_->setCurrentIndex(provider_index >= 0 ? provider_index : 0);
    QWidget *provider_row = nullptr;
    std::tie(provider_row, row_provider_lbl_) = config_row("翻译引擎", provider_combo_);
    card_layout->addWidget(provider_row);

    credential_stack_ = new QStackedWidget;
    set_own_style(credential_stack_, "background: transparent;");
    for (const auto &metadata : provider_metadata_) build_credential_page(metadata);
    card_layout->addWidget(credential_stack_);
    connect(provider_combo_, &QComboBox::currentIndexChanged, this, &TranslationPage::on_provider_changed);
    on_provider_changed();

    // Target language.
    lang_combo_ = new ComboBox;
    lang_combo_->setMinimumWidth(dialog_scaled(240));
    lang_combo_->setCursor(Qt::PointingHandCursor);
    populate_lang_combo();
    QWidget *row = nullptr;
    std::tie(row, row_lang_lbl_) = config_row("翻译目标语言", lang_combo_);
    card_layout->addWidget(row);
}

void TranslationPage::retranslate() {
    title_label->setText(page_title());
    subtitle_label->setText(page_subtitle());
    if (row_provider_lbl_) row_provider_lbl_->setText(welcome_tr("翻译引擎"));
    if (row_lang_lbl_) row_lang_lbl_->setText(welcome_tr("翻译目标语言"));
    for (const auto &[label, text] : credential_labels_) label->setText(welcome_tr(text.toUtf8().constData()));
    for (int index = 0; index < static_cast<int>(provider_metadata_.size()); ++index) {
        const auto &metadata = provider_metadata_[index];
        // Brand names have no translation entry and come back unchanged; only names such as "Custom" change.
        provider_combo_->setItemText(index, welcome_tr(metadata.display_name.toUtf8().constData()));
        for (const auto &field : metadata.credentials)
            if (auto *edit = qobject_cast<QLineEdit *>(credential_edits_.value(field.config_key).data()); edit && !field.placeholder.isEmpty())
                edit->setPlaceholderText(welcome_tr(field.placeholder.toUtf8().constData()));
    }
    illus_area->retranslate();
}

void TranslationPage::populate_lang_combo() {
    static const QMap<QString, QString> default_map{{"zh", "ZH"}, {"en", "EN"}, {"ja", "JA"}};
    QString saved = config_->get_app_setting("translation_target_lang", "").toString();
    if (saved.isEmpty()) saved = default_map.value(I18nManager::get_current_language(), "ZH");
    for (const auto &[code, name] : translation_languages()) lang_combo_->addItem(QString("%1 (%2)").arg(name, code), code);
    if (const int idx = lang_combo_->findData(saved); idx >= 0) lang_combo_->setCurrentIndex(idx);
}

void TranslationPage::build_credential_page(const ProviderMetadata &metadata) {
    auto *page = new QWidget;
    set_own_style(page, "background: transparent;");
    auto *form = new QFormLayout(page);
    form->setContentsMargins(0, dialog_scaled(2), 0, dialog_scaled(2));
    form->setHorizontalSpacing(dialog_scaled(FORM_COLUMN_GAP));
    form->setVerticalSpacing(dialog_scaled(7));
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    for (const auto &field : metadata.credentials) {
        auto *edit = new LineEdit;
        edit->setText(provider_fields::read_config(config_, field).toString());
        edit->setPlaceholderText(!field.placeholder.isEmpty() ? welcome_tr(field.placeholder.toUtf8().constData()) : QString());
        if (field.secret) edit->setEchoMode(QLineEdit::Password);
        credential_edits_[field.config_key] = edit;
        auto *label = new QLabel(welcome_tr(field.label.toUtf8().constData()));
        set_welcome_label_style(label, "primary", 12, 600);
        label->setFixedWidth(dialog_scaled(FORM_LABEL_WIDTH));
        credential_labels_.emplace_back(label, field.label);
        form->addRow(label, edit);
    }
    if (!metadata.help_url.isEmpty()) {
        auto *hint = new QLabel(QString("<a href=\"%1\" style=\"color:%2;\">%3</a>").arg(metadata.help_url, ACCENT, welcome_tr(metadata.help_label.toUtf8().constData())));
        hint->setOpenExternalLinks(true);
        set_welcome_label_style(hint, "muted", 11, 400);
        form->addRow("", hint);
    }
    provider_pages_[metadata.provider_id] = page;
    credential_stack_->addWidget(page);
}

void TranslationPage::on_provider_changed() {
    const QString provider_id = provider_combo_->currentData().toString();
    QWidget *page = provider_pages_.value(provider_id);
    if (!page) {
        // A provider without a credential page: staying on the previous page would show another
        // provider's credentials as its own, so the stack is emptied and the provider logged.
        log_error(T("翻译引擎 {provider_id} 没有凭据页", {{"provider_id", provider_id}}), "Welcome");
        credential_stack_->setCurrentIndex(-1);
        credential_stack_->setFixedHeight(0);
        return;
    }
    credential_stack_->setCurrentWidget(page);
    // Fluent inputs have 26 px of content plus padding and borders; a small allowance keeps the
    // last row's focus border from being clipped at fractional DPI scales.
    credential_stack_->setFixedHeight(std::max(dialog_scaled(68), page->sizeHint().height() + dialog_scaled(12)));
}

void TranslationPage::save() {
    const QString provider_id = provider_combo_->currentData().toString();
    config_->set_translation_provider(provider_id.isEmpty() ? QString("google") : provider_id);
    // The same reads and writes as the settings page.
    provider_fields::save_from(config_, provider_fields::all_fields(*registry_), credential_edits_);
    const QString lang = lang_combo_->currentData().toString();
    if (!lang.isEmpty()) config_->set_app_setting("translation_target_lang", lang);
}
}
