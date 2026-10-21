#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "settings/tool_settings.h"
#include "translation/languages.h"
#include "translation/registry.h"
#include "translation/service.h"
#include "translation/worker.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/labels.h"
#include "ui/fluent_lite/switch.h"
#include "ui/fluent_lite/text_context_menu.h"
#include "ui/settings_ui/components.h"
#include "ui/settings_ui/dialog.h"
#include "ui/settings_ui/pages.h"
#include <QFrame>
#include <QLocale>
#include <QMenu>
#include <QRegularExpression>
#include <QThread>
#include <QToolTip>
#include <set>

// The translation settings page. No provider is named here: each provider's credentials,
// own switches, hints, help link and which general options it uses all come from its own
// declaration (ProviderField / ProviderMetadata); the page only renders them, so a provider
// cannot be missing from either the form or the save.
namespace jietuba {
namespace {
// Label column width. Providers differ in field count and label length; a fixed width
// keeps inputs from jumping sideways when switching providers.
constexpr int LABEL_WIDTH = 135;

// A set of rows shown with its provider. Like SettingCardGroup::addSettingCard it puts
// 1 px separators between rows, but has no title or frame: the one enclosing group draws
// those, so the page stays a single block. With lead_each every card gets its own leading
// separator, hidden together with the card by set_card_visible so no orphan line stays;
// such a section joins its group without the group's separator.
class ProviderSection : public QWidget {
public:
    explicit ProviderSection(QWidget *parent = nullptr, bool lead_each = false) : QWidget(parent), lead_each_(lead_each) {
        v_ = new QVBoxLayout(this);
        v_->setContentsMargins(0, 0, 0, 0);
        v_->setSpacing(0);
        QObject::connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { apply_separator_theme(); });
    }
    void addSettingCard(QWidget *card) {
        QFrame *separator = lead_each_ || !cards_.isEmpty() ? add_separator() : nullptr;
        card->setParent(this);
        v_->addWidget(card);
        cards_.append(card);
        if (lead_each_) card->setProperty("_leading_separator", QVariant::fromValue(static_cast<QObject *>(separator)));
        apply_separator_theme();
    }
private:
    QFrame *add_separator() {
        auto *separator = new QFrame(this);
        separator->setFixedHeight(dialog_scaled(1));
        separators_.append(separator);
        v_->addWidget(separator);
        return separator;
    }
    void apply_separator_theme() {
        const QString color = ui_tokens(this).separator;
        for (auto *separator : separators_) set_own_style(separator, QString("background: %1; border: none; margin-left: %2px;").arg(color).arg(card_text_left()));
    }
    QVBoxLayout *v_;
    bool lead_each_;
    QList<QWidget *> cards_;
    QList<QFrame *> separators_;
};

// Request threads are not children of the settings window: the window may close while a
// thread still waits on the network, and a destroyed parent would take the running QThread with it.
void keep_until_finished(QThread *thread) {
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

class ModelListThread : public QThread {
    Q_OBJECT
public:
    explicit ModelListThread(std::unique_ptr<TranslationProvider> provider) : provider_(std::move(provider)) {}
signals:
    // (success, models / error message).
    void finished_signal(bool ok, const QStringList &models, const QString &error);
protected:
    void run() override {
        try {
            emit finished_signal(true, provider_->list_models(), {});
        } catch (const std::exception &exc) {
            emit finished_signal(false, {}, QString::fromUtf8(exc.what()));
        }
    }
private:
    std::unique_ptr<TranslationProvider> provider_;
};

TranslationService pending_service(SettingsDialog *dialog, const QString &provider_id) {
    const auto fields = provider_fields::provider_fields(*dialog->translation_registry, provider_id);
    return TranslationService(dialog->translation_registry, provider_fields::pending_config(dialog->config_manager, fields, *dialog->provider_field_widgets));
}

// An empty text shows what the row is for, rather than a blank.
void show_status(SettingsDialog *dialog, const QString &provider_id, const QString &text) {
    if (auto *label = dialog->provider_status_labels.value(provider_id).data())
        label->setText(!text.isEmpty() ? text : dialog->tr_text("Click the button on the right to test the translation API"));
}

void test_connection(SettingsDialog *dialog, const QString &provider_id, QPushButton *button) {
    QString target;
    if (auto *combo = dialog->attr<QComboBox>("translation_target_combo")) target = combo->currentData().toString();
    const TranslationRequest request("Hello, world!", !target.isEmpty() ? target : "ZH");
    auto *worker = new TranslationWorker(pending_service(dialog, provider_id), request, provider_id);
    button->setEnabled(false);
    show_status(dialog, provider_id, dialog->tr_text("Testing..."));
    QPointer<QPushButton> guarded_button(button);
    QPointer<SettingsDialog> guarded_dialog(dialog);
    QObject::connect(worker, &TranslationWorker::finished_signal, worker, [guarded_button, guarded_dialog, provider_id](const TranslationResult &result) {
        if (!guarded_button || !guarded_dialog) return;
        guarded_button->setEnabled(true);
        show_status(guarded_dialog, provider_id, result.success ? QString::fromUtf8("✓ ") + result.translated_text : QString::fromUtf8("✗ ") + result.error_message);
    });
    keep_until_finished(worker);
}

// The status row sits at the bottom of the provider's form, several rows away from the
// model row, so a failure is repeated next to the button. No owning widget is passed:
// the button's style sheet would then style the tooltip with dark text on black.
void show_near(QPushButton *button, const QString &text) { QToolTip::showText(button->mapToGlobal(QPoint(0, button->height())), text, nullptr, QRect(), 8000); }

void fetch_models(SettingsDialog *dialog, const QString &provider_id, QLineEdit *edit, QPushButton *button) {
    std::unique_ptr<TranslationProvider> provider;
    try {
        provider = pending_service(dialog, provider_id).provider(provider_id);
    } catch (const std::invalid_argument &exc) {
        show_status(dialog, provider_id, QString::fromUtf8("✗ ") + QString::fromUtf8(exc.what()));
        return;
    }
    auto *thread = new ModelListThread(std::move(provider));
    button->setEnabled(false);
    show_status(dialog, provider_id, dialog->tr_text("Fetching models..."));
    QPointer<QPushButton> guarded_button(button);
    QPointer<QLineEdit> guarded_edit(edit);
    QPointer<SettingsDialog> guarded_dialog(dialog);
    QObject::connect(thread, &ModelListThread::finished_signal, thread,
                     [guarded_button, guarded_edit, guarded_dialog, provider_id](bool ok, const QStringList &models, const QString &error) {
                         if (!guarded_button || !guarded_dialog) return;
                         guarded_button->setEnabled(true);
                         if (!ok || models.isEmpty()) {
                             const QString text = !ok ? QString::fromUtf8("✗ ") + error : guarded_dialog->tr_text("No models returned");
                             show_status(guarded_dialog, provider_id, text);
                             show_near(guarded_button, text);
                             return;
                         }
                         show_status(guarded_dialog, provider_id, "");
                         if (!guarded_edit) return;
                         QMenu menu(guarded_edit);
                         style_popup_menu(&menu, ui_tokens(guarded_edit));
                         for (const auto &model : models) {
                             QLineEdit *target = guarded_edit;
                             QObject::connect(menu.addAction(model), &QAction::triggered, target, [target, model] { target->setText(model); });
                         }
                         menu.exec(guarded_edit->mapToGlobal(QPoint(0, guarded_edit->height())));
                     });
    keep_until_finished(thread);
}

void toggle_secret(SettingsDialog *dialog, QLineEdit *edit, QPushButton *button) {
    if (edit->echoMode() == QLineEdit::Password) {
        edit->setEchoMode(QLineEdit::Normal);
        button->setText(dialog->tr_text("Hide"));
    } else {
        edit->setEchoMode(QLineEdit::Password);
        button->setText(dialog->tr_text("Show"));
    }
    adjust_button_width(button, 60);
}

// A provider's own switch. Like the credential rows: no icon, text aligned with the title column, so it reads as part of the provider above.
QWidget *build_toggle_row(SettingsDialog *dialog, ProviderSection *section, const ProviderField &field) {
    auto *card = new WhiteCard(section);
    auto *row = new QHBoxLayout(card);
    row->setContentsMargins(card_text_left(), dialog_scaled(10), card_right_margin(), dialog_scaled(10));
    row->setSpacing(dialog_scaled(10));
    auto *text = new QVBoxLayout;
    text->setSpacing(dialog_scaled(2));
    auto *title = new QLabel(dialog->tr_text(field.label.toUtf8().constData()), card);
    apply_theme_text_style(title, 13);
    text->addWidget(title);
    if (!field.description.isEmpty()) {
        auto *description = new QLabel(dialog->tr_text(field.description.toUtf8().constData()), card);
        description->setWordWrap(true);
        apply_theme_text_style(description, 12, false, {}, true);
        text->addWidget(description);
    }
    row->addLayout(text, 1);
    auto *switch_button = new SwitchButton(card);
    switch_button->setChecked(provider_fields::read_config(dialog->config_manager, field).toBool());
    row->addWidget(switch_button, 0, Qt::AlignVCenter);
    card->setMinimumHeight(dialog_scaled(58));
    section->addSettingCard(card);
    return switch_button;
}

// One control row per field, by the field's kind.
QWidget *build_field(SettingsDialog *dialog, ProviderSection *section, const ProviderField &field, const QString &provider_id) {
    if (field.kind == ProviderField::Toggle) return build_toggle_row(dialog, section, field);
    auto *card = new WhiteCard(section);
    auto *row = new QHBoxLayout(card);
    row->setContentsMargins(card_text_left(), dialog_scaled(12), card_right_margin(), dialog_scaled(12));
    row->setSpacing(dialog_scaled(10));
    auto *title = new QLabel(dialog->tr_text(field.label.toUtf8().constData()), card);
    apply_theme_text_style(title, 13);
    title->setFixedWidth(dialog_scaled(LABEL_WIDTH));
    row->addWidget(title);
    auto *edit = new LineEdit(card);
    edit->setText(provider_fields::read_config(dialog->config_manager, field).toString());
    if (!field.placeholder.isEmpty()) edit->setPlaceholderText(dialog->tr_text(field.placeholder.toUtf8().constData()));
    if (field.secret) edit->setEchoMode(QLineEdit::Password);
    row->addWidget(edit, 1);
    // Every secret field has a show / hide button, so each can be checked by eye.
    if (field.secret) {
        auto *button = new PushButton(dialog->tr_text("Show"), card);
        button->setFixedHeight(dialog_scaled(32));
        adjust_button_width(button, 60);
        QObject::connect(button, &QPushButton::clicked, dialog, [dialog, edit, button] { toggle_secret(dialog, edit, button); });
        row->addWidget(button);
    }
    if (field.kind == ProviderField::Model) {
        auto *button = new PushButton(dialog->tr_text("Fetch Models"), card);
        button->setFixedHeight(dialog_scaled(32));
        adjust_button_width(button, 60);
        QObject::connect(button, &QPushButton::clicked, dialog, [dialog, provider_id, edit, button] { fetch_models(dialog, provider_id, edit, button); });
        row->addWidget(button);
    }
    card->setFixedHeight(dialog_scaled(58));
    section->addSettingCard(card);
    return edit;
}

// Each provider's last row sends a test sentence with the form's current (possibly unsaved) values.
void build_test_row(SettingsDialog *dialog, ProviderSection *section, const QString &provider_id) {
    auto *card = new WhiteCard(section);
    auto *row = new QHBoxLayout(card);
    row->setContentsMargins(card_text_left(), dialog_scaled(12), card_right_margin(), dialog_scaled(12));
    row->setSpacing(dialog_scaled(10));
    auto *status = new QLabel(card);
    status->setWordWrap(true);
    status->setTextInteractionFlags(status->textInteractionFlags() | Qt::TextSelectableByMouse);
    apply_theme_text_style(status, 13, false, {}, true);
    row->addWidget(status, 1);
    dialog->provider_status_labels[provider_id] = status;
    show_status(dialog, provider_id, "");
    auto *button = new PushButton(dialog->tr_text("Test Connection"), card);
    button->setFixedHeight(dialog_scaled(32));
    adjust_button_width(button, 60);
    QObject::connect(button, &QPushButton::clicked, dialog, [dialog, provider_id, button] { test_connection(dialog, provider_id, button); });
    row->addWidget(button);
    card->setMinimumHeight(dialog_scaled(58));
    section->addSettingCard(card);
}

// Shows or hides a card with its leading separator (only lead_each sections have one).
void set_card_visible(QWidget *card, bool visible) {
    card->setVisible(visible);
    if (auto *separator = qobject_cast<QWidget *>(card->property("_leading_separator").value<QObject *>())) separator->setVisible(visible);
}

QString notice_html(SettingsDialog *dialog, const std::optional<ProviderMetadata> &metadata) {
    if (!metadata) return {};
    QStringList parts;
    if (!metadata->notice.isEmpty()) {
        // URLs never wrap inside: word joiners (U+2060) between their characters keep a narrow
        // description from breaking "http://" from the rest; <nobr> only covers spaces.
        static const QRegularExpression url(R"(https?://[\w.:/%?#=&-]+)", QRegularExpression::NoPatternOption);
        QString text = dialog->tr_text(metadata->notice.toUtf8().constData());
        QString result;
        qsizetype last = 0;
        for (auto it = url.globalMatch(text); it.hasNext();) {
            const auto match = it.next();
            result += text.mid(last, match.capturedStart() - last);
            const QString found = match.captured(0);
            QString joined;
            for (int i = 0; i < found.size(); ++i) {
                if (i) joined += QChar(0x2060);
                joined += found[i];
            }
            result += joined;
            last = match.capturedEnd();
        }
        result += text.mid(last);
        parts.append(result);
    }
    if (!metadata->help_url.isEmpty()) {
        const QString label = !metadata->help_label.isEmpty() ? dialog->tr_text(metadata->help_label.toUtf8().constData()) : QString();
        parts.append(QString("<a href=\"%1\" style=\"color:%2;\">%3</a>").arg(metadata->help_url, ACCENT, !label.isEmpty() ? label : metadata->help_url));
    }
    return parts.join(" ");
}

// Expands only the current provider's settings and shows the general options its declaration
// supports. No provider is named: an unknown or empty one hides everything rather than
// leaving the previous provider's inputs, since the registry is dynamic.
void update_provider_groups(SettingsDialog *dialog) {
    const QString provider_id = dialog->attr<QComboBox>("translation_provider_combo")->currentData().toString();
    for (auto it = dialog->provider_sections.begin(); it != dialog->provider_sections.end(); ++it)
        if (it.value()) it.value()->setVisible(it.key() == provider_id);

    std::optional<ProviderMetadata> metadata;
    if (dialog->translation_registry && !provider_id.isEmpty()) {
        try {
            metadata = dialog->translation_registry->metadata(provider_id);
        } catch (const std::invalid_argument &) {
            metadata.reset();
        }
    }
    const QSet<QString> supported = metadata ? metadata->supported_request_options : QSet<QString>();
    for (auto it = dialog->request_option_cards.begin(); it != dialog->request_option_cards.end(); ++it)
        if (it.value()) set_card_visible(it.value(), supported.contains(it.key()));
    if (auto *section = dialog->attr_widget("switch_options_section")) section->setVisible(!supported.isEmpty());
    if (auto *label = dialog->attr<QLabel>("translation_notice_label")) {
        auto html = notice_html(dialog, metadata);
        const auto usage = quota_usage_text(dialog, provider_id);
        if (!usage.isEmpty()) html = html.isEmpty() ? usage : html + "<br/>" + usage;
        label->setText(html);
        label->setVisible(!html.isEmpty());
    }
    for (const char *attr : {"translation_engine_group", "translation_options_group"})
        if (auto *group = dialog->attr<SettingsCardGroup>(attr)) group->refreshHeight();
}
}

QString quota_usage_text(SettingsDialog *dialog, const QString &provider_id) {
    if (provider_id.isEmpty() || !dialog->translation_registry || !dialog->config_manager) return {};
    std::unique_ptr<TranslationProvider> provider;
    try {
        provider = dialog->translation_registry->create(provider_id, {});
    } catch (const std::invalid_argument &) {
        return {};
    }
    if (provider->quota_limit_chars <= 0) return {};
    const auto custom = dialog->config_manager->get_translation_quota_limit(provider_id);
    const auto limit = custom > 0 ? custom : provider->quota_limit_chars;
    const auto used = dialog->config_manager->get_translation_usage_chars(provider_id);
    const auto text = dialog->config_manager->get_translation_quota_enabled(provider_id) ? dialog->tr_text("Used this month: %1 / %2 characters (quota guard on)")
                                                                                        : dialog->tr_text("Used this month: %1 / %2 characters (quota guard off)");
    // Grouped with commas whatever the interface language.
    const QLocale grouping(QLocale::English, QLocale::UnitedStates);
    return text.arg(grouping.toString(used), grouping.toString(limit));
}

QWidget *create_translation_page(SettingsDialog *dialog) {
    auto *config = dialog->config_manager;
    auto *scroll = page_scroll_area(dialog);
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet("QScrollArea { border: none; background: transparent; }");
    auto *page = new QWidget;
    set_own_style(page, "background: transparent;");
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, dialog_scaled(10), 0);
    layout->setSpacing(dialog_scaled(14));

