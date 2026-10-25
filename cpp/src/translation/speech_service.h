#pragma once
#include <QHash>
#include <QLocale>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace jietuba {

struct VoiceInfo {
    QString name;
    QLocale locale;
    // Backend-specific handle; the SAPI backend uses the token id.
    QString token;
};

// SAPI's ranges: rate -10..10 (each step about 12% faster), volume 0..100.
struct SpeechStyle {
    int rate = 0;
    int volume = 100;
};

struct SpeechStatus {
    bool done = true;
    qint64 position = 0;
    qint32 hresult = 0;
};

// The voice's Language attribute is a list of hexadecimal LCIDs, such as "804" or "409;9".
std::optional<QLocale> locale_from_lcid(const QString &attribute);

// Reading language of text whose language nobody stated, from the scripts it uses:
// Hangul, then kana, Han, Cyrillic (Ukrainian letters make it UK), Thai, Arabic,
// then Latin as English. Empty when none of these appear.
QString guess_text_language(const QString &text);

// Key under which a voice choice is saved: the ISO 639 code of the language, such as "zh".
QString voice_language_key(const QLocale &locale);
QString voice_language_key(const QString &lang_code);

// The registered voice tokens read straight from the registry, so listing them does not load
// SAPI. Token ids, names and locales are those SAPI reports. Voices that only a token
// enumerator provides cannot be seen this way; has_enumerator is then set.
std::vector<VoiceInfo> registered_voices(const QStringList &categories, bool *has_enumerator = nullptr);
// A self-contained settings query; any COM objects are created and released on its caller thread.
std::vector<VoiceInfo> installed_speech_voices(const QStringList &categories);

// Each call returns false with error set on failure.
class SpeechBackend {
public:
    virtual ~SpeechBackend() = default;
    virtual bool voices(std::vector<VoiceInfo> &found, QString &error) = 0;
    virtual bool speak(const VoiceInfo &voice, const QString &text, const SpeechStyle &style, QString &error) = 0;
    virtual bool purge(QString &error) = 0;
    virtual bool poll(SpeechStatus &status, QString &error) = 0;
    virtual bool close(QString &error) = 0;
};

// Thin wrapper over SpVoice; created and called on the UI thread only.
class SapiBackend : public SpeechBackend {
public:
    // Returns null with error set when SAPI is unavailable.
    static std::shared_ptr<SapiBackend> create(QString &error);
    ~SapiBackend() override;
    bool voices(std::vector<VoiceInfo> &found, QString &error) override;
    bool speak(const VoiceInfo &voice, const QString &text, const SpeechStyle &style, QString &error) override;
    bool purge(QString &error) override;
    bool poll(SpeechStatus &status, QString &error) override;
    bool close(QString &error) override;
    // Synthesises into memory instead of the speakers.
    bool use_memory_output(QString &error);
    qint64 memory_output_size() const;

private:
    SapiBackend();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

using SpeechBackendFactory = std::function<std::shared_ptr<SpeechBackend>(QString &error)>;

// One reading per process: a new request interrupts the old one, buttons show
// their own state by owner.
class SpeechService : public QObject {
    Q_OBJECT
public:
    static constexpr int POLL_INTERVAL_MS = 100;
    // A reading position that stays put this long counts as stuck (audio device
    // unplugged and the like); the queue is purged.
    static constexpr double STALL_SECONDS = 10.0;
    // Minimum interval between enumerations while no voice is found; the user may
    // have just installed one in Windows Settings.
    static constexpr double VOICE_RESCAN_SECONDS = 5.0;

    static SpeechService *instance();
    // Owners are ids rather than addresses so a new window at a freed address is not
    // taken for the old one.
    static quint64 new_owner();
    static void set_backend_factory(SpeechBackendFactory factory);
    static void set_clock(std::function<double()> clock);
    // Registry keys the settings page lists voices from; tests point them at their own keys.
    static void set_voice_categories(const QStringList &categories);
    static QStringList voice_categories();
    // Shuts down and deletes the instance; the next instance() starts over.
    static void reset_instance();

    explicit SpeechService(QObject *parent = nullptr);
    ~SpeechService() override;

    // The voice chosen for the language in the settings, else one of the same language,
    // preferring the same territory. No voice of the same language means none; another
    // language must not read it.
    std::optional<VoiceInfo> voice_for(const QString &lang_code);
    bool can_speak(const QString &lang_code);
    bool is_speaking(quint64 owner) const;
    void toggle(quint64 owner, const QString &text, const QString &lang_code);
    bool speak(quint64 owner, const QString &text, const QString &lang_code);
    // Reads with the given voice token (empty: as voice_for would choose) and style rather
    // than the saved ones, for trying a choice before it is saved.
    bool preview(quint64 owner, const QString &text, const QString &lang_code, const QString &token, const SpeechStyle &style);
    // owner 0 stops unconditionally; otherwise only that owner's reading.
    void stop(quint64 owner = 0);
    void shutdown();

    quint64 owner() const { return owner_; }
    bool polling() const { return timer_.isActive(); }
    void poll();

signals:
    void state_changed();

private:
    SpeechBackend *ensure_backend();
    void load_voices();
    std::optional<VoiceInfo> voice_by_token(const QString &token);
    std::optional<VoiceInfo> automatic_voice(const QString &lang_code);
    bool start(quint64 owner, const QString &text, const std::optional<VoiceInfo> &voice, const SpeechStyle &style);
    // A voice uninstalled since the list was read is still listed and fails to start; the list
    // is then read again and the voice chosen anew, once.
    bool start_or_relist(quint64 owner, const QString &text, const std::function<std::optional<VoiceInfo>()> &choose, const SpeechStyle &style);
    void purge();
    void finish();

    std::shared_ptr<SpeechBackend> backend_;
    bool backend_failed_ = false;
    std::vector<VoiceInfo> all_voices_;
    QHash<int, std::vector<VoiceInfo>> voices_;
    std::optional<double> voices_loaded_at_;
    quint64 owner_ = 0;
    qint64 last_position_ = 0;
    double last_progress_at_ = 0.0;
    QTimer timer_;
};
}
