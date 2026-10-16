#pragma once
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QString>
#include <QVariant>
#include <utility>
#include <vector>

namespace jietuba {
// Runs dialog modally, blocking only its own window so the capture overlay and
// other top-levels stay usable. exec() would otherwise default to application
// modality. owner gives a parentless dialog a window to belong to.
int exec_dialog(QDialog *dialog, QWidget *owner = nullptr);

// (path, selected filter); the path is empty when cancelled.
std::pair<QString, QString> get_open_file_name(QWidget *parent, const QString &caption = {}, const QString &directory = {}, const QString &name_filter = {});
std::pair<QString, QString> get_save_file_name(QWidget *parent, const QString &caption = {}, const QString &directory = {}, const QString &name_filter = {});
QString get_existing_directory(QWidget *parent, const QString &caption = {}, const QString &directory = {});

// Replacement for QMessageBox that neither steals global focus nor stays on top
// of unrelated windows.
class StandardDialog : public QDialog {
    Q_OBJECT
public:
    StandardDialog(QWidget *parent, const QString &title, const QString &message);
    QLabel *label;
    QDialogButtonBox *button_box;
    QVariant result_action;
};

QDialog *track_modeless_dialog(QDialog *dialog);

struct DialogButton {
    QVariant id;
    QString text;
    QDialogButtonBox::ButtonRole role;
    bool is_default = false;
};
// Returns the id of the clicked button (the reject-role id when closed).
QVariant show_custom_confirm_dialog(QWidget *parent, const QString &title, const QString &message, const std::vector<DialogButton> &buttons_config);
bool show_confirm_dialog(QWidget *parent, const QString &title, const QString &message);
// (confirmed, checkbox checked)
std::pair<bool, bool> show_confirm_checkbox_dialog(QWidget *parent, const QString &title, const QString &message, const QString &checkbox_text,
                                                   bool checkbox_checked = false);
void show_info_dialog(QWidget *parent, const QString &title, const QString &message);
void show_warning_dialog(QWidget *parent, const QString &title, const QString &message);
void show_text_dialog(QWidget *parent, const QString &title, const QString &content);
void show_update_dialog(QWidget *parent, const QString &title, const QString &content, const QString &download_caption,
                        const QString &download_url, const QString &download_button_text);
// For flows such as capture that must not block; single_line shows the message
// in a scrollable read-only field (paths).
QDialog *show_modeless_warning_dialog(QWidget *parent, const QString &title, const QString &message, bool single_line = false);
void show_error_dialog(QWidget *parent, const QString &title, const QString &message);
}
