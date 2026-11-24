#pragma once
#include <QByteArray>
#include <QString>
#include <QStringConverter>
#include <optional>

namespace jietuba {
struct LoadedText {
    QString text;
    // The file as written, for an html file; its text is then the page's plain text.
    QString html;
    // The file was longer than the limit; only its start is here.
    bool truncated = false;
};
inline constexpr qint64 TEXT_FILE_LIMIT = 64 * 1024;

// Bytes as text: by the byte order mark when there is one, else as UTF-8 when they are valid
// UTF-8, else in fallback. A cut that fell inside a character leaves that character out.
QString decode_text_bytes(const QByteArray &bytes, QStringConverter::Encoding fallback = QStringConverter::System);

// The start of a text file, at most limit bytes. Nothing when it cannot be read. html and htm
// files keep their markup in html.
std::optional<LoadedText> load_text_file(const QString &path, QStringConverter::Encoding fallback = QStringConverter::System, qint64 limit = TEXT_FILE_LIMIT);
}
