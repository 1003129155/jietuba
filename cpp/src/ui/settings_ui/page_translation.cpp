#include "core/resource_manager.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "settings/tool_settings.h"
#include "translation/languages.h"
#include "translation/registry.h"
#include "translation/service.h"
#include "translation/speech_service.h"
#include "translation/worker.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/labels.h"
#include "ui/fluent_lite/switch.h"
#include "ui/fluent_lite/text_context_menu.h"
#include "ui/settings_ui/components.h"
#include "ui/settings_ui/dialog.h"
#include "ui/settings_ui/pages.h"
#include "ui/settings_ui/search.h"
#include <QDesktopServices>
#include <QDate>
#include <QFrame>
#include <QLocale>
#include <QMenu>
#include <QRegularExpression>
#include <QThread>
#include <QToolTip>
#include <QUrl>
#include <algorithm>
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
    QLabel *description = nullptr;
    if (!field.description.isEmpty()) {
        description = new QLabel(dialog->tr_text(field.description.toUtf8().constData()), card);
        description->setWordWrap(true);
        apply_theme_text_style(description, 12, false, {}, true);
        text->addWidget(description);
    }
    mark_search_row(card, title, description);
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
    mark_search_row(card, title);
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

// SAPI rates for the labelled speeds: each rate step is about 12% faster.
const std::pair<const char *, int> SPEECH_RATES[] = {{"0.5\u00d7", -6}, {"0.75\u00d7", -3}, {"1\u00d7", 0}, {"1.25\u00d7", 2}, {"1.5\u00d7", 4}, {"2\u00d7", 6}};
const int SPEECH_VOLUMES[] = {100, 80, 60, 40, 20};

QString speech_sample(const QLocale &locale) {
    const QString key = voice_language_key(locale);
    if (key == "zh") return QString::fromUtf8("你好，这是朗读音色的试听。");
    if (key == "ja") return QString::fromUtf8("こんにちは。読み上げ音声のテストです。");
    if (key == "ko") return QString::fromUtf8("안녕하세요. 읽기 음성 미리 듣기입니다.");
    if (key == "en") return "Hello, this is a preview of the reading voice.";
    return locale.nativeLanguageName();
}

// "Microsoft Huihui - Chinese (Simplified, PRC)" shows as "Huihui": the language is already the
// row's title, and the shared vendor prefix would leave the narrow combo no room for the name.
QString voice_label(const VoiceInfo &voice) {
    QString label = voice.name.section(" - ", 0, 0).trimmed();
    if (label.startsWith("Microsoft ", Qt::CaseInsensitive) && label.size() > 10) label = label.mid(10);
    return label;
}

ComboBox *speech_style_combo(SettingsDialog *dialog, SettingCard *card, const QString &key, const std::vector<std::pair<QString, int>> &items) {
    auto *combo = new ComboBox(card);
    for (const auto &[label, value] : items) combo->addItem(label, value);
    int index = combo->findData(dialog->config_manager->get_app_setting(key));
    if (index < 0) index = combo->findData(dialog->config_manager->app_defaults().value(key));
    combo->setCurrentIndex(std::max(0, index));
    dialog->behavior_controls.emplace_back(key, combo);
    card->addControl(combo);
    return combo;
}

SpeechStyle pending_speech_style(SettingsDialog *dialog) {
    SpeechStyle style;
    if (auto *combo = qobject_cast<QComboBox *>(dialog->behavior_control("speech_rate"))) style.rate = combo->currentData().toInt();
    if (auto *combo = qobject_cast<QComboBox *>(dialog->behavior_control("speech_volume"))) style.volume = combo->currentData().toInt();
    return style;
}

// Reads a sample with the row's current voice and the page's current speed and volume, so a
// choice can be heard before the autosave writes it; pressed again it stops. An icon button,
// since a captioned one would leave the voice combo too little of the control column.
PushButton *speech_preview_button(SettingsDialog *dialog, SettingCard *card, QComboBox *combo, const QLocale &locale) {
    const QString preview_text = dialog->tr_text("Preview"), stop_text = dialog->tr_text("Stop");
    const IconSource preview_icon = ResourceManager::get_icon_path(QString::fromUtf8("录制声音.svg"));
    const IconSource stop_icon = ResourceManager::get_icon_path(QString::fromUtf8("结束录制.svg"));
    auto *button = new PushButton({}, card, preview_icon);
    button->setFixedSize(dialog_scaled(32), dialog_scaled(32));
    button->setIconSize(QSize(dialog_scaled(16), dialog_scaled(16)));
    button->setToolTip(preview_text);
    button->setAccessibleName(preview_text);
    const quint64 owner = SpeechService::new_owner();
    QPointer<SpeechService> service = SpeechService::instance();
    QObject::connect(button, &QPushButton::clicked, button, [dialog, combo, locale, service, owner] {
        if (!service) return;
        if (service->is_speaking(owner)) service->stop(owner);
        else service->preview(owner, speech_sample(locale), locale.name(), combo->currentData().toString(), pending_speech_style(dialog));
    });
    QObject::connect(service, &SpeechService::state_changed, button, [button, service, owner, preview_text, stop_text, preview_icon, stop_icon] {
        const bool speaking = service && service->is_speaking(owner);
        button->setIcon(speaking ? stop_icon : preview_icon);
        button->setToolTip(speaking ? stop_text : preview_text);
        button->setAccessibleName(speaking ? stop_text : preview_text);
    });
    QObject::connect(button, &QObject::destroyed, service, [service, owner] {
        if (service) service->stop(owner);
    });
    return button;
}

