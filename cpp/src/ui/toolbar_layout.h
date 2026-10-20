#pragma once
#include <QJsonArray>
#include <QJsonValue>
#include <QSet>
#include <QString>
#include <QStringList>
namespace jietuba {
class ToolSettingsManager;
// Capture and pin toolbars each keep their own layout (LayoutProfile) and share the read, write and
// normalization rules.
namespace toolbar_layout {
inline constexpr auto SETTING_KEY = "screenshot_toolbar_layout";
inline const QString SHOW = "show";  // always on the toolbar
inline const QString MORE = "more";  // folded into the "..." popup
inline const QString HIDE = "hide";  // always hidden
// Layout rules of one toolbar. order: every configurable button, also the default order; default_more:
// folded into "..." by default, the rest shown; locked: always shown and absent from the adjust dialog;
// setting_key: where the layout is stored.
struct LayoutProfile {
    QStringList order;
    QSet<QString> default_more;
    QSet<QString> locked;
    QString setting_key;
    QString default_mode(const QString &key) const { return default_more.contains(key) ? MORE : SHOW; }
};
// Confirm is always shown and not adjustable: a capture must always have a way to finish.
extern const LayoutProfile SCREENSHOT;
// Pins have none of the capture-only buttons (long screenshot, recording, cancel, pin, confirm) and add copy.
// The buttons shown by default and their order match the capture default, so a button is found in the same place.
extern const LayoutProfile PIN;
// Screen annotation: the drawing tools that need no captured image, plus pass-through, board, cursor
// highlight, clearing, hiding and leaving. Leaving is always shown, so the overlay can always be closed from the toolbar.
extern const LayoutProfile ANNOTATION;
const QStringList &setting_keys();
const QStringList &default_order();
QJsonArray default_layout(const LayoutProfile &profile = SCREENSHOT);
QJsonArray normalize_layout(const QJsonValue &entries, const LayoutProfile &profile = SCREENSHOT);
QJsonArray normalize_layout_json(const QByteArray &json, const LayoutProfile &profile = SCREENSHOT);
QJsonArray load_layout(const ToolSettingsManager &config, const LayoutProfile &profile = SCREENSHOT);
QJsonArray save_layout(ToolSettingsManager &config, const QJsonValue &entries, const LayoutProfile &profile = SCREENSHOT);
}
}
