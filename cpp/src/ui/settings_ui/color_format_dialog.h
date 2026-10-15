#pragma once
#include "settings/color_formats.h"
#include <QDialog>

// The magnifier's colour format manager. Formats are a fixed set of presets: checked ones
// join the magnifier's cycle (Shift switches, C copies the shown one) and the order decides
// where the cycle starts, so dragging one to the top makes it the default. The dialog only
// edits; the caller reads and writes the configuration.
namespace jietuba {
class ReorderableRowList;

class ColorFormatDialog : public QDialog {
    Q_OBJECT
public:
    explicit ColorFormatDialog(const QVector<color_formats::ColorFormat> &formats, QWidget *parent = nullptr);
    // The edited list, in display order.
    QVector<color_formats::ColorFormat> entries() const;
private:
    void fill(const QVector<color_formats::ColorFormat> &formats);
    // Shows every format if possible; the list scrolls only past the screen's available height.
    void fit_height_to_rows();
    ReorderableRowList *list_;
};
}
