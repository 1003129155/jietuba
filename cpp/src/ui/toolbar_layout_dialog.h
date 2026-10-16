#pragma once
#include "ui/reorderable_rows.h"
#include "ui/toolbar_layout.h"
#include <QDialog>
#include <QIcon>
#include <QJsonArray>
#include <QMap>

namespace jietuba {
class ComboBox;

// One row: drag grip, button icon, name and a visibility combo.
class ToolbarLayoutRow : public DraggableRow {
    Q_OBJECT
public:
    ToolbarLayoutRow(const QString &key, const QIcon &icon, QWidget *parent = nullptr);
    QString mode() const;
    void set_mode(const QString &mode);
    QString key;
    ComboBox *combo = nullptr;
};

// Edits one toolbar's layout. layout is [[button, mode]]; icons maps button -> icon; profile holds the
// toolbar's layout rules. Reading and writing settings and rebuilding the toolbar are up to the caller.
class ToolbarLayoutDialog : public QDialog {
    Q_OBJECT
public:
    ToolbarLayoutDialog(const QJsonArray &layout, const QMap<QString, QIcon> &icons, QWidget *parent = nullptr,
                        const toolbar_layout::LayoutProfile &profile = toolbar_layout::SCREENSHOT);
    // The edited layout; locked buttons go back to their positions from when the dialog was filled.
    QJsonArray entries() const;
private:
    void fill(const QJsonArray &layout);
    // Shows every adjustable row; the list scrolls only past the screen's available height.
    void fit_height_to_rows();
    const toolbar_layout::LayoutProfile &profile_;
    ReorderableRowList *list_ = nullptr;
    QMap<QString, ToolbarLayoutRow *> rows_;
    std::vector<std::pair<int, QString>> locked_slots_;
};
}
