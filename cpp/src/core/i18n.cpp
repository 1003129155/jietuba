#include "core/i18n.h"
#include "core/logger.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLocale>
#include <QXmlStreamReader>

namespace jietuba {
bool XmlTranslator::load_from_xml(const QString &xml_path) {
    QFile file(xml_path);
    if (!file.open(QIODevice::ReadOnly)) {
        log_error(T("加载 XML 翻译文件失败: {e}", {{"e", file.errorString()}}), "I18n");
        return false;
    }
    translations_.clear();
    QXmlStreamReader xml(&file);
    QString context, source, translation;
    bool has_source = false, has_translation = false, unfinished = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const auto name = xml.name();
            if (name == QLatin1String("context")) { context.clear(); }
            else if (name == QLatin1String("name") && xml.namespaceUri().isEmpty()) { context = xml.readElementText(); translations_[context]; }
            else if (name == QLatin1String("message")) { source.clear(); translation.clear(); has_source = has_translation = unfinished = false; }
            else if (name == QLatin1String("source")) { source = xml.readElementText(); has_source = true; }
            else if (name == QLatin1String("translation")) {
                unfinished = xml.attributes().value("type") == QLatin1String("unfinished");
                translation = xml.readElementText(); has_translation = true;
            }
        } else if (xml.isEndElement() && xml.name() == QLatin1String("message")) {
            if (has_source && has_translation) {
                // Empty or unfinished translations use the source text.
                translations_[context][source] = translation.isEmpty() || unfinished ? source : translation;
            }
        }
    }
    if (xml.hasError()) {
        log_error(T("加载 XML 翻译文件失败: {e}", {{"e", xml.errorString()}}), "I18n");
        return false;
    }
    return true;
}

QString XmlTranslator::translate(const char *context, const char *source_text, const char *, int) const {
    const auto source = QString::fromUtf8(source_text);
    if (const auto found = translations_.constFind(QString::fromUtf8(context)); found != translations_.constEnd())
        if (const auto text = found->constFind(source); text != found->constEnd()) return *text;
    // Some translations are shared across contexts.
    for (const auto &table : translations_)
        if (const auto text = table.constFind(source); text != table.constEnd()) return *text;
    return {};
}

I18nManager::I18nManager() : translator_(new XmlTranslator), signals_(new I18nSignals) {}

I18nManager &I18nManager::instance() {
    static I18nManager manager;
    return manager;
}

const QMap<QString, QString> &I18nManager::LANGUAGES() {
    static const QMap<QString, QString> languages = {
        {"ja", QString::fromUtf8("日本語")}, {"en", "English"}, {"ko", QString::fromUtf8("한국어")}, {"zh", QString::fromUtf8("简体中文")},
        {"zh_TW", QString::fromUtf8("繁體中文")},
    };
    return languages;
}

QStringList I18nManager::get_translations_dirs() {
    // Compiled catalogs are embedded; a translations folder next to the exe can override them.
    return {QDir(QCoreApplication::applicationDirPath()).filePath("translations"), ":/translations"};
}

bool I18nManager::load_language(const QString &lang_code) {
    if (!LANGUAGES().contains(lang_code)) {
        log_warning(T("不支持的语言: {lang_code}", {{"lang_code", lang_code}}), "I18n");
        return false;
    }
    auto *app = QCoreApplication::instance();
    if (!app) {
        log_warning(T("QApplication 实例不存在"), "I18n");
        return false;
    }
    auto &self = instance();
    if (self.translator_) QCoreApplication::removeTranslator(self.translator_);
    QString qm_file, xml_file;
    for (const auto &dir : get_translations_dirs()) {
        const auto qm = QDir(dir).filePath(QString("app_%1.qm").arg(lang_code));
        const auto xml = QDir(dir).filePath(QString("app_%1.xml").arg(lang_code));
        if (qm_file.isEmpty() && QFile::exists(qm)) qm_file = qm;
        if (xml_file.isEmpty() && QFile::exists(xml)) xml_file = xml;
    }
    if (!qm_file.isEmpty()) {
        auto *qt_translator = new QTranslator;
        if (qt_translator->load(qm_file)) {
            delete self.translator_;
            self.translator_ = qt_translator;
            QCoreApplication::installTranslator(self.translator_);
            current_lang_ = lang_code;
            log_info(T("已加载语言 (QM): {lang_name} ({lang_code})", {{"lang_name", LANGUAGES().value(lang_code)}, {"lang_code", lang_code}}), "I18n");
            emit self.signals_->language_changed(lang_code);
            return true;
        }
        delete qt_translator;
        log_warning(T("QM 文件加载失败，尝试 XML: {qm_file}", {{"qm_file", qm_file}}), "I18n");
    }
    auto *xml_translator = new XmlTranslator;
    delete self.translator_;
    self.translator_ = xml_translator;
    if (!xml_file.isEmpty()) {
        if (xml_translator->load_from_xml(xml_file)) {
            QCoreApplication::installTranslator(self.translator_);
            current_lang_ = lang_code;
            log_info(T("已加载语言 (XML): {lang_name} ({lang_code})", {{"lang_name", LANGUAGES().value(lang_code)}, {"lang_code", lang_code}}), "I18n");
            emit self.signals_->language_changed(lang_code);
            return true;
        }
        log_error(T("加载翻译文件失败: {xml_file}", {{"xml_file", xml_file}}), "I18n");
        return false;
    }
    // Without a catalog the source texts are used.
    current_lang_ = lang_code;
    log_info(T("翻译文件不存在: {xml_file}，使用默认文本", {{"xml_file", QString("app_%1.xml").arg(lang_code)}}), "I18n");
    emit self.signals_->language_changed(lang_code);
    return true;
}

QString I18nManager::get_current_language_name() { return LANGUAGES().value(current_lang_, current_lang_); }

QString I18nManager::get_system_language() {
    switch (QLocale::system().language()) {
    case QLocale::Japanese: return "ja";
    case QLocale::English: return "en";
    case QLocale::Korean: return "ko";
    case QLocale::Chinese: {
        // Traditional is what Taiwan, Hong Kong and Macao read, and what a Traditional script asks for.
        const auto locale = QLocale::system();
        const bool traditional = locale.script() == QLocale::TraditionalHanScript || locale.territory() == QLocale::Taiwan
            || locale.territory() == QLocale::HongKong || locale.territory() == QLocale::Macao;
        return traditional ? "zh_TW" : "zh";
    }
    default: return "en";
    }
}

QString i18n_tr(const char *text, const char *context) {
    const auto translated = QCoreApplication::translate(context, text);
    return translated.isEmpty() ? QString::fromUtf8(text) : translated;
}

QString i18n_tr(const QString &text, const char *context) {
    const auto bytes = text.toUtf8();
    return i18n_tr(bytes.constData(), context);
}

std::function<QString(const char *)> make_tr(const char *context) {
    return [context](const char *text) { return i18n_tr(text, context); };
}
}