QStringList voice_tokens(const std::vector<VoiceInfo> &voices) {
    QStringList tokens;
    for (const auto &voice : voices) tokens << voice.token.toLower() + "|" + voice.name + "|" + voice.locale.name();
    tokens.sort();
    return tokens;
}

// One row per language with a voice installed, in the order of the translation languages.
SettingsCardGroup *build_speech_group(SettingsDialog *dialog, QWidget *page, std::vector<VoiceInfo> voices) {
    auto *group = new SettingsCardGroup(dialog->tr_text("Read Aloud"), page, "translate");
    // What the rows were built from, so a later visit can tell whether voices came or went.
    group->setProperty("voice_tokens", voice_tokens(voices));
    dialog->set_attr("speech_group", group);
    std::stable_sort(voices.begin(), voices.end(), [](const VoiceInfo &a, const VoiceInfo &b) { return voice_label(a).compare(voice_label(b), Qt::CaseInsensitive) < 0; });
    QStringList keys;
    QMap<QString, QString> titles;
    for (const auto &[code, name] : translation_languages()) {
        const QString key = voice_language_key(code);
        if (!titles.contains(key)) titles[key] = name;
    }
    for (const auto &voice : voices) {
        const QString key = voice_language_key(voice.locale);
        if (!keys.contains(key)) keys << key;
        if (!titles.contains(key)) titles[key] = voice.locale.nativeLanguageName();
    }
    const auto order = [](const QString &key) {
        const auto &languages = translation_languages();
        const auto it = std::find_if(languages.begin(), languages.end(), [&key](const auto &entry) { return voice_language_key(entry.first) == key; });
        return static_cast<int>(it - languages.begin());
    };
    std::stable_sort(keys.begin(), keys.end(), [&order](const QString &a, const QString &b) { return order(a) != order(b) ? order(a) < order(b) : a < b; });

    dialog->speech_voice_combos.clear();
    for (const auto &key : keys) {
        auto *card = new SettingCard(FluentIcon::Language, titles.value(key), {}, group);
        auto *combo = new ComboBox(card);
        combo->addItem(dialog->tr_text("Auto"), QString());
        for (const auto &voice : voices)
            if (voice_language_key(voice.locale) == key) combo->addItem(voice_label(voice), voice.token);
        const auto select_voice = [dialog, combo, key] {
            auto &preferences = dialog->speech_voice_preferences;
            preferences.removeIf([&key](const QString &entry) { return entry.section('=', 0, 0) == key; });
            const QString token = combo->currentData().toString();
            if (!token.isEmpty()) preferences << key + "=" + token;
        };
        QObject::connect(combo, &QComboBox::currentIndexChanged, dialog, select_voice);
        QObject::connect(combo, &QComboBox::activated, dialog, [dialog, select_voice] {
            // Auto can already be displayed for an unavailable preference.
            select_voice();
            dialog->schedule_save();
        });
        card->addControl(combo);
        card->addControl(speech_preview_button(dialog, card, combo, QLocale(key)), Qt::AlignRight);
        group->addSettingCard(card);
        dialog->speech_voice_combos.emplace_back(key, combo);
    }
    select_speech_voices(dialog, dialog->config_manager->get_speech_voices());

    auto *rate_card = new SettingCard(FluentIcon::StopWatch, dialog->tr_text("Speech Rate"), {}, group);
    std::vector<std::pair<QString, int>> rates;
    for (const auto &[label, rate] : SPEECH_RATES) rates.emplace_back(QString::fromUtf8(label), rate);
    speech_style_combo(dialog, rate_card, "speech_rate", rates);
    group->addSettingCard(rate_card);
    auto *volume_card = new SettingCard(FluentIcon::FontSize, dialog->tr_text("Volume"), {}, group);
    std::vector<std::pair<QString, int>> volumes;
    for (const int volume : SPEECH_VOLUMES) volumes.emplace_back(QString("%1%").arg(volume), volume);
    speech_style_combo(dialog, volume_card, "speech_volume", volumes);
    group->addSettingCard(volume_card);

    auto *add_card = new SettingCard(FluentIcon::Download, dialog->tr_text("Add Voices"), dialog->tr_text("Install voices for more languages in Windows Settings."),
                                     group);
    auto *open_button = new PushButton(dialog->tr_text("Open Settings"), add_card);
    open_button->setFixedHeight(dialog_scaled(32));
    adjust_button_width(open_button, 60);
    QObject::connect(open_button, &QPushButton::clicked, dialog, [] { QDesktopServices::openUrl(QUrl("ms-settings:speech")); });
    add_card->addControl(open_button, Qt::AlignRight);
    group->addSettingCard(add_card);
    return group;
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
        // Until the first query returns, the count comes from the settings rather than as 0.
        const auto read = dialog->translation_usage.constFind(provider_id);
        const auto usage = quota_usage_text(dialog, provider_id, read == dialog->translation_usage.cend() ? std::nullopt : std::optional<qint64>(*read));
        if (!usage.isEmpty()) html = html.isEmpty() ? usage : html + "<br/>" + usage;
        label->setText(html);
        label->setVisible(!html.isEmpty());
    }
    for (const char *attr : {"translation_engine_group", "translation_options_group"})
        if (auto *group = dialog->attr<SettingsCardGroup>(attr)) group->refreshHeight();
}
}

