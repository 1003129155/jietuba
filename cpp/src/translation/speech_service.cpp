#include "translation/speech_service.h"
#include "core/logger.h"
#include "core/py_text.h"
#include <QCoreApplication>
#include <QSet>
#include <QStringList>
#include <chrono>
#include <limits>

#include <windows.h>
#include <sapi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace jietuba {
namespace {
constexpr DWORD SPEAK_FLAGS = SPF_ASYNC | SPF_PURGEBEFORESPEAK;
// Without this flag a translation starting with "<" is parsed as SSML and fails when malformed.
constexpr DWORD SPEAK_TEXT_FLAGS = SPEAK_FLAGS | SPF_IS_NOT_XML;
// Voices added in Settings > Speech are only registered here; SAPI drives them too.
constexpr const wchar_t *ONECORE_VOICES = L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech_OneCore\\Voices";

QString hresult_hex(qint64 hr) { return QString("0x%1").arg(static_cast<quint32>(hr), 8, 16, QChar('0')); }

QString com_error(const char *call, HRESULT hr) { return QString("%1 failed: HRESULT %2").arg(call, hresult_hex(hr)); }

QString take_string(wchar_t *value) {
    const auto text = QString::fromWCharArray(value);
    CoTaskMemFree(value);
    return text;
}

// Accepts surrounding whitespace, a sign, a 0x prefix and single underscores between digits.
std::optional<qint64> parse_hex(const QString &raw) {
    const QString text = py_strip(raw);
    qsizetype i = 0;
    bool negative = false;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) negative = text[i++] == '-';
    if (i + 1 < text.size() && text[i] == '0' && (text[i + 1] == 'x' || text[i + 1] == 'X')) {
        i += 2;
        if (i < text.size() && text[i] == '_') ++i;
    }
    // Beyond the C long range the Windows call cannot take the value anyway.
    constexpr qint64 limit = qint64(1) << 33;
    qint64 value = 0;
    bool any_digit = false, after_underscore = false;
    for (; i < text.size(); ++i) {
        const QChar ch = text[i];
        int digit = -1;
        if (ch == '_') {
            if (!any_digit || after_underscore) return std::nullopt;
            after_underscore = true;
            continue;
        }
        if (ch >= 'a' && ch <= 'f') digit = ch.unicode() - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') digit = ch.unicode() - 'A' + 10;
        else if (ch.category() == QChar::Number_DecimalDigit) digit = ch.digitValue();
        if (digit < 0) return std::nullopt;
        value = std::min(value * 16 + digit, limit);
        any_digit = true;
        after_underscore = false;
    }
    if (!any_digit || after_underscore) return std::nullopt;
    return negative ? -value : value;
}

// SpGetDescription: the value named after the UI language, else the default value.
bool token_description(ISpObjectToken *token, QString &name) {
    wchar_t language[8];
    swprintf_s(language, L"%x", GetUserDefaultUILanguage());
    wchar_t *value = nullptr;
    if (FAILED(token->GetStringValue(language, &value)) && FAILED(token->GetStringValue(nullptr, &value))) return false;
    name = take_string(value);
    return true;
}

bool token_attribute(ISpObjectToken *token, const wchar_t *attribute, QString &result) {
    ComPtr<ISpDataKey> attributes;
    wchar_t *value = nullptr;
    if (FAILED(token->OpenKey(L"Attributes", &attributes)) || FAILED(attributes->GetStringValue(attribute, &value))) return false;
    result = take_string(value);
    return true;
}

SpeechBackendFactory &backend_factory() {
    static SpeechBackendFactory factory;
    return factory;
}

std::function<double()> &clock() {
    static std::function<double()> now;
    return now;
}

double monotonic() {
    if (clock()) return clock()();
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

SpeechService *g_instance = nullptr;
quint64 g_last_owner = 0;
}

