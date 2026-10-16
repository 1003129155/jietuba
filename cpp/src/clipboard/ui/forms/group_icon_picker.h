#pragma once
#include <QLabel>
#include <QString>

class QWidget;

// The group icon picker: emoji input, preview, category tabs and the preset grid.
namespace jietuba::clipboard {
class ManageDialog;

void create_group_icon_picker(ManageDialog *dialog, const QString &current_icon = QString::fromUtf8("📁"));
QString emoji_tab_style(bool active, const QWidget *dialog = nullptr);
QString emoji_btn_style(const QWidget *dialog = nullptr);
void switch_emoji_group(ManageDialog *dialog, int group_idx);
// The first character after leading spaces, as a whole grapheme cluster: ❤️, 🧔‍♂️, 👍🏽 and flags
// span several code points.
QString first_icon_cluster(const QString &text);
// Keeps only the first non-space character and previews it.
void on_icon_input_changed(ManageDialog *dialog, const QString &text);
void on_preset_icon_clicked(ManageDialog *dialog, const QString &icon);

// The large preview tile of the current group icon.
class IconPreview : public QLabel {
    Q_OBJECT
public:
    explicit IconPreview(const QString &icon);
    Q_INVOKABLE void _apply_theme();
};
}
