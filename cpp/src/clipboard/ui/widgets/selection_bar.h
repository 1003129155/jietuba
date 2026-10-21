#pragma once
#include "clipboard/ui/theme/themes.h"
#include <QWidget>

class QLabel;
class QPushButton;
class QToolButton;

namespace jietuba::clipboard {
// The bar under the list while anything is marked: how many rows are marked, pasting and saving them,
// the other actions for them, and leaving the multiple selection.
class SelectionBar : public QWidget {
    Q_OBJECT
public:
    explicit SelectionBar(QWidget *parent = nullptr);
    // Shown while the count is above zero.
    void set_count(int count);
    // Colours from the theme, sizes from the toolbar and panel scale.
    void apply_style(const Theme &theme);
    void retranslate();

    QLabel *count_label = nullptr;
    QPushButton *paste_button = nullptr;
    QPushButton *save_button = nullptr;
    QToolButton *more_button = nullptr;
    QToolButton *exit_button = nullptr;

signals:
    void paste_requested();
    void save_requested();
    void more_requested();
    void exit_requested();

private:
    int count_ = 0;
};
}