QString quota_usage_text(SettingsDialog *dialog, const QString &provider_id, std::optional<qint64> usage) {
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
    const auto used = usage ? *usage : dialog->config_manager->get_translation_usage_chars(provider_id);
    const auto text = dialog->config_manager->get_translation_quota_enabled(provider_id) ? dialog->tr_text("Used this month: %1 / %2 characters (quota guard on)")
                                                                                        : dialog->tr_text("Used this month: %1 / %2 characters (quota guard off)");
    // Grouped with commas whatever the interface language.
    const QLocale grouping(QLocale::English, QLocale::UnitedStates);
    return text.arg(grouping.toString(used), grouping.toString(limit));
}

QStringList speech_voice_choices(const SettingsDialog *dialog) {
    QStringList result = dialog->speech_voice_preferences;
    result.sort();
    return result;
}

SettingsCardGroup *rebuild_speech_group(SettingsDialog *dialog, std::vector<VoiceInfo> voices) {
    auto *old_group = dialog->attr<SettingsCardGroup>("speech_group");
    if (!old_group) return nullptr;
    if (voice_tokens(voices) == old_group->property("voice_tokens").toStringList()) return nullptr;
    QWidget *page = old_group->parentWidget();
    auto *layout = qobject_cast<QBoxLayout *>(page->layout());
    if (!layout) return nullptr;
    // Choices not saved yet survive the rebuild.
    const QStringList choices = speech_voice_choices(dialog);
    const SpeechStyle style = pending_speech_style(dialog);
    const int index = layout->indexOf(old_group);
    delete old_group;
    auto &controls = dialog->behavior_controls;
    controls.erase(std::remove_if(controls.begin(), controls.end(), [](const auto &entry) { return !entry.second; }), controls.end());
    auto *group = build_speech_group(dialog, page, std::move(voices));
    layout->insertWidget(index, group);
    select_speech_voices(dialog, choices);
    for (const auto &[key, value] : {std::pair<const char *, int>{"speech_rate", style.rate}, {"speech_volume", style.volume}})
        if (auto *combo = qobject_cast<QComboBox *>(dialog->behavior_control(key)); combo && combo->findData(value) >= 0) combo->setCurrentIndex(combo->findData(value));
    return group;
}

void select_speech_voices(SettingsDialog *dialog, const QStringList &saved) {
    dialog->speech_voice_preferences = saved;
    for (const auto &[key, combo] : dialog->speech_voice_combos) {
        if (!combo) continue;
        QString token;
        for (const auto &entry : saved)
            if (entry.section('=', 0, 0) == key) token = entry.section('=', 1);
        const QSignalBlocker blocker(combo);
        int index = 0;
        for (int i = 1; i < combo->count(); ++i)
            if (combo->itemData(i).toString().compare(token, Qt::CaseInsensitive) == 0) index = i;
        combo->setCurrentIndex(index);
    }
}

void reset_speech_voices(SettingsDialog *dialog) {
    auto choices = speech_voice_choices(dialog);
    for (const auto &[key, combo] : dialog->speech_voice_combos)
        choices.removeIf([&key](const QString &entry) { return entry.section('=', 0, 0) == key; });
    select_speech_voices(dialog, choices);
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
    const QString current_lang = config->get_saved_translation_target_lang();
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

    // ════ Read aloud ════
    layout->addWidget(build_speech_group(dialog, page, {}));
    const auto categories = SpeechService::voice_categories();
    dialog->add_external_refresh([categories] { return installed_speech_voices(categories); }, [dialog](const std::vector<VoiceInfo> &voices) {
        if (auto *group = rebuild_speech_group(dialog, voices)) dialog->adopt_controls(group);
    });
    const auto file = config->settings().fileName();
    const auto format = config->settings().format();
    dialog->add_external_refresh([file, format, providers] {
        QSettings settings(file, format);
        QMap<QString, qint64> usage;
        const auto prefix = "translation/usage/" + QDate::currentDate().toString("yyyyMM") + "/";
        for (const auto &provider : providers) usage[provider.provider_id] = settings.value(prefix + provider.provider_id, 0).toLongLong();
        return usage;
    }, [dialog](const QMap<QString, qint64> &usage) {
        dialog->translation_usage = usage;
        update_provider_groups(dialog);
    });

    layout->addStretch();
    scroll->setWidget(page);
    return scroll;
}
}

#include "page_translation.moc"
