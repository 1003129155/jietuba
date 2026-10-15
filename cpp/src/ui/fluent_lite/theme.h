#pragma once
#include "core/ui_theme.h"
#include "ui/fluent_lite/icons.h"
#include <QColor>
#include <QIcon>
#include <QString>
#include <QStringList>
#include <optional>
#include <variant>

class QWidget;

namespace jietuba {
inline const QString FONT_FAMILY = R"("Segoe UI Variable", "Microsoft YaHei UI", "Segoe UI", sans-serif)";
// The same stack for QFont::setFamilies.
const QStringList &FONT_FAMILIES();
// One restrained blue-grey accent so dialogs do not drift to unrelated blues.
inline const QString ACCENT = "#6F8FAB";
inline const QString ACCENT_HOVER = "#627F99";
inline const QString ACCENT_PRESSED = "#526D85";
inline const QString ACCENT_SOFT = "#DFE8EF";
inline const QString ACCENT_SUBTLE = "rgba(111, 143, 171, 0.18)";
inline const QString FOCUS_RING = "rgba(111, 143, 171, 0.24)";
// Frame metrics of input controls; widgets aligned with them (such as colour
// swatch buttons in the same column) take these values too.
constexpr int INPUT_RADIUS = 10;
constexpr int INPUT_CONTENT_HEIGHT = 26;
constexpr int INPUT_PADDING_V = 4;
constexpr int INPUT_PADDING_H = 11;
constexpr int INPUT_HEIGHT = INPUT_CONTENT_HEIGHT + 2 * INPUT_PADDING_V + 2;

// Semantic colours for widget.
const UIThemeTokens &ui_tokens(const QWidget *widget = nullptr);
// The shared thin scrollbar of application windows.
QString scrollbar_qss(const QWidget *widget = nullptr);

// An icon given as a QIcon, a FluentIcon or an SVG path.
using IconSource = std::variant<std::monostate, QIcon, FluentIcon, QString>;
// SVG-backed icons are tinted with the theme's text colour; *White icons keep their colour.
QIcon to_qicon(const IconSource &icon, const QWidget *widget = nullptr);
// Parses token colours for QPainter; QColor rejects rgb()/rgba().
QColor css_color(const QString &value);
QIcon tinted_icon(const IconSource &icon, const QString &color);
// Opaque rounded background with a subtle vertical gradient for frameless dialogs.
void paint_solid_background(QWidget *widget, int radius = 8);
}