QString guess_text_language(const QString &text) {
    bool kana = false, han = false, cyrillic = false, ukrainian = false, thai = false, arabic = false, latin = false;
    for (const char32_t ch : text.toUcs4()) {
        switch (QChar::script(ch)) {
        case QChar::Script_Hangul: return "KO";
        case QChar::Script_Hiragana:
        case QChar::Script_Katakana: kana = true; break;
        case QChar::Script_Han: han = true; break;
        case QChar::Script_Cyrillic:
            cyrillic = true;
            // і ї є ґ do not occur in Russian.
            if (QStringView(u"\u0456\u0457\u0454\u0491\u0406\u0407\u0404\u0490").contains(QChar(char16_t(ch)))) ukrainian = true;
            break;
        case QChar::Script_Thai: thai = true; break;
        case QChar::Script_Arabic: arabic = true; break;
        case QChar::Script_Latin: latin = latin || QChar::isLetter(ch); break;
        default: break;
        }
    }
    if (kana) return "JA";
    if (han) return "ZH";
    if (cyrillic) return ukrainian ? "UK" : "RU";
    if (thai) return "TH";
    if (arabic) return "AR";
    if (latin) return "EN";
    return {};
}

std::optional<QLocale> locale_from_lcid(const QString &attribute) {
    const auto lcid = parse_hex(attribute.split(';').first());
    if (!lcid) return std::nullopt;
    // A primary language of 0 is a placeholder such as "user default", which would
    // resolve to the current locale.
    if ((*lcid & 0x3FF) == 0) return std::nullopt;
    if (*lcid > 0xFFFFFFFFLL || *lcid < std::numeric_limits<qint32>::min()) return std::nullopt;
    wchar_t buffer[LOCALE_NAME_MAX_LENGTH];
    if (!LCIDToLocaleName(static_cast<LCID>(*lcid), buffer, LOCALE_NAME_MAX_LENGTH, 0)) return std::nullopt;
    QLocale locale(QString::fromWCharArray(buffer));
    if (locale.language() == QLocale::C) return std::nullopt;
    return locale;
}

struct SapiBackend::Impl {
    bool com_initialized = false;
    ComPtr<ISpVoice> voice;
    // ComPtr overloads operator&, so it is kept out of Qt containers.
    std::vector<std::pair<QString, ComPtr<ISpObjectToken>>> tokens;
    ComPtr<IStream> memory;
};

SapiBackend::SapiBackend() : impl_(std::make_unique<Impl>()) {}

SapiBackend::~SapiBackend() {
    impl_->memory.Reset();
    impl_->tokens.clear();
    impl_->voice.Reset();
    if (impl_->com_initialized) CoUninitialize();
}

std::shared_ptr<SapiBackend> SapiBackend::create(QString &error) {
    std::shared_ptr<SapiBackend> backend(new SapiBackend);
    // RPC_E_CHANGED_MODE: the thread was initialised in the other apartment, which SpVoice also supports.
    backend->impl_->com_initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    const HRESULT hr = CoCreateInstance(__uuidof(SpVoice), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&backend->impl_->voice));
    if (FAILED(hr)) {
        error = com_error("CoCreateInstance(SpVoice)", hr);
        return nullptr;
    }
    return backend;
}

bool SapiBackend::voices(std::vector<VoiceInfo> &found, QString &error) {
    found.clear();
    impl_->tokens.clear();
    if (!impl_->voice) {
        error = "engine closed";
        return false;
    }
    QSet<QString> seen;
    for (const wchar_t *category_id : {SPCAT_VOICES, ONECORE_VOICES}) {
        ComPtr<ISpObjectTokenCategory> category;
        ComPtr<IEnumSpObjectTokens> tokens;
        ULONG count = 0;
        HRESULT hr = CoCreateInstance(__uuidof(SpObjectTokenCategory), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&category));
        if (SUCCEEDED(hr)) hr = category->SetId(category_id, FALSE);
        if (SUCCEEDED(hr)) hr = category->EnumTokens(nullptr, nullptr, &tokens);
        if (SUCCEEDED(hr)) hr = tokens->GetCount(&count);
        if (FAILED(hr)) {
            log_debug(QString("Speech voice category unavailable: %1 HRESULT %2").arg(QString::fromWCharArray(category_id), hresult_hex(hr)), "Speech");
            continue;
        }
        for (ULONG index = 0; index < count; ++index) {
            // A single broken registration (missing attributes, engine uninstalled) only skips that voice.
            ComPtr<ISpObjectToken> token;
            QString name, language, id;
            wchar_t *token_id = nullptr;
            if (FAILED(tokens->Item(index, &token)) || !token_description(token.Get(), name) || !token_attribute(token.Get(), L"Language", language))
                continue;
            const auto locale = locale_from_lcid(language);
            if (!locale || seen.contains(name) || FAILED(token->GetId(&token_id))) continue;
            id = take_string(token_id);
            seen.insert(name);
            impl_->tokens.emplace_back(id, token);
            found.push_back({name, *locale, id});
        }
    }
    return true;
}

