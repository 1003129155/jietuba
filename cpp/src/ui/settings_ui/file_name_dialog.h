#pragma once
#include "ui/fluent_lite/fluent_dialog.h"
#include <QDateTime>
#include <QStringList>

class QLineEdit;
class QLabel;
class QPushButton;
class QStackedWidget;
namespace jietuba {
class FileNameDialog : public FluentDialog {
public:
    explicit FileNameDialog(const QStringList &patterns, QWidget *parent = nullptr, const QString &image_format = "PNG");
    QStringList patterns() const;
    void accept() override;
private:
    void refresh();
    QList<QLineEdit *> edits_;
    QList<QLabel *> previews_;
    QStringList field_names_;
    // The hint, or the first problem in its place.
    QStackedWidget *notice_;
    QLabel *error_;
    QPushButton *ok_;
    const QDateTime preview_time_ = QDateTime::currentDateTime();
    QString image_extension_;
};
}
