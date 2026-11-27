#pragma once
// ui/fluent_lite/__init__.py: the check button defined in the package
// itself, plus every submodule, as one include.
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/cards.h"
#include "ui/fluent_lite/frameless.h"
#include "ui/fluent_lite/icons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/labels.h"
#include "ui/fluent_lite/navigation.h"
#include "ui/fluent_lite/segmented.h"
#include "ui/fluent_lite/switch.h"
#include "ui/fluent_lite/theme.h"
#include "ui/fluent_lite/titlebar.h"
#include <QCheckBox>
#include <QVariantAnimation>

namespace jietuba {
// Draws a crisp tick and a separately placed label: Qt's QSS indicator looks thick and
// some Windows/QSS combinations paint the label underneath the indicator. On screen the box
// fades between unchecked and checked.
class CheckBox : public QCheckBox {
    Q_OBJECT
public:
    static constexpr int LABEL_SPACING = 7;
    explicit CheckBox(const QString &text = {}, QWidget *parent = nullptr);
    Q_INVOKABLE void _apply_theme();
    // How far the box is towards checked, 0 to 1.
    double fill() const { return fill_; }
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    double fill_ = 0;
    QVariantAnimation fade_;
};
}