bool SapiBackend::speak(const VoiceInfo &voice, const QString &text, QString &error) {
    ComPtr<ISpObjectToken> token;
    for (const auto &[id, candidate] : impl_->tokens)
        if (id == voice.token) token = candidate;
    if (!impl_->voice || !token) {
        error = "voice unavailable: " + voice.token;
        return false;
    }
    HRESULT hr = impl_->voice->SetVoice(token.Get());
    if (FAILED(hr)) {
        error = com_error("SetVoice", hr);
        return false;
    }
    hr = impl_->voice->Speak(reinterpret_cast<const wchar_t *>(text.utf16()), SPEAK_TEXT_FLAGS, nullptr);
    if (FAILED(hr)) {
        error = com_error("Speak", hr);
        return false;
    }
    return true;
}

bool SapiBackend::purge(QString &error) {
    if (!impl_->voice) {
        error = "engine closed";
        return false;
    }
    const HRESULT hr = impl_->voice->Speak(L"", SPEAK_FLAGS, nullptr);
    if (FAILED(hr)) {
        error = com_error("Speak(purge)", hr);
        return false;
    }
    return true;
}

bool SapiBackend::poll(SpeechStatus &status, QString &error) {
    if (!impl_->voice) {
        error = "engine closed";
        return false;
    }
    SPVOICESTATUS voice_status{};
    HRESULT hr = impl_->voice->GetStatus(&voice_status, nullptr);
    if (FAILED(hr)) {
        error = com_error("GetStatus", hr);
        return false;
    }
    hr = impl_->voice->WaitUntilDone(0);
    if (FAILED(hr)) {
        error = com_error("WaitUntilDone", hr);
        return false;
    }
    status = {hr == S_OK, static_cast<qint64>(voice_status.ulInputWordPos), static_cast<qint32>(voice_status.hrLastResult)};
    return true;
}

bool SapiBackend::close(QString &error) {
    const bool purged = purge(error);
    impl_->tokens.clear();
    impl_->voice.Reset();
    return purged;
}

bool SapiBackend::use_memory_output(QString &error) {
    ComPtr<ISpStreamFormat> stream;
    HRESULT hr = CoCreateInstance(__uuidof(SpMemoryStream), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&stream));
    if (SUCCEEDED(hr)) hr = stream.As(&impl_->memory);
    if (SUCCEEDED(hr)) hr = impl_->voice ? impl_->voice->SetOutput(stream.Get(), TRUE) : E_POINTER;
    if (FAILED(hr)) {
        error = com_error("SetOutput(SpMemoryStream)", hr);
        return false;
    }
    return true;
}

qint64 SapiBackend::memory_output_size() const {
    STATSTG stat{};
    if (!impl_->memory || FAILED(impl_->memory->Stat(&stat, STATFLAG_NONAME))) return -1;
    return static_cast<qint64>(stat.cbSize.QuadPart);
}

SpeechService *SpeechService::instance() {
    if (!g_instance) {
        auto *app = QCoreApplication::instance();
        g_instance = new SpeechService(app);
        connect(app, &QCoreApplication::aboutToQuit, g_instance, &SpeechService::shutdown);
    }
    return g_instance;
}

quint64 SpeechService::new_owner() { return ++g_last_owner; }

void SpeechService::set_backend_factory(SpeechBackendFactory factory) { backend_factory() = std::move(factory); }

void SpeechService::set_clock(std::function<double()> now) { clock() = std::move(now); }

void SpeechService::reset_instance() {
    auto *service = g_instance;
    g_instance = nullptr;
    if (service) {
        service->shutdown();
        delete service;
    }
}

SpeechService::SpeechService(QObject *parent) : QObject(parent) {
    timer_.setInterval(POLL_INTERVAL_MS);
    connect(&timer_, &QTimer::timeout, this, &SpeechService::poll);
}

SpeechService::~SpeechService() {
    if (g_instance == this) g_instance = nullptr;
    if (backend_) {
        QString error;
        backend_->close(error);
    }
}

