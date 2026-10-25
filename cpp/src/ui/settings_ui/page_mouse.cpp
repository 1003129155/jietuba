#include "core/resource_manager.h"
#include "core/shortcut_manager.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "settings/tool_settings.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/segmented.h"
#include "ui/inapp_key_edit.h"
#include "ui/key_chip.h"
#include "ui/settings_ui/components.h"
#include "ui/settings_ui/dialog.h"
#include "ui/settings_ui/mouse_binding_editor.h"
#include "ui/settings_ui/pages.h"
#include <QStackedWidget>
#include <functional>
#include <map>

namespace jietuba {
namespace {
// Row icons shared with the keyboard page's in-app shortcuts, naming each row's action.
IconSource quick_capture_icon(const QString &action) {
    static const QMap<QString, IconSource> icons{
        {"pin", inapp_icon("inapp_pin")}, {"copy", inapp_icon("inapp_confirm")}, {"copy_pin", inapp_icon("inapp_copy_pin")},
        {"ocr", inapp_icon("inapp_text_recognize")}, {"translate", inapp_icon("inapp_translate")}, {"edit", FluentIcon::Edit},
        {"scan_code", QString::fromUtf8("扫码.svg")},
    };
    return icons.value(action);
}

IconSource capture_mouse_icon(const QString &action) {
    static const QMap<QString, IconSource> icons{
        {"copy", inapp_icon("inapp_confirm")}, {"pin", inapp_icon("inapp_pin")}, {"save", QString::fromUtf8("保存.svg")},
        {"quick_save", FluentIcon::Download}, {"close", FluentIcon::Close},
    };
    return icons.value(action);
}

IconSource pin_mouse_icon(const QString &action) {
    // The toolbar's close icon has a red disc; as a tinted row icon the disc and its white
    // cross would merge into one shape, so the plain cross is used.
    static const QMap<QString, IconSource> icons{
        {"zoom", FluentIcon::Search}, {"fine_zoom", FluentIcon::Search}, {"opacity", FluentIcon::Transparent}, {"close", FluentIcon::Close},
        {"reset", inapp_icon("inapp_pin_reset_size")}, {"thumbnail", inapp_icon("inapp_thumbnail")}, {"region", QString::fromUtf8("选择.svg")},
        {"copy_text", inapp_icon("inapp_copy_pin_text")},
    };
    return icons.value(action);
}

IconSource clipboard_mouse_icon(const QString &action) {
    static const QMap<QString, IconSource> icons{{"paste", FluentIcon::Paste}, {"pin", FluentIcon::Pin}, {"quick_edit", FluentIcon::Edit}, {"menu", FluentIcon::Layout}};
    return icons.value(action);
}

const QStringList INAPP_MODIFIERS = {"", "ctrl", "shift", "alt", "ctrl+shift", "ctrl+alt", "shift+alt", "ctrl+shift+alt"};
// Global gestures need a modifier. Win combinations come first: Ctrl / Shift / Alt alone
// plus a drag are common operations and easy to trigger by accident.
const QStringList GLOBAL_MODIFIERS = {"", "win", "ctrl+win", "shift+win", "alt+win", "ctrl+alt", "ctrl+shift", "shift+alt", "ctrl", "shift", "alt"};

using Gestures = std::vector<std::pair<QString, const char *>>;
const std::map<QString, Gestures> &MOUSE_GESTURES() {
    static const std::map<QString, Gestures> gestures{
        {"wheel", {{"wheel", "Mouse Wheel"}}},
        {"drag", {{"dragleft", "Left Drag"}, {"dragmiddle", "Middle Drag"}, {"dragright", "Right Drag"}}},
        {"click",
         {{"left", "Left Click"},
          {"middle", "Middle Click"},
          {"right", "Right Click"},
          {"doubleleft", "Left Double-click"},
          {"doublemiddle", "Middle Double-click"},
          {"doubleright", "Right Double-click"}}},
        // The capture canvas's left click does selection and annotation, so it is not offered;
        // right has no double click, or a single click would wait for the double-click timeout.
        {"capture", {{"middle", "Middle Click"}, {"right", "Right Click"}, {"doubleleft", "Left Double-click"}}},
        {"global",
         {{"dragleft", "Left Drag"},
          {"dragmiddle", "Middle Drag"},
          {"dragright", "Right Drag"},
          {"dragx1", "Back Button Drag"},
          {"dragx2", "Forward Button Drag"}}},
    };
    return gestures;
}

const char *gesture_label(const QString &value) {
    for (const auto &[kind, gestures] : MOUSE_GESTURES())
        for (const auto &[gesture, label] : gestures)
            if (gesture == value) return label;
    return nullptr;
}

// Python str.title(): each run of letters starts upper case.
QString title_case(const QString &text) {
    QString result = text;
    bool start = true;
    for (auto &c : result) {
        if (c.isLetter()) {
            c = start ? c.toUpper() : c.toLower();
            start = false;
        } else {
            start = true;
        }
    }
    return result;
}

QWidget *build_mouse_rows(SettingsDialog *dialog, const std::vector<MouseAction> &actions, const QString &key_prefix, IconSource (*action_icons)(const QString &),
                          QString (*binding_getter)(const ToolSettingsManager *, const QString &)) {
    auto *page = new QWidget;
    auto *page_layout = new QVBoxLayout(page);
    page_layout->setContentsMargins(0, 0, 0, 0);
    page_layout->setSpacing(0);
    for (const auto &action : actions) {
        auto *card = new QWidget(page);
        auto *row = new QHBoxLayout(card);
        row->setContentsMargins(0, dialog_scaled(8), 0, dialog_scaled(8));
        row->setSpacing(dialog_scaled(14));
        const auto icon = action_icons(action.key);
        row->addWidget(new IconBadge(std::holds_alternative<std::monostate>(icon) ? icon : icon_ref(icon), QString(), card));
        auto *title = row_label(card, dialog->tr_text(action.label));
        title->setWordWrap(true);
        row->addWidget(title, 1);
        auto *editor = new MouseBindingEditor(action.kind, card);
        editor->setFixedWidth(dialog_scaled(320));
        editor->setBinding(binding_getter(dialog->config_manager, action.key));
        dialog->behavior_controls.emplace_back(key_prefix + action.key, editor);
        row->addWidget(editor);
        add_separated_row(page_layout, card);
    }
    page_layout->addStretch(1);
    return page;
}

QWidget *create_global_section(SettingsDialog *dialog, QWidget *parent) {
    auto *group = new SectionCard(ResourceManager::get_icon_path(QString::fromUtf8("鼠标线框.svg")), dialog->tr_text("Global Mouse Shortcuts"),
                                  dialog->tr_text("Hold modifier keys and drag the mouse"), parent);
    group->addWidget(build_mouse_rows(dialog, QUICK_CAPTURE_ACTIONS(), "quick_capture_", quick_capture_icon, get_quick_capture_binding));
    return group;
}

QWidget *create_inapp_section(SettingsDialog *dialog, QWidget *parent) {
    auto *group = new SectionCard(FluentIcon::Layout, dialog->tr_text("In-App Mouse Shortcuts"), dialog->tr_text("Only in screenshot, pin and clipboard windows"), parent);

    auto *tab_switch = new SegmentedWidget(group);
    tab_switch->setFixedHeight(dialog_scaled(34));
    tab_switch->setIndicatorColor(ACCENT, ACCENT);

    auto *stack = new QStackedWidget(group);
    stack->setObjectName("MouseShortcutStack");
    stack->setStyleSheet("#MouseShortcutStack { background: transparent; border: none; }");
    struct Tab {
        QString route;
        const char *title;
        const std::vector<MouseAction> &actions;
        QString prefix;
        IconSource (*icons)(const QString &);
        QString (*getter)(const ToolSettingsManager *, const QString &);
    };
    std::vector<Tab> tabs{
        {"screenshot", "Screenshot Shortcuts", CAPTURE_MOUSE_ACTIONS(), "mouse_capture_", capture_mouse_icon, get_capture_mouse_binding},
        {"pin", "Pin Shortcuts", PIN_MOUSE_ACTIONS(), "mouse_pin_", pin_mouse_icon, get_pin_mouse_binding},
    };
    tabs.push_back({"clipboard", "Clipboard Shortcuts", CLIPBOARD_MOUSE_ACTIONS(), "mouse_clipboard_", clipboard_mouse_icon, get_clipboard_mouse_binding});
    QStringList routes;
    for (const auto &tab : tabs) {
        routes.append(tab.route);
        stack->addWidget(build_mouse_rows(dialog, tab.actions, tab.prefix, tab.icons, tab.getter));
        tab_switch->addItem(tab.route, dialog->tr_text(tab.title));
    }
    QObject::connect(tab_switch, &SegmentedWidget::currentItemChanged, stack, [stack, routes](const QString &route) { stack->setCurrentIndex(routes.indexOf(route)); });
    tab_switch->setCurrentItem("screenshot");
    // The pin page's "Edit" button switches to the pin tab here.
    dialog->mouse_inapp_tabs = tab_switch;

    auto *tab_row = new QWidget(group);
    auto *tab_row_layout = new QHBoxLayout(tab_row);
    tab_row_layout->setContentsMargins(0, 0, 0, dialog_scaled(10));
    tab_row_layout->addWidget(tab_switch);
    tab_row_layout->addStretch(1);
    group->addWidget(tab_row);
    group->addWidget(stack);
    return group;
}

QString normalized_mouse_binding(const QString &binding) {
    QStringList parts;
    for (const auto &part : binding.toLower().split('+'))
        if (!part.isEmpty()) parts.append(part == "mousemiddle" ? "middle" : part);
    return parts.join("+");
}

// A mouse binding as the settings page names it.
QString mouse_binding_text(SettingsDialog *dialog, const QString &binding) {
    QStringList parts = normalized_mouse_binding(binding).split('+');
    const QString gesture = parts.takeLast();
    const QString modifier_text = format_shortcut_text(parts.join("+"));
    const char *label = gesture_label(gesture);
    const QString gesture_text = label ? dialog->tr_text(label) : dialog->tr_text(gesture.toUtf8().constData());
    return modifier_text.isEmpty() ? gesture_text : modifier_text + " + " + gesture_text;
}
}

MouseBindingEditor::MouseBindingEditor(const QString &kind, QWidget *parent) : QWidget(parent), modifier_required_(kind == "global") {
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(dialog_scaled(8));
    modifiers = new ComboBox(this);
    for (const auto &value : kind == "global" ? GLOBAL_MODIFIERS : INAPP_MODIFIERS) {
        const QString label = !value.isEmpty() ? title_case(value) : (kind == "global" ? QString::fromUtf8("—") : settings_tr("No Modifier"));
        modifiers->addItem(label, value);
    }
    gesture = new ComboBox(this);
    gesture->addItem(settings_tr("No Action"), QString());
    for (const auto &[value, label] : MOUSE_GESTURES().at(kind)) gesture->addItem(settings_tr(label), value);
    auto *plus = new QLabel("+", this);
    plus->setAlignment(Qt::AlignCenter);
    apply_theme_text_style(plus, 14, false, {}, true);
    // Both sides share the spare width, so the plus sign sits in the same column on every row.
    layout->addWidget(modifiers, 1);
    layout->addWidget(plus);
    layout->addWidget(gesture, 1);
    if (modifier_required_) {
        connect(gesture, &QComboBox::currentIndexChanged, this, &MouseBindingEditor::pair_modifiers);
        connect(modifiers, &QComboBox::currentIndexChanged, this, &MouseBindingEditor::pair_gesture);
    }
}

void MouseBindingEditor::pair_modifiers() {
    const int index = gesture->currentData().toString().isEmpty() ? 0 : 1;
    if (modifiers->currentData().toString().isEmpty() == (index == 0)) return;
    modifiers->setCurrentIndex(index);
}

void MouseBindingEditor::pair_gesture() {
    if (modifiers->currentData().toString().isEmpty() && gesture->currentIndex()) gesture->setCurrentIndex(0);
}

QString MouseBindingEditor::currentData() const {
    const QString gesture_value = gesture->currentData().toString();
    const QString modifier_value = modifiers->currentData().toString();
    return !gesture_value.isEmpty() && !modifier_value.isEmpty() ? modifier_value + "+" + gesture_value : gesture_value;
}

void MouseBindingEditor::setBinding(const QString &binding) {
    const QStringList parts = binding.toLower().split('+');
    const QStringList held = parts.mid(0, parts.size() - 1);
    QStringList ordered;
    for (const auto &key : QUICK_CAPTURE_MODIFIERS)
        if (held.contains(key)) ordered.append(key);
    int modifier_index = modifiers->findData(ordered.join("+"));
    int gesture_index = gesture->findData(parts.last());
    // Spellings missing from the options show as unset, never as another gesture.
    if (modifier_index < 0 || gesture_index < 0 || (modifier_required_ && (modifier_index != 0) != (gesture_index != 0))) modifier_index = gesture_index = 0;
    modifiers->setCurrentIndex(modifier_index);
    gesture->setCurrentIndex(gesture_index);
}

QWidget *create_mouse_page(SettingsDialog *dialog) {
    auto *scroll = page_scroll_area(dialog);
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet("QScrollArea { border: none; background: transparent; }");
    auto *view = new QWidget;
    set_own_style(view, "background: transparent;");
    auto *layout = new QVBoxLayout(view);
    layout->setContentsMargins(0, 0, dialog_scaled(10), 0);
    layout->setSpacing(dialog_scaled(12));
    layout->addWidget(create_global_section(dialog, view));
    layout->addWidget(create_inapp_section(dialog, view));
    layout->addStretch();
    scroll->setWidget(view);
    return scroll;
}

std::vector<std::tuple<QString, QString, QStringList>> mouse_binding_conflicts(SettingsDialog *dialog) {
    std::vector<std::tuple<QString, QString, QStringList>> conflicts;
    struct Domain {
        QString prefix;
        QString inapp_group;
        const char *context_source;
        const std::vector<MouseAction> &actions;
    };
    const std::vector<Domain> domains{
        {"quick_capture_", "", "Global Mouse Shortcuts", QUICK_CAPTURE_ACTIONS()},
        {"mouse_capture_", "screenshot", "Screenshot Shortcuts", CAPTURE_MOUSE_ACTIONS()},
        {"mouse_pin_", "pin", "Pin Shortcuts", PIN_MOUSE_ACTIONS()},
        {"mouse_clipboard_", "clipboard", "Clipboard Shortcuts", CLIPBOARD_MOUSE_ACTIONS()},
    };
    for (const auto &domain : domains) {
        // Bindings in first-seen order, with their owners in the page's row order.
        std::vector<std::pair<QString, QStringList>> owners_by_binding;
        std::map<QString, QSet<QString>> actions_by_binding;
        const auto owners_of = [&owners_by_binding](const QString &binding) -> QStringList & {
            for (auto &entry : owners_by_binding)
                if (entry.first == binding) return entry.second;
            owners_by_binding.emplace_back(binding, QStringList());
            return owners_by_binding.back().second;
        };
        for (const auto &action : domain.actions) {
            auto *control = qobject_cast<MouseBindingEditor *>(dialog->behavior_control(domain.prefix + action.key));
            if (!control) continue;
            const QString binding = normalized_mouse_binding(control->currentData());
            if (binding.isEmpty()) continue;
            actions_by_binding[binding].insert(action.key);
            owners_of(binding).append(QString("%1 (%2)").arg(dialog->tr_text(action.label), dialog->tr_text("Mouse Shortcuts")));
        }
        for (const auto &[key, edit] : dialog->inapp_edits) {
            // The global domain has no in-app group (None in the reference), so nothing joins it.
            if (domain.inapp_group.isEmpty() || dialog->inapp_groups.value(key) != domain.inapp_group) continue;
            const QString raw_binding = edit->text();
            const QString binding = normalized_mouse_binding(raw_binding);
            if (is_inapp_mouse_shortcut(raw_binding) && !binding.isEmpty())
                owners_of(binding).append(QString("%1 (%2)").arg(inapp_label(dialog, key), dialog->tr_text("In-App Shortcuts")));
        }
        for (const auto &[binding, owners] : owners_by_binding) {
            // Pin applies only to images and quick edit only to text, so they may share a gesture.
            if (domain.prefix == "mouse_clipboard_" && owners.size() == 2 && actions_by_binding[binding] == QSet<QString>{"pin", "quick_edit"}) continue;
            if (owners.size() > 1) conflicts.emplace_back(dialog->tr_text(domain.context_source), mouse_binding_text(dialog, binding), owners);
        }
    }
    return conflicts;
}

QString mouse_binding_conflict_message(SettingsDialog *dialog, const std::optional<std::vector<std::tuple<QString, QString, QStringList>>> &given) {
    const auto conflicts = given ? *given : mouse_binding_conflicts(dialog);
    QStringList lines{dialog->tr_text("The following shortcuts conflict:"), ""};
    for (const auto &[context, binding, owners] : conflicts) lines.append(QString::fromUtf8("• %1 · %2: %3").arg(context, binding, owners.join(" / ")));
    lines.append("");
    lines.append(dialog->tr_text("The new shortcut was not saved."));
    return lines.join("\n");
}
}
