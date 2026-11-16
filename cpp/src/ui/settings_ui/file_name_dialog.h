#pragma once
#include <QDialog>
#include <QDateTime>
#include <QStringList>

class QLineEdit;
class QLabel;
class QPushButton;
namespace jietuba {
class FileNameDialog : public QDialog {
public:
    explicit FileNameDialog(const QStringList &patterns, QWidget *parent = nullptr, const QString &image_format = "PNG");
    QStringList patterns() const;
    void accept() override;
private:
    void refresh();
    QList<QLineEdit *> edits_;
    QList<QLabel *> previews_;
    QStringList field_names_;
    QLabel *error_;
    QPushButton *ok_;
    const QDateTime preview_time_ = QDateTime::currentDateTime();
    QString image_extension_;
};
}
