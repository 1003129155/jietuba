#pragma once
#include <QList>
#include <QString>
#include <QStringList>
#include <stdexcept>
#include <tuple>

// CSV export of grouped text items and import back into groups; no UI here.
namespace jietuba::clipboard {
class ClipboardManager;

// Raised for file and parse errors; what() is the message shown to the user.
class CsvError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Encodings tried in order when reading an import file.
const QStringList &DEFAULT_IMPORT_ENCODINGS();

using ImportRow = std::tuple<QString, QString, QString>;

// [group name, content, title] for every non-empty text item, group by group.
QList<QStringList> collect_text_export_rows(ClipboardManager &manager, qint64 page_size = 500);
// csv.writer rows with "\r\n" line ends; characters the encoding lacks become "?".
void write_csv_rows(const QString &file_path, const QStringList &header, const QList<QStringList> &rows, const QString &encoding);
// (group, content, title) rows after the header; rows missing a group or content are skipped.
QList<ImportRow> read_import_rows(const QString &file_path);
// Imports rows newest-last, reusing groups with the same name; the number imported.
int import_text_rows(ClipboardManager &manager, const QList<ImportRow> &rows);

// csv.reader over already decoded text (excel dialect, strict=False).
QList<QStringList> parse_csv(const QString &content);
// One csv.writer row, line end included.
QString format_csv_row(const QStringList &row);
}