    const auto service = create_default_translation_service();
    dialog->translation_registry = service->registry;
    const auto providers = service->registry->available_providers();
    dialog->provider_field_widgets.emplace();

    // ════ Translation service ════
    // The provider's credentials and switches sit in this group. The provider also decides
    // which translation options show, so this group comes first: whatever changes on a switch
    // is below the provider row and never pushes the combo away.
    auto *grp_engine = new SettingsCardGroup(dialog->tr_text("Translation Service"), page, "translate");
    auto *engine_card = new SettingCard(FluentIcon::Application, dialog->tr_text("Translation Engine"), {}, grp_engine);
    // The description shows the provider's declared hint and sign-up link, see update_provider_groups.
    engine_card->contentLabel->setOpenExternalLinks(true);
    dialog->set_attr("translation_notice_label", engine_card->contentLabel);
    auto *provider_combo = new ComboBox(engine_card);
    const QString current_provider = config->get_translation_provider();
    int current_provider_index = 0;
    for (int index = 0; index < static_cast<int>(providers.size()); ++index) {
        // Brand names have no translation entry and come back unchanged; only names such as "Custom" change.
        provider_combo->addItem(dialog->tr_text(providers[index].display_name.toUtf8().constData()), providers[index].provider_id);
        if (providers[index].provider_id == current_provider) current_provider_index = index;
    }
    provider_combo->setCurrentIndex(current_provider_index);
    dialog->set_attr("translation_provider_combo", provider_combo);
    engine_card->addControl(provider_combo);
    grp_engine->addSettingCard(engine_card);

