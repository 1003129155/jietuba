#pragma once
#include <QLineEdit>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <utility>

// Key chips and status icons shared by the global and in-app shortcut editors: the
// configuration value stays as it is ("ctrl+1", "mouseforward") and is only drawn as
// "Ctrl + 1" or "Mouse Forward".
namespace jietuba {
constexpr int CHIP_WIDTH = 196;
constexpr int CHIP_HEIGHT = 28;
constexpr int STATUS_SIZE = 18;
constexpr int STATUS_GAP = 12;

// Configuration value to (display name of each part, whether it is an unfinished prefix such as "ctrl+").
std::pair<QStringList, bool> shortcut_display_parts(const QString &value);
QString format_shortcut_text(const QString &value);

// An input drawing its shortcut as a key chip. Still a QLineEdit: the shortcut manager
// treats focus on a line edit as text input, so capture and pin shortcuts do not take
// the keys while recording.
class KeyChipLineEdit : public QLineEdit {
    Q_OBJECT
public:
    explicit KeyChipLineEdit(QWidget *parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    void setErrorState(bool error);
    bool errorState() const { return error_; }
protected:
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
private:
    QRectF clear_rect() const;
    static void paint_plus(QPainter &painter, const QPointF &center, const QColor &color);
    bool error_ = false;
    bool hover_clear_ = false;
};

// The status dot right of a key chip: available / unset / conflict; empty shows nothing.
class StatusIcon : public QWidget {
    Q_OBJECT
public:
    explicit StatusIcon(QWidget *parent = nullptr);
    QString state() const { return state_; }
    // state is "", "ok", "idle" or "error"; throws std::invalid_argument otherwise.
    void setState(const QString &state, const QString &tooltip = {});
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    QString state_;
};
}
