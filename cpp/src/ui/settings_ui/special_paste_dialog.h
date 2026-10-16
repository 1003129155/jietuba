#pragma once
#include "settings/special_paste.h"
#include <QDialog>
#include <QMap>

class QStackedWidget;

// Order and switches of the text and file Special Paste menus. The dialog only edits; the
// settings window keeps the entries and saves them on Apply.
namespace jietuba {
class ReorderableRowList;
class SegmentedWidget;
class TransparentPushButton;

class SpecialPasteDialog : public QDialog {
    Q_OBJECT
public:
    explicit SpecialPasteDialog(const QList<special_paste::Entry> &entries, QWidget *parent = nullptr);
    QList<special_paste::Entry> entries() const;
    SegmentedWidget *tabs;
    TransparentPushButton *reset_button;

private:
    void fill(const QList<special_paste::Entry> &entries);
    QStackedWidget *stack_;
    QMap<QString, ReorderableRowList *> lists_;
};
}
