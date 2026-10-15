#include "ui/fluent_lite/text_context_menu.h"
#include "core/i18n.h"
#include "ui/fluent_lite/theme.h"
#include <QAction>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QStyle>
#include <QStyleFactory>
#include <QTextEdit>

namespace jietuba {
namespace {
void translate_standard_actions(QMenu *menu) {
    // Qt's built-in editor labels routed through the app translator.
    static const QHash<QString, const char *> labels = {
        {"edit-undo", "Undo"}, {"edit-redo", "Redo"}, {"edit-cut", "Cut"}, {"edit-copy", "Copy"},
        {"edit-paste", "Paste"}, {"edit-delete", "Delete"}, {"select-all", "Select All"},
    };
    for (auto *action : menu->actions()) {
        const auto found = labels.constFind(action->objectName());
        if (found == labels.constEnd()) continue;
        const auto text = action->text();
        const auto tab = text.indexOf('\t');
        const auto translated = i18n_tr(*found, "TextEditContextMenu");
        action->setText(tab >= 0 ? translated + text.mid(tab) : translated);
    }
}
}

QMenu *create_text_context_menu(QWidget *editor) {
    QMenu *menu = nullptr;
    if (auto *line = qobject_cast<QLineEdit *>(editor)) menu = line->createStandardContextMenu();
    else if (auto *text = qobject_cast<QTextEdit *>(editor)) menu = text->createStandardContextMenu();
    else if (auto *plain = qobject_cast<QPlainTextEdit *>(editor)) menu = plain->createStandardContextMenu();
    if (!menu) menu = new QMenu(editor);
    translate_standard_actions(menu);
    style_popup_menu(menu, ui_tokens(editor));
    return menu;
}

void style_popup_menu(QMenu *menu, const UIThemeTokens &tokens) {
    // Transient top-level menus may otherwise keep the OS dark popup renderer while the app is light.
    if (auto *fusion = QStyleFactory::create("Fusion")) {
        fusion->setParent(menu);
        menu->setStyle(fusion);
    }
    menu->setAttribute(Qt::WA_TranslucentBackground, false);
    menu->setAttribute(Qt::WA_StyledBackground, true);
    menu->setAutoFillBackground(true);
    menu->setPalette(UIThemeManager::build_palette(tokens));
    menu->setStyleSheet(QString(R"(
        QMenu {
            color: %1;
            background-color: %2;
            border: 1px solid %3;
            border-radius: 6px;
            padding: 4px;
        }
        QMenu::item {
            color: %1;
            background-color: transparent;
            padding: 6px 28px 6px 26px;
            margin: 1px 0;
        }
        QMenu::item:selected {
            color: %1;
            background-color: %4;
        }
        QMenu::item:disabled {
            color: %5;
            background-color: transparent;
        }
        QMenu::separator {
            height: 1px;
            background-color: %6;
            margin: 4px 7px;
        }
    )").arg(tokens.text, tokens.popup_background, tokens.border_hover, tokens.popup_hover, tokens.text_disabled, tokens.separator));
}

void show_text_context_menu(QWidget *editor, const QPoint &global_pos) {
    auto *menu = create_text_context_menu(editor);
    menu->exec(global_pos);
    menu->deleteLater();
}

void install_text_context_menu(QWidget *editor) {
    editor->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(editor, &QWidget::customContextMenuRequested, editor, [editor](const QPoint &pos) {
        show_text_context_menu(editor, editor->mapToGlobal(pos));
    });
}
}
