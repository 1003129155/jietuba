#pragma once
#include <QString>
#include <utility>
#include <vector>

namespace jietuba {
// Target languages with their native names, shared by every language list.
inline const std::vector<std::pair<QString, QString>> &translation_languages() {
    static const std::vector<std::pair<QString, QString>> languages = {
        {"ZH", QString::fromUtf8("中文")}, {"ZH-HANT", QString::fromUtf8("繁体中文")}, {"EN", "English"}, {"JA", QString::fromUtf8("日本語")},
        {"KO", QString::fromUtf8("한국어")}, {"VI", QString::fromUtf8("Tiếng Việt")}, {"DE", "Deutsch"}, {"FR", QString::fromUtf8("Français")},
        {"ES", QString::fromUtf8("Español")}, {"IT", "Italiano"}, {"PT", QString::fromUtf8("Português")}, {"RU", QString::fromUtf8("Русский")},
        {"PL", "Polski"}, {"NL", "Nederlands"}, {"TR", QString::fromUtf8("Türkçe")}, {"TH", QString::fromUtf8("ไทย")},
        {"AR", QString::fromUtf8("العربية")}, {"ID", "Indonesia"}, {"UK", QString::fromUtf8("Українська")},
    };
    return languages;
}

// Source languages: automatic detection (translated in the UI) plus the targets.
inline const std::vector<std::pair<QString, QString>> &source_languages() {
    static const std::vector<std::pair<QString, QString>> languages = [] {
        std::vector<std::pair<QString, QString>> result = {{"auto", "Auto Detect"}};
        for (const auto &entry : translation_languages()) result.push_back(entry);
        return result;
    }();
    return languages;
}
}
