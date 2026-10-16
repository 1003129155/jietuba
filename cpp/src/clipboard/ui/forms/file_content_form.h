#pragma once
#include "clipboard/core/models.h"
#include <QFrame>
#include <QLabel>
#include <QUrl>
#include <functional>

// The file item form of the manage window: title, path, browse buttons and a drop zone.
namespace jietuba::clipboard {
class ManageDialog;

// The drop target for a file or folder path.
class FileDropZone : public QFrame {
    Q_OBJECT
public:
    FileDropZone(std::function<void(const QString &)> on_path_dropped, const QString &prompt_text, QWidget *parent = nullptr);
    Q_INVOKABLE void _apply_theme();
    QLabel *prompt_label;

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    static QString extract_first_local_path(const QList<QUrl> &urls);
    bool apply_dropped_urls(const QList<QUrl> &urls);
    std::function<void(const QString &)> on_path_dropped_;
    bool drag_active_ = false;
    QString default_style_;
    QString hover_style_;
};

void build_file_content_form(ManageDialog *dialog, bool add_stretch = true);
void build_edit_file_content_form(ManageDialog *dialog, const ClipboardItem &item, const QString &file_path);
}