SpeechBackend *SpeechService::ensure_backend() {
    if (!backend_ && !backend_failed_) {
        QString error;
        backend_ = backend_factory() ? backend_factory()(error) : SapiBackend::create(error);
        if (!backend_) {
            // No SAPI or no COM: treated as "no voice" and not retried.
            backend_failed_ = true;
            log_warning("Speech engine unavailable: " + error, "Speech");
        }
    }
    return backend_.get();
}

void SpeechService::load_voices() {
    auto *backend = ensure_backend();
    voices_.clear();
    voices_loaded_at_ = monotonic();
    if (!backend) return;
    std::vector<VoiceInfo> voices;
    QString error;
    if (!backend->voices(voices, error)) {
        log_warning("Speech voice enumeration failed: " + error, "Speech");
        return;
    }
    QStringList names;
    for (const auto &voice : voices) {
        voices_[static_cast<int>(voice.locale.language())].push_back(voice);
        names << voice.name;
    }
    log_debug("Speech voices: " + names.join(", "), "Speech");
}

std::optional<VoiceInfo> SpeechService::voice_for(const QString &lang_code) {
    if (lang_code.isEmpty()) return std::nullopt;
    if (!voices_loaded_at_) load_voices();
    const QLocale wanted(QString(lang_code).replace('-', '_'));
    const int language = static_cast<int>(wanted.language());
    if (voices_.value(language).empty() && monotonic() - *voices_loaded_at_ >= VOICE_RESCAN_SECONDS) load_voices();
    const auto it = voices_.constFind(language);
    if (it == voices_.cend() || it->empty()) return std::nullopt;
    for (const auto &voice : *it)
        if (voice.locale.territory() == wanted.territory()) return voice;
    return it->front();
}

bool SpeechService::can_speak(const QString &lang_code) { return voice_for(lang_code).has_value(); }

bool SpeechService::is_speaking(quint64 owner) const { return owner != 0 && owner_ == owner; }

void SpeechService::toggle(quint64 owner, const QString &text, const QString &lang_code) {
    if (is_speaking(owner)) stop(owner);
    else speak(owner, text, lang_code);
}

bool SpeechService::speak(quint64 owner, const QString &raw_text, const QString &lang_code) {
    const QString text = py_strip(raw_text);
    const auto voice = text.isEmpty() ? std::nullopt : voice_for(lang_code);
    if (!voice || !backend_) return false;
    QString error;
    // The purge-first flag replaces whatever is being read without waiting for it to stop.
    if (!backend_->speak(*voice, text, error)) {
        log_warning(QString("Speech failed to start with %1: %2").arg(voice->name, error), "Speech");
        // Setting the voice may have failed, leaving the previous reading running.
        purge();
        finish();
        return false;
    }
    owner_ = owner;
    last_position_ = 0;
    last_progress_at_ = monotonic();
    timer_.start();
    emit state_changed();
    return true;
}

void SpeechService::stop(quint64 owner) {
    if (owner != 0 && owner_ != owner) return;
    if (owner_ == 0) return;
    purge();
    finish();
}

void SpeechService::shutdown() {
    stop();
    auto backend = std::move(backend_);
    backend_failed_ = true;
    if (backend) {
        QString error;
        if (!backend->close(error)) log_debug("Speech engine close failed: " + error, "Speech");
    }
}

void SpeechService::purge() {
    if (!backend_) return;
    QString error;
    if (!backend_->purge(error)) log_warning("Speech failed to stop: " + error, "Speech");
}

void SpeechService::finish() {
    timer_.stop();
    if (owner_ != 0) {
        owner_ = 0;
        emit state_changed();
    }
}

void SpeechService::poll() {
    if (owner_ == 0 || !backend_) {
        finish();
        return;
    }
    SpeechStatus status;
    QString error;
    if (!backend_->poll(status, error)) {
        log_warning("Speech status unavailable: " + error, "Speech");
        purge();
        finish();
        return;
    }
    if (status.done) {
        // An asynchronous failure also ends the stream; the code may be left over from an
        // earlier reading, so it is only logged.
        if (status.hresult < 0) log_warning("Speech ended with HRESULT " + hresult_hex(status.hresult), "Speech");
        finish();
    } else if (status.position != last_position_) {
        last_position_ = status.position;
        last_progress_at_ = monotonic();
    } else if (monotonic() - last_progress_at_ >= STALL_SECONDS) {
        log_warning("Speech made no progress, giving up", "Speech");
        purge();
        finish();
    }
}
}
