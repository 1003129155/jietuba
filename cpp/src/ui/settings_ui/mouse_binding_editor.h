#pragma once
#include <QString>
#include <QWidget>

// Modifier combination plus mouse gesture; currentData() is like "ctrl+dragright".
namespace jietuba {
class ComboBox;

class MouseBindingEditor : public QWidget {
    Q_OBJECT
public:
    // kind: "wheel", "drag", "click", "capture" or "global".
    explicit MouseBindingEditor(const QString &kind, QWidget *parent = nullptr);
    QString currentData() const;
    void setBinding(const QString &binding);
    ComboBox *modifiers;
    ComboBox *gesture;
private:
    // Choosing a mouse button adds Win; clearing it clears the modifiers too.
    void pair_modifiers();
    void pair_gesture();
    bool modifier_required_;
};
}