    auto *providers_host = new QWidget(grp_engine);
    auto *providers_layout = new QVBoxLayout(providers_host);
    providers_layout->setContentsMargins(0, 0, 0, 0);
    providers_layout->setSpacing(0);
    // One section per provider, rendered from its declaration. The host joins the group as a
    // single card: one card per section would give each a separator, and with one provider
    // visible at a time the others' lines would be left alone.
    dialog->provider_sections.clear();
    dialog->provider_status_labels.clear();
    for (const auto &metadata : providers) {
        auto *section = new ProviderSection(providers_host);
        for (const auto &field : metadata.all_fields()) (*dialog->provider_field_widgets)[field.config_key] = build_field(dialog, section, field, metadata.provider_id);
        build_test_row(dialog, section, metadata.provider_id);
        providers_layout->addWidget(section);
        dialog->provider_sections[metadata.provider_id] = section;
    }
    grp_engine->addSettingCard(providers_host);
    layout->addWidget(grp_engine);
    dialog->set_attr("translation_engine_group", grp_engine);

    // ════ Translation options ════
    auto *grp_opts = new SettingsCardGroup(dialog->tr_text("Translation Options"), page, "translate");
    auto *lang_card = new SettingCard(FluentIcon::Language, dialog->tr_text("Target Language"), {}, grp_opts);
    auto *target_combo = new ComboBox(lang_card);
    std::vector<std::pair<QString, QString>> lang_options{{"", dialog->tr_text("Auto (System)")}};
    lang_options.insert(lang_options.end(), translation_languages().begin(), translation_languages().end());
    const QString current_lang = config->get_app_setting("translation_target_lang", "").toString();
    int current_index = 0;
    for (int i = 0; i < static_cast<int>(lang_options.size()); ++i) {
        target_combo->addItem(lang_options[i].second, lang_options[i].first);
        if (lang_options[i].first == current_lang) current_index = i;
    }
    target_combo->setCurrentIndex(current_index);
    dialog->set_attr("translation_target_combo", target_combo);
    lang_card->addControl(target_combo);
    grp_opts->addSettingCard(lang_card);

