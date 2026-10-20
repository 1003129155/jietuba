#pragma once
#include <vector>
#include <QCoreApplication>
#include <QHash>
#include <QMap>
#include <QObject>
#include <QTranslator>
#include <functional>

namespace jietuba {
class I18nSignals : public QObject {
    Q_OBJECT
signals:
    void language_changed(const QString &lang_code);
};

// Loads Qt Linguist .ts XML; used when no compiled .qm is available.
class XmlTranslator : public QTranslator {
public:
    using QTranslator::QTranslator;
    bool load_from_xml(const QString &xml_path);
    QString translate(const char *context, const char *source_text, const char *disambiguation = nullptr, int n = -1) const override;
    bool isEmpty() const override { return translations_.isEmpty(); }
private:
    QHash<QString, QHash<QString, QString>> translations_;
};

class I18nManager {
public:
    static I18nManager &instance();
    // Emits language_changed(lang_code) after a catalog switch.
    I18nSignals *language_signals() const { return signals_; }
    static const QMap<QString, QString> &LANGUAGES();
    static QStringList get_translations_dirs();
    static bool load_language(const QString &lang_code);
    static QString get_current_language() { return current_lang_; }
    static QString get_current_language_name();
    static QMap<QString, QString> get_available_languages() { return LANGUAGES(); }
    // (code, name) in the reference's listing order; the map above is sorted by code.
    static std::vector<std::pair<QString, QString>> language_items() {
        std::vector<std::pair<QString, QString>> items;
        for (const char *code : {"ja", "en", "ko", "zh", "zh_TW"}) items.emplace_back(code, LANGUAGES().value(code));
        return items;
    }
    static QString get_system_language();
private:
    I18nManager();
    QTranslator *translator_ = nullptr;
    I18nSignals *signals_ = nullptr;
    static inline QString current_lang_ = "ja";
};

QString i18n_tr(const char *text, const char *context = "I18nManager");
QString i18n_tr(const QString &text, const char *context = "I18nManager");
// Translation helper bound to a fixed context: auto _tr = make_tr("UndoCommands");
std::function<QString(const char *)> make_tr(const char *context);
}

// Gives a class tr() with the Python class name as its translation context, so
// the shipped .qm files (keyed by Python class names) apply unchanged. Placed
// after Q_OBJECT it hides QObject::tr, whose context would include the namespace.
#define JIETUBA_TR_CONTEXT(Context) \
public: \
    static QString tr(const char *source, const char *disambiguation = nullptr, int n = -1) \
    { return QCoreApplication::translate(Context, source, disambiguation, n); } \
private:
