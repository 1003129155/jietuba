#include "clipboard/services/import_export_service.h"
#include "clipboard/core/manager.h"
#include "core/py_text.h"
#include <QFile>
#include <QHash>
#include <QStringDecoder>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <share.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace jietuba::clipboard {
namespace {
constexpr qsizetype FIELD_LIMIT = 131072;

// repr() of a path as it appears in OSError messages.
QString path_repr(const QString &path) {
    QString escaped = path;
    escaped.replace("\\", "\\\\").replace("'", "\\'");
    return "'" + escaped + "'";
}

[[noreturn]] void raise_os_error(int error_number, const QString &path) {
    char message[256] = {};
    strerror_s(message, sizeof(message), error_number);
    throw CsvError(QString("[Errno %1] %2: %3").arg(error_number).arg(QString::fromLocal8Bit(message), path_repr(path)).toStdString());
}

// shift_jis, gbk and the others go through the Windows code pages Excel reads and writes.
UINT code_page(const QString &encoding) {
    static const QHash<QString, UINT> pages{{"shift_jis", 932}, {"cp932", 932}, {"gbk", 936}, {"gb2312", 936}, {"gb18030", 54936}, {"cp1252", 1252}};
    return pages.value(encoding, 0);
}

QByteArray encode(const QString &text, const QString &encoding) {
    if (encoding == "utf-8-sig") return QByteArray("\xEF\xBB\xBF") + text.toUtf8();
    if (encoding == "utf-8") return text.toUtf8();
    const UINT page = code_page(encoding);
    if (!page || text.isEmpty()) return text.toUtf8();
    QString chars = text;
    if (page != 54936) {
        // Nothing outside the BMP fits these pages; errors="replace" gives one "?" per character, not per UTF-16 unit.
        chars.clear();
        for (qsizetype i = 0; i < text.size(); ++i) {
            const bool pair = text[i].isHighSurrogate() && i + 1 < text.size() && text[i + 1].isLowSurrogate();
            chars += pair ? QChar(u'?') : text[i];
            if (pair) ++i;
        }
    }
    const auto source = reinterpret_cast<LPCWCH>(chars.utf16());
    const int size = static_cast<int>(chars.size());
    // No best-fit mapping: unencodable characters become "?" as errors="replace" does.
    const DWORD flags = page == 54936 ? 0 : WC_NO_BEST_FIT_CHARS;
    const char *fallback = page == 54936 ? nullptr : "?";
    const int needed = WideCharToMultiByte(page, flags, source, size, nullptr, 0, fallback, nullptr);
    QByteArray out(needed, '\0');
    WideCharToMultiByte(page, flags, source, size, out.data(), needed, fallback, nullptr);
    return out;
}

bool decode_page(const QByteArray &data, UINT page, QString *out) {
    if (data.isEmpty()) {
        out->clear();
        return true;
    }
    const int needed = MultiByteToWideChar(page, MB_ERR_INVALID_CHARS, data.constData(), static_cast<int>(data.size()), nullptr, 0);
    if (needed <= 0) return false;
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(page, MB_ERR_INVALID_CHARS, data.constData(), static_cast<int>(data.size()), wide.data(), needed);
    *out = QString::fromWCharArray(wide.data(), needed);
    return true;
}

// Strict decoding; false when the bytes are not valid in the encoding.
bool decode(const QByteArray &data, const QString &encoding, QString *out) {
    if (encoding == "utf-8-sig" || encoding == "utf-8") {
        QByteArray bytes = data;
        if (encoding == "utf-8-sig" && bytes.startsWith("\xEF\xBB\xBF")) bytes.remove(0, 3);
        QStringDecoder decoder(QStringDecoder::Utf8);
        const QString text = decoder.decode(bytes);
        if (decoder.hasError()) return false;
        *out = text;
        return true;
    }
    if (encoding == "latin-1") {
        *out = QString::fromLatin1(data);
        return true;
    }
    const UINT page = code_page(encoding);
    if (!page) return false;
    if (page == 1252) {
        // Python's cp1252 leaves these bytes undefined; Windows maps them through.
        for (char byte : data)
            if (QByteArray("\x81\x8D\x8F\x90\x9D").contains(byte)) return false;
    }
    // GB18030 reads GBK's two-byte area with real characters where cp936 has private-use ones
    // (€ at A2E3 and others); only cp936 takes the single-byte € (0x80) Excel writes.
    if (page == 936) return decode_page(data, 54936, out) || decode_page(data, 936, out);
    return decode_page(data, page, out);
}

QByteArray read_file(const QString &file_path) {
    // Shared like Python's open(), so a file another program keeps open can still be used.
    FILE *file = _wfsopen(reinterpret_cast<const wchar_t *>(file_path.utf16()), L"rb", _SH_DENYNO);
    if (!file) raise_os_error(errno, file_path);
    QByteArray data;
    char buffer[65536];
    size_t count = 0;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0) data.append(buffer, static_cast<qsizetype>(count));
    fclose(file);
    return data;
}

enum class State { START_RECORD, START_FIELD, IN_FIELD, IN_QUOTED_FIELD, QUOTE_IN_QUOTED_FIELD, EAT_CRNL };

class CsvReader {
public:
    QList<QStringList> rows;

    void feed_line(const QString &line) {
        for (QChar c : line) process(c.unicode(), false);
        process(0, true);
        if (state_ == State::START_RECORD) finish_record();
    }

    void end() {
        if (!field_.isEmpty() || state_ == State::IN_QUOTED_FIELD) {
            save_field();
            finish_record();
        }
    }

private:
    void save_field() {
        fields_.append(field_);
        field_.clear();
    }
    void add_char(char16_t c) {
        if (field_.size() >= FIELD_LIMIT) throw CsvError(QString("field larger than field limit (%1)").arg(FIELD_LIMIT).toStdString());
        field_ += QChar(c);
    }
    void finish_record() {
        if (record_started_) rows.append(fields_);
        else rows.append(QStringList());
        fields_.clear();
        record_started_ = false;
    }

    void process(char16_t c, bool eol) {
        const bool newline = !eol && (c == u'\n' || c == u'\r');
        switch (state_) {
        case State::START_RECORD:
            if (eol) return;  // empty line
            record_started_ = true;
            if (newline) {
                state_ = State::EAT_CRNL;
                return;
            }
            state_ = State::START_FIELD;
            [[fallthrough]];
        case State::START_FIELD:
            record_started_ = true;
            if (newline || eol) {
                save_field();
                state_ = eol ? State::START_RECORD : State::EAT_CRNL;
            } else if (c == u'"') {
                state_ = State::IN_QUOTED_FIELD;
            } else if (c == u',') {
                save_field();
            } else {
                add_char(c);
                state_ = State::IN_FIELD;
            }
            return;
        case State::IN_FIELD:
            if (newline || eol) {
                save_field();
                state_ = eol ? State::START_RECORD : State::EAT_CRNL;
            } else if (c == u',') {
                save_field();
                state_ = State::START_FIELD;
            } else {
                add_char(c);
            }
            return;
        case State::IN_QUOTED_FIELD:
            if (eol) return;
            if (c == u'"') state_ = State::QUOTE_IN_QUOTED_FIELD;
            else add_char(c);
            return;
        case State::QUOTE_IN_QUOTED_FIELD:
            if (!eol && c == u'"') {
                add_char(c);
                state_ = State::IN_QUOTED_FIELD;
            } else if (!eol && c == u',') {
                save_field();
                state_ = State::START_FIELD;
            } else if (newline || eol) {
                save_field();
                state_ = eol ? State::START_RECORD : State::EAT_CRNL;
            } else {
                add_char(c);
                state_ = State::IN_FIELD;
            }
            return;
        case State::EAT_CRNL:
            if (newline) return;
            if (eol) {
                state_ = State::START_RECORD;
                return;
            }
            throw CsvError("new-line character seen in unquoted field - do you need to open the file with newline=''?");
        }
    }

    State state_ = State::START_RECORD;
    QString field_;
    QStringList fields_;
    bool record_started_ = false;
};
}

const QStringList &DEFAULT_IMPORT_ENCODINGS() {
    static const QStringList encodings{"utf-8-sig", "utf-8", "shift_jis", "cp932", "gbk", "gb2312", "gb18030", "cp1252", "latin-1"};
    return encodings;
}

QList<QStringList> collect_text_export_rows(ClipboardManager &manager, qint64 page_size) {
    QList<QStringList> rows;
    for (const auto &group : manager.get_groups()) {
        qint64 offset = 0;
        while (true) {
            const auto items = manager.get_by_group(group.id, offset, page_size);
            if (items.isEmpty()) break;
            for (const auto &item : items)
                if (item.content_type == "text" && !item.content.isEmpty()) rows.append({group.name, item.content, item.title.value_or(QString())});
            if (items.size() < page_size) break;
            offset += page_size;
        }
    }
    return rows;
}

QString format_csv_row(const QStringList &row) {
    QStringList fields;
    for (const auto &field : row) {
        bool quoted = false;
        QString out;
        for (QChar c : field) {
            if (c == u',' || c == u'"' || c == u'\r' || c == u'\n') {
                quoted = true;
                if (c == u'"') out += u'"';
            }
            out += c;
        }
        fields.append(quoted ? "\"" + out + "\"" : out);
    }
    return fields.join(",") + "\r\n";
}

void write_csv_rows(const QString &file_path, const QStringList &header, const QList<QStringList> &rows, const QString &encoding) {
    // Shared like Python's open(), so a file another program keeps open can still be used.
    FILE *file = _wfsopen(reinterpret_cast<const wchar_t *>(file_path.utf16()), L"wb", _SH_DENYNO);
    if (!file) raise_os_error(errno, file_path);
    QString text = format_csv_row(header);
    for (const auto &row : rows) text += format_csv_row(row);
    const QByteArray data = encode(text, encoding);
    // A full disk fails the export as the reference's write raises, not silently.
    const bool written = fwrite(data.constData(), 1, static_cast<size_t>(data.size()), file) == static_cast<size_t>(data.size());
    const int write_error = written ? 0 : (errno ? errno : EIO);
    const bool closed = fclose(file) == 0;
    if (!written) raise_os_error(write_error, file_path);
    if (!closed) raise_os_error(errno ? errno : EIO, file_path);
}

QList<QStringList> parse_csv(const QString &content) {
    CsvReader reader;
    // io.StringIO yields lines split after each "\n", the newline kept.
    qsizetype start = 0;
    while (start < content.size()) {
        const qsizetype end = content.indexOf(u'\n', start);
        const qsizetype stop = end < 0 ? content.size() : end + 1;
        reader.feed_line(content.mid(start, stop - start));
        start = stop;
    }
    reader.end();
    return reader.rows;
}

QList<ImportRow> read_import_rows(const QString &file_path) {
    const QByteArray data = read_file(file_path);
    QString content;
    bool decoded = false;
    for (const auto &encoding : DEFAULT_IMPORT_ENCODINGS()) {
        if (decode(data, encoding, &content)) {
            decoded = true;
            break;
        }
    }
    if (!decoded) content = QString::fromUtf8(data);

    QList<ImportRow> rows;
    auto records = parse_csv(content);
    if (!records.isEmpty()) records.removeFirst();
    for (const auto &row : records) {
        if (row.size() < 2) continue;
        const QString group_name = py_strip(row[0]);
        const QString item_content = row[1];
        const QString title = row.size() > 2 ? py_strip(row[2]) : QString();
        if (!group_name.isEmpty() && !item_content.isEmpty()) rows.append({group_name, item_content, title});
    }
    return rows;
}

int import_text_rows(ClipboardManager &manager, const QList<ImportRow> &rows) {
    QHash<QString, qint64> existing_group_name_to_id;
    for (const auto &group : manager.get_groups()) existing_group_name_to_id.insert(group.name, group.id);
    QHash<QString, qint64> import_group_name_to_target;

    int imported_count = 0;
    for (auto it = rows.crbegin(); it != rows.crend(); ++it) {
        const auto &[group_name, content, title] = *it;
        if (!import_group_name_to_target.contains(group_name)) {
            if (existing_group_name_to_id.contains(group_name)) {
                import_group_name_to_target.insert(group_name, existing_group_name_to_id.value(group_name));
            } else {
                const auto new_group_id = manager.create_group(group_name);
                if (!new_group_id || !*new_group_id) continue;
                existing_group_name_to_id.insert(group_name, *new_group_id);
                import_group_name_to_target.insert(group_name, *new_group_id);
            }
        }
        const qint64 group_id = import_group_name_to_target.value(group_name);
        const auto item_id = manager.add_item(content, "text", title.isEmpty() ? std::nullopt : std::optional<QString>(title));
        if (item_id && *item_id) {
            manager.move_to_group(*item_id, group_id);
            imported_count += 1;
        }
    }
    return imported_count;
}
}