    // Only some providers support these two; each card shows by the provider's declared
    // options (update_provider_groups). Each card brings its own separator, so the group adds none.
    auto *switches = new ProviderSection(grp_opts, true);
    dialog->set_attr("switch_options_section", switches);
    auto *split_card = new SwitchSettingCard(FluentIcon::Alignment, dialog->tr_text("Ignore Line Breaks"), dialog->tr_text("Merge multi-line text for better translation"), switches,
                                             "translate");
    split_card->setChecked(config->get_translation_split_sentences());
    dialog->set_attr("split_sentences_toggle", split_card);
    switches->addSettingCard(split_card);
    auto *preserve_card = new SwitchSettingCard(FluentIcon::Document, dialog->tr_text("Preserve Formatting"), dialog->tr_text("Keep original text formatting"), switches,
                                                "translate");
    preserve_card->setChecked(config->get_translation_preserve_formatting());
    dialog->set_attr("preserve_formatting_toggle", preserve_card);
    switches->addSettingCard(preserve_card);
    // Keys match the options providers declare.
    dialog->request_option_cards = {{"split_sentences", split_card}, {"preserve_formatting", preserve_card}};
    grp_opts->addSettingCard(switches, false);
    dialog->set_attr("translation_options_group", grp_opts);
    layout->addWidget(grp_opts);

    QObject::connect(provider_combo, &QComboBox::currentIndexChanged, dialog, [dialog] { update_provider_groups(dialog); });
    update_provider_groups(dialog);

    layout->addStretch();
    scroll->setWidget(page);
    return scroll;
}
}

#include "page_translation.moc"
