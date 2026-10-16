#pragma once
#include "core/i18n.h"
#include <QDialog>
#include <QMenu>
#include <QToolButton>
#include <optional>

namespace jietuba {
class CaptionLabel;
class TextEdit;
class ToolSettingsManager;

// (short name on the button, Qt date format, description). Short names are format
// codes shared by every language; descriptions follow the UI language.
struct TimeFormat {
    const char *name;
    const char *format;
    const char *description;
};
const std::vector<TimeFormat> &watermark_time_formats();
QString default_watermark_time_format();
QString time_token(const QString &time_format);
// A format typed by the user is not in the list, so it is shown as itself.
QString time_format_short_name(const QString &time_format);

// Watermark text input. Ctrl+Enter applies; Enter starts a new line, since watermarks may span lines.
class WatermarkInputDialog : public QDialog {
    Q_OBJECT
public:
    static constexpr int BASE_WIDTH = 420;
    static constexpr int BASE_EDIT_HEIGHT = 110;
    explicit WatermarkInputDialog(const QString &template_text, const QString &time_format = default_watermark_time_format(), QWidget *parent = nullptr);
    QString template_text() const;
    QString time_format() const { return time_format_; }
    // Inserts a time placeholder at the cursor; a given format also becomes the button's format.
    void insert_time(const QString &time_format = {});
    // Below anchor, or above when there is no room below; left-aligned.
    void place_near(QWidget *anchor);
    TextEdit *edit = nullptr;
    CaptionLabel *preview = nullptr;
    QToolButton *time_button = nullptr;
    QMenu *time_menu = nullptr;
private:
    void sync_time_button();
    // Built on open so each entry shows an example of the current moment.
    void fill_time_menu();
    void refresh_preview();
    QString time_format_;
};

// Opens the input dialog. Returns the new text when applied, nullopt when cancelled;
// the time format used is remembered either way.
std::optional<QString> edit_watermark_text(QWidget *anchor, ToolSettingsManager *settings_manager);
}
