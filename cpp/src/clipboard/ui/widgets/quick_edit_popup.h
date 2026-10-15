#pragma once
#include "clipboard/core/models.h"
#include "clipboard/ui/theme/themes.h"
#include "core/shortcut_manager.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>
#include <optional>

namespace jietuba {
class TextEdit;
}

namespace jietuba::clipboard {
inline const QString SAVE_SHORTCUT = "inapp_clipboard_edit_save";
inline const QString SAVE_AND_PASTE_SHORTCUT = "inapp_clipboard_edit_save_paste";

struct QuickEditResult {
    qint64 item_id = 0;
    // nullopt means cancelled.
    std::optional<QString> text;
    bool paste = false;
    // Ended because focus left; the window must not take focus back.
    bool focus_left = false;
};

// Edits one text item in a small window owned by the clipboard window (whose hide-on-
// deactivate rule counts it as its own). Saving and pasting are the window's job;
// buttons never take focus, so focus leaving the editor means a click elsewhere.
class QuickEditPopup : public QWidget {
    Q_OBJECT
public:
    explicit QuickEditPopup(QWidget *parent);
    bool is_open() const { return item_id_.has_value(); }
    void open_for(const ClipboardItem &item, const QPoint &anchor, const QRect &avoid_rect, const Theme &theme);
    void commit(bool paste = false);
    void cancel();
    void close_for_focus_loss();

    TextEdit *editor = nullptr;
    QLabel *format_hint = nullptr;
    QPushButton *save_button = nullptr;
    QPushButton *save_and_paste_button = nullptr;

signals:
    void finished(const jietuba::clipboard::QuickEditResult &result);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void finish(const std::optional<QString> &text, bool paste = false, bool focus_left = false);
    void on_editor_focus_out();
    std::optional<qint64> item_id_;
    QHash<QString, KeyBinding> bindings_;
    QVBoxLayout *layout_ = nullptr;
    QHBoxLayout *footer_ = nullptr;
};
}

Q_DECLARE_METATYPE(jietuba::clipboard::QuickEditResult)
