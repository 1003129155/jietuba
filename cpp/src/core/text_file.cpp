#include "core/text_file.h"
#include <QFile>
#include <QFileInfo>
#include <QTextDocument>

namespace jietuba {
namespace {
int bom_length(QStringConverter::Encoding encoding) {
    switch (encoding) {
    case QStringConverter::Utf8: return 3;
    case QStringConverter::Utf16LE:
    case QStringConverter::Utf16BE: return 2;
    case QStringConverter::Utf32LE:
    case QStringConverter::Utf32BE: return 4;
    default: return 0;
    }
}

// Cuts off a UTF-8 character the end of the data begins but does not finish.
void drop_incomplete_utf8_tail(QByteArray &bytes) {
    for (int back = 1; back <= 4 && back <= bytes.size(); ++back) {
        const auto byte = static_cast<unsigned char>(bytes[bytes.size() - back]);
        // A continuation byte: the character's start is further back.
        if ((byte & 0xC0) == 0x80) continue;
        const int length = byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3 : byte >= 0xC0 ? 2 : 1;
        if (length > back) bytes.chop(back);
        return;
    }
}
}

QString decode_text_bytes(const QByteArray &bytes, QStringConverter::Encoding fallback) {
    // Stateless: bytes of a character that ends the data are an error, not held back for more.
    constexpr auto flags = QStringConverter::Flag::Stateless;
    if (const auto encoding = QStringConverter::encodingForData(bytes)) return QStringDecoder(*encoding, flags)(bytes.mid(bom_length(*encoding)));
    QStringDecoder utf8(QStringConverter::Utf8, flags);
    const QString text = utf8(bytes);
    if (!utf8.hasError()) return text;
    return QStringDecoder(fallback, flags)(bytes);
}

std::optional<LoadedText> load_text_file(const QString &path, QStringConverter::Encoding fallback, qint64 limit) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
    QByteArray bytes = file.read(limit + 1);
    // A failed read is not an empty file.
    if (bytes.isEmpty() && file.error() != QFileDevice::NoError) return std::nullopt;
    LoadedText loaded;
    if (bytes.size() > limit) {
        loaded.truncated = true;
        bytes.truncate(limit);
        const auto encoding = QStringConverter::encodingForData(bytes);
        if (!encoding || *encoding == QStringConverter::Utf8) drop_incomplete_utf8_tail(bytes);
    }
    loaded.text = decode_text_bytes(bytes, fallback);
    if (loaded.truncated)
        while (loaded.text.endsWith(QChar::ReplacementCharacter)) loaded.text.chop(1);
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == "html" || suffix == "htm") {
        loaded.html = loaded.text;
        QTextDocument document;
        document.setHtml(loaded.html);
        loaded.text = document.toPlainText();
    }
    return loaded;
}
}
