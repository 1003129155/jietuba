#include "clipboard/ui/widgets/quick_edit_popup.h"
#include "clipboard/ui/theme/theme_styles.h"
#include "clipboard/ui/widgets/preview_popup.h"
#include "core/ui_scale.h"
#include "settings/tool_settings.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/key_chip.h"
#include <QCoreApplication>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QTextCursor>
#include <QTimer>

namespace jietuba::clipboard {
namespace {
QString tr_quick_edit(const char *text) { return QCoreApplication::translate("ClipboardQuickEdit", text); }
}

QuickEditPopup::QuickEditPopup(QWidget *parent) : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint) {
    setAttribute(Qt::WA_StyledBackground, true);
    layout_ = new QVBoxLayout(this);

    editor = new TextEdit;
    editor->setAcceptRichText(false);
    editor->installEventFilter(this);
    layout_->addWidget(editor);

    footer_ = new QHBoxLayout;
    format_hint = new QLabel;
    footer_->addWidget(format_hint, 1);
    save_button = new QPushButton;
    save_and_paste_button = new QPushButton;
    save_and_paste_button->setObjectName("primary");
    for (auto *button : {save_button, save_and_paste_button}) {
        button->setFocusPolicy(Qt::NoFocus);
        button->setCursor(Qt::PointingHandCursor);
        footer_->addWidget(button);
    }
    layout_->addLayout(footer_);

    connect(save_button, &QPushButton::clicked, this, [this] { commit(false); });
    connect(save_and_paste_button, &QPushButton::clicked, this, [this] { commit(true); });
}

void QuickEditPopup::open_for(const ClipboardItem &item, const QPoint &anchor, const QRect &avoid_rect, const Theme &theme) {
    item_id_ = item.id;
    editor->setPlainText(item.content);
    editor->moveCursor(QTextCursor::End);

    // Read on each open, so keys changed in settings apply next time.
    bindings_ = load_inapp_bindings({SAVE_SHORTCUT, SAVE_AND_PASTE_SHORTCUT});
    auto *config = get_tool_settings_manager();
    save_button->setText(tr_quick_edit("Save"));
    save_button->setToolTip(format_shortcut_text(config->get_inapp_shortcut(SAVE_SHORTCUT)));
    save_and_paste_button->setText(tr_quick_edit("Save and Paste"));
    save_and_paste_button->setToolTip(format_shortcut_text(config->get_inapp_shortcut(SAVE_AND_PASTE_SHORTCUT)));
    // Changing the content drops the original formatting; the paste is plain text.
    format_hint->setText(item.html_content && !item.html_content->isEmpty() ? tr_quick_edit("Formatting will be removed") : QString());
    // The type selector names the C++ class (namespaces become "--").
    setStyleSheet(ThemeStyleGenerator(theme).generate_quick_edit_style().replace("QuickEditPopup {", "jietuba--clipboard--QuickEditPopup {"));
    // Sizes follow the toolbar and panel scale, laid out at the current scale on each open.
    const int margin = scaled(8);
    layout_->setContentsMargins(margin, margin, margin, margin);
    layout_->setSpacing(scaled(6));
    footer_->setSpacing(scaled(6));
    resize(scaled(420), scaled(240));

    move(side_position(size(), anchor, avoid_rect, "left"));
    show();
    raise();
    activateWindow();
    editor->setFocus();
}

void QuickEditPopup::commit(bool paste) { finish(editor->toPlainText(), paste); }

void QuickEditPopup::cancel() { finish(std::nullopt); }

void QuickEditPopup::close_for_focus_loss() { finish(editor->toPlainText(), false, true); }

void QuickEditPopup::finish(const std::optional<QString> &text, bool paste, bool focus_left) {
    if (!item_id_) return;
    const QuickEditResult result{*item_id_, text, paste, focus_left};
    item_id_.reset();
    hide();
    emit finished(result);
}

void QuickEditPopup::on_editor_focus_out() {
    // Focus back in the editor (a context menu closed, say) does not count as leaving.
    if (is_open() && !editor->hasFocus()) close_for_focus_loss();
}

bool QuickEditPopup::eventFilter(QObject *obj, QEvent *event) {
    if (obj == editor) {
        if (event->type() == QEvent::KeyPress) {
            auto *key_event = static_cast<QKeyEvent *>(event);
            if (match_inapp_binding(key_event, SAVE_AND_PASTE_SHORTCUT, bindings_)) {
                commit(true);
                return true;
            }
            if (match_inapp_binding(key_event, SAVE_SHORTCUT, bindings_)) {
                commit();
                return true;
            }
            if (key_event->key() == Qt::Key_Escape) {
                cancel();
                return true;
            }
        } else if (event->type() == QEvent::FocusOut && static_cast<QFocusEvent *>(event)->reason() != Qt::PopupFocusReason) {
            QTimer::singleShot(0, this, [this] { on_editor_focus_out(); });
        }
    }
    return QWidget::eventFilter(obj, event);
}
}
