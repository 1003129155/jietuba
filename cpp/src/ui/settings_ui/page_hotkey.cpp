#include "core/resource_manager.h"
#include "core/shortcut_manager.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "settings/tool_settings.h"
#include "ui/dialogs.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/segmented.h"
#include "ui/hotkey_edit.h"
#include "ui/inapp_key_edit.h"
#include "ui/key_chip.h"
#include "ui/settings_ui/components.h"
#include "ui/settings_ui/dialog.h"
#include "ui/settings_ui/pages.h"
#include "ui/settings_ui/search.h"
#include <QGridLayout>
#include <QStackedWidget>
#include <functional>

namespace jietuba {
namespace {
const std::vector<InAppKey> SCREENSHOT_KEYS = {
    {"inapp_confirm", "Copy to Clipboard", "ctrl+c"},
    {"inapp_pin", "Pin to Screen", "mousemiddle"},
    {"inapp_delete", "Delete Selected", "delete"},
    {"inapp_restore_last_region", "Restore Last Region", "l"},
    {"inapp_zoom_in", "Magnifier Zoom In", "pageup"},
    {"inapp_zoom_out", "Magnifier Zoom Out", "pagedown"},
    {"inapp_translate", "Screenshot Translate", "shift+c"},
    {"inapp_text_recognize", "Recognize Text", "shift+t"},
};

const std::vector<InAppKey> PIN_KEYS = {
    {"inapp_copy_pin", "Copy Pinned Image", "ctrl+c"},
    {"inapp_copy_pin_text", "Copy All Text", "ctrl+shift+c"},
    {"inapp_pin_reset_size", "Reset Size", ""},
    {"inapp_thumbnail", "Toggle Thumbnail", "r"},
    {"inapp_toggle_toolbar", "Toggle Toolbar", "space"},
    {"inapp_pin_hide_annotations", "Hide Annotations", "h"},
    {"inapp_pin_lock", "Lock / Unlock Pin", "l"},
    {"inapp_pin_text_mode", "Toggle Text Mode", "t"},
    {"inapp_pin_text_selection", "Toggle Text Selection", "p"},
    // While editing a plain key is typed; Esc stops editing there.
    {"inapp_pin_text_edit", "Edit Text", "enter"},
};

const std::vector<InAppKey> &TOOL_KEYS() {
    static const auto keys = [] {
        std::vector<InAppKey> result;
        for (const auto &tool : ANNOTATION_TOOL_SHORTCUTS()) result.push_back({tool.config_key, tool.label, tool.default_key});
        result.push_back({"inapp_undo", "Undo", "ctrl+z"});
        result.push_back({"inapp_redo", "Redo", "ctrl+y"});
        return result;
    }();
    return keys;
}

const std::vector<InAppKey> CLIPBOARD_KEYS = {
    {"inapp_clipboard_quick_edit", "Quick Edit", "tab"},
};

// Active in the quick edit box's text field, so they must not take typing keys.
const std::vector<InAppKey> CLIPBOARD_EDITOR_KEYS = {
    {"inapp_clipboard_edit_save", "Save Edit", "ctrl+enter"},
    {"inapp_clipboard_edit_save_paste", "Save and Paste", "ctrl+shift+enter"},
};

// Pressed again in the search box it leaves it, so it must not take typing keys either.
const std::vector<InAppKey> CLIPBOARD_SEARCH_KEYS = {
    {"inapp_clipboard_search", "Search", "ctrl+f"},
};

const QString CURSOR_MOVE_ICON = QString::fromUtf8("移动窗口.svg");

// (function, attribute of the slot 1 editor, title, tone, icon); slot n's attribute adds _n, like the setting.
struct GlobalRow {
    QString function, first_attr;
    const char *title;
    QString tone;
    FluentIcon icon;
};

const std::vector<GlobalRow> &GLOBAL_HOTKEY_ROWS() {
    static const std::vector<GlobalRow> rows = {
        {"screenshot", "hotkey_input", "Screenshot Hotkey", "capture", FluentIcon::Camera},
        {"clipboard", "clipboard_hotkey_edit", "Clipboard Hotkey", "clipboard", FluentIcon::Paste},
        {"pin_clipboard", "pin_clipboard_hotkey_edit", "Pin Clipboard Image", "pin", FluentIcon::Pin},
        {"translation", "translation_hotkey_edit", "Translation Hotkey", "translate", FluentIcon::Language},
        {"screen_annotation", "screen_annotation_hotkey_edit", "Screen Annotation Hotkey", "capture", FluentIcon::Brush},
    };
    return rows;
}

constexpr int INAPP_ROW_HEIGHT = 44;
// In-app chips line up with the global ones, which have a status icon to their right.
constexpr int STATUS_COLUMN = STATUS_GAP + STATUS_SIZE;

std::vector<HotkeyEdit *> global_hotkey_edits(SettingsDialog *dialog) {
    std::vector<HotkeyEdit *> edits;
    for (const auto &attr : GLOBAL_HOTKEY_EDIT_ATTRS)
        if (auto *edit = dialog->attr<HotkeyEdit>(attr)) edits.push_back(edit);
    return edits;
}

// The effective values of the global hotkey editors; empty values and unfinished prefixes (such as "ctrl+") do not count.
QSet<QString> global_hotkey_values(SettingsDialog *dialog) {
    QSet<QString> values;
    for (auto *edit : global_hotkey_edits(dialog)) {
        const QString text = edit->text().trimmed().toLower();
        if (!text.isEmpty() && !text.endsWith("+")) values.insert(text);
    }
    return values;
}

// Marks in-app shortcuts that equal a global hotkey. RegisterHotKey intercepts the key
// before it reaches any window, so such a shortcut never fires; this only hints and
// changes no saved value.
void refresh_inapp_shadow_states(SettingsDialog *dialog) {
    if (dialog->inapp_edits.empty()) return;
    const auto globals = global_hotkey_values(dialog);
    const QString tip = dialog->tr_text("This is the same as a global hotkey — the system intercepts it first, so it won't work here.");
    for (const auto &[key, edit] : dialog->inapp_edits) {
        const QString text = edit->text().trimmed().toLower();
        const bool shadowed = !text.isEmpty() && globals.contains(text);
        edit->setErrorState(shadowed);
        edit->setToolTip(shadowed ? tip : QString());
    }
}

QWidget *global_row(QWidget *parent, const QString &title, const QString &tone, FluentIcon icon, const std::vector<HotkeyEdit *> &editors) {
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, dialog_scaled(8), 0, dialog_scaled(8));
    layout->setSpacing(dialog_scaled(14));
    layout->addWidget(new IconBadge(icon, tone, row, 28, 16));
    layout->addWidget(row_label(row, title), 1);
    // Column by column: the first two slots keep their old column, the added ones go to its right.
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(dialog_scaled(10));
    grid->setVerticalSpacing(dialog_scaled(5));
    for (int slot = 0; slot < static_cast<int>(editors.size()); ++slot) {
        editors[slot]->setFixedWidth(dialog_scaled(CHIP_WIDTH + STATUS_COLUMN));
        grid->addWidget(editors[slot], slot % 2, slot / 2);
    }
    layout->addLayout(grid);
    return row;
}

QWidget *inapp_row(QWidget *parent, const IconSource &icon, const QString &title, QWidget *editor) {
    auto *row = new QWidget(parent);
    row->setFixedHeight(dialog_scaled(INAPP_ROW_HEIGHT));
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(dialog_scaled(14));
    layout->addWidget(new IconBadge(std::holds_alternative<std::monostate>(icon) ? icon : icon_ref(icon), QString(), row));
    layout->addWidget(row_label(row, title), 1);
    // The width sits on an outer slot: ComboBox resets its minimum width on every restyle,
    // so a fixed width on it would shrink back to the content.
    auto *slot = new QWidget(row);
    slot->setFixedWidth(dialog_scaled(CHIP_WIDTH));
    auto *slot_layout = new QHBoxLayout(slot);
    slot_layout->setContentsMargins(0, 0, 0, 0);
    slot_layout->addWidget(editor);
    layout->addWidget(slot, 0, Qt::AlignVCenter);
    // No spacing is inserted between a widget and a spacer, so this is the whole gap.
    layout->addSpacing(dialog_scaled(STATUS_COLUMN));
    return row;
}

QString reserved_free(const QString &value) { return is_reserved_inapp_shortcut(value) ? QString() : value; }

bool ask_replace(SettingsDialog *dialog, const QString &key_text, const QString &owner_label) {
    return show_confirm_dialog(dialog, dialog->tr_text("Shortcut Conflict"),
                               dialog->tr_text("\"%1\" is already used by \"%2\".\nReplace it?").replace("%1", format_shortcut_text(key_text)).replace("%2", owner_label));
}

// The character a key would give clipboard direct pick ("e", "shift+e", "3"), else empty.
// Direct pick only yields to Ctrl/Alt/Win, so Shift plus a letter is taken as well.
QString pick_char_of(const QString &text) {
    QStringList parts = text.trimmed().toLower().split('+');
    const QString main = parts.takeLast();
    for (const auto &modifier : parts)
        if (modifier != "shift") return {};
    return main.size() == 1 && clipboard_pick_keys("both").contains(main) ? main : QString();
}

void set_pick_mode(SettingsDialog *dialog, const QString &mode) {
    auto *combo = dialog->attr<QComboBox>("clipboard_pick_combo");
    combo->blockSignals(true);
    combo->setCurrentIndex(combo->findData(mode));
    combo->blockSignals(false);
    dialog->clipboard_pick_mode = mode;
}

// A clipboard shortcut takes a current direct pick key: replacing switches direct pick to
// the set without it, otherwise the input is undone.
void check_pick_conflict(SettingsDialog *dialog, const QString &changed_key, const QString &new_text) {
    const QString character = pick_char_of(new_text);
    if (character.isEmpty() || !clipboard_pick_keys(dialog->clipboard_pick_mode).contains(character)) return;
    if (ask_replace(dialog, new_text, dialog->tr_text("Direct Pick Keys"))) {
        set_pick_mode(dialog, character[0].isDigit() ? "letters" : "digits");
        return;
    }
    auto *edit = dialog->inapp_edit(changed_key);
    edit->blockSignals(true);
    edit->setText(reserved_free(dialog->config_manager->get_inapp_shortcut(changed_key)));
    edit->blockSignals(false);
}

// A new value that duplicates another shortcut in its group asks whether to replace it.
void on_shortcut_changed(SettingsDialog *dialog, const QString &changed_key, const QString &value) {
    if (dialog->batch_editing()) return;
    const QString new_text = value.trimmed().toLower();
    // Empty values and unfinished prefixes ("ctrl+") are ignored.
    if (new_text.isEmpty() || new_text.endsWith("+")) return;
    const QString my_group = dialog->inapp_groups.value(changed_key);
    QString conflict_key;
    for (const auto &[cfg_key, edit] : dialog->inapp_edits) {
        if (cfg_key == changed_key || dialog->inapp_groups.value(cfg_key) != my_group) continue;
        if (edit->text().trimmed().toLower() == new_text) {
            conflict_key = cfg_key;
            break;
        }
    }
    if (conflict_key.isEmpty()) {
        if (my_group == "clipboard") check_pick_conflict(dialog, changed_key, new_text);
        return;
    }
    const QString conflict_label = inapp_label(dialog, conflict_key);
    auto *current_edit = dialog->inapp_edit(changed_key);
    auto *conflict_edit = dialog->inapp_edit(conflict_key);
    // Signals blocked against recursion.
    current_edit->blockSignals(true);
    conflict_edit->blockSignals(true);
    if (ask_replace(dialog, new_text, conflict_label)) conflict_edit->setText("");
    else current_edit->setText(reserved_free(dialog->config_manager->get_inapp_shortcut(changed_key)));
    current_edit->blockSignals(false);
    conflict_edit->blockSignals(false);
}

// After switching the direct pick keys, checks each clipboard shortcut against them.
void on_pick_mode_changed(SettingsDialog *dialog) {
    const QString mode = dialog->attr<QComboBox>("clipboard_pick_combo")->currentData().toString();
    if (dialog->batch_editing()) {
        dialog->clipboard_pick_mode = mode;
        return;
    }
    const QString keys = clipboard_pick_keys(mode);
    for (const auto &[cfg_key, edit] : dialog->inapp_edits) {
        if (dialog->inapp_groups.value(cfg_key) != "clipboard") continue;
        const QString character = pick_char_of(edit->text());
        if (character.isEmpty() || !keys.contains(character)) continue;
        if (!ask_replace(dialog, edit->text(), inapp_label(dialog, cfg_key))) {
            set_pick_mode(dialog, dialog->clipboard_pick_mode);
            return;
        }
        edit->blockSignals(true);
        edit->setText("");
        edit->blockSignals(false);
    }
    dialog->clipboard_pick_mode = mode;
}

struct GlobalSlot {
    QString function;
    QString label;
    int slot = 0;
};

std::optional<GlobalSlot> global_slot_of(SettingsDialog *dialog, HotkeyEdit *edit) {
    for (const auto &row : GLOBAL_HOTKEY_ROWS()) {
        const auto attrs = global_hotkey_edit_attrs(row.first_attr);
        for (int slot = 0; slot < attrs.size(); ++slot)
            if (dialog->attr<HotkeyEdit>(attrs[slot]) == edit) {
                const QString title = dialog->tr_text(row.title);
                return GlobalSlot{row.function, slot ? QString("%1 (%2)").arg(title).arg(slot + 1) : title, slot};
            }
    }
    return std::nullopt;
}

// A finished hotkey that another global hotkey already uses asks whether to move it here;
// declining puts back the saved value.
void on_global_hotkey_changed(SettingsDialog *dialog, HotkeyEdit *edit) {
    if (dialog->batch_editing()) return;
    const QString text = edit->text().trimmed();
    if (text.isEmpty() || text.endsWith("+")) return;
    const auto identity = hotkey_identity(text);
    const auto mine = global_slot_of(dialog, edit);
    if (!identity || !mine) return;
    const QString saved = dialog->config_manager->get_global_hotkeys(mine->function).value(mine->slot);
    if (hotkey_identity(saved) == identity) return;
    for (auto *other : global_hotkey_edits(dialog)) {
        if (other == edit || hotkey_identity(other->text()) != identity) continue;
        const auto owner = global_slot_of(dialog, other);
        if (ask_replace(dialog, text, owner ? owner->label : QString())) other->setText("");
        else edit->setText(saved);
        return;
    }
}
}

QStringList global_hotkey_edit_attrs(const QString &first_attr) {
    QStringList attrs;
    for (int slot = 0; slot < GLOBAL_HOTKEY_SLOTS; ++slot) attrs.append(first_attr + global_hotkey_slot_suffix(slot));
    return attrs;
}

const QStringList GLOBAL_HOTKEY_EDIT_ATTRS = [] {
    QStringList attrs;
    for (const auto &row : GLOBAL_HOTKEY_ROWS()) attrs += global_hotkey_edit_attrs(row.first_attr);
    return attrs;
}();

std::vector<std::pair<QString, std::vector<HotkeyEdit *>>> global_hotkey_edit_rows(SettingsDialog *dialog) {
    std::vector<std::pair<QString, std::vector<HotkeyEdit *>>> rows;
    for (const auto &row : GLOBAL_HOTKEY_ROWS()) {
        std::vector<HotkeyEdit *> edits;
        for (const auto &attr : global_hotkey_edit_attrs(row.first_attr)) edits.push_back(dialog->attr<HotkeyEdit>(attr));
        rows.emplace_back(row.function, edits);
    }
    return rows;
}

const std::vector<InAppKey> &INAPP_KEYS() {
    static const auto keys = [] {
        std::vector<InAppKey> result = SCREENSHOT_KEYS;
        result.insert(result.end(), TOOL_KEYS().begin(), TOOL_KEYS().end());
        result.insert(result.end(), PIN_KEYS.begin(), PIN_KEYS.end());
        result.insert(result.end(), CLIPBOARD_KEYS.begin(), CLIPBOARD_KEYS.end());
        result.insert(result.end(), CLIPBOARD_EDITOR_KEYS.begin(), CLIPBOARD_EDITOR_KEYS.end());
        result.insert(result.end(), CLIPBOARD_SEARCH_KEYS.begin(), CLIPBOARD_SEARCH_KEYS.end());
        return result;
    }();
    return keys;
}

IconSource inapp_icon(const QString &cfg_key) {
    // The same icons as the toolbar where possible. Row icons are tinted as a whole, and the
    // toolbar's number icon is a white disc that would tint to a solid dot, so the outline one is used.
    static const QMap<QString, IconSource> icons = {
        {"inapp_confirm", QString::fromUtf8("确定.svg")},
        {"inapp_pin", QString::fromUtf8("钉图.svg")},
        {"inapp_undo", QString::fromUtf8("撤回.svg")},
        {"inapp_redo", QString::fromUtf8("复原.svg")},
        {"inapp_delete", FluentIcon::Delete},
        {"inapp_restore_last_region", FluentIcon::History},
        {"inapp_zoom_in", FluentIcon::Search},
        {"inapp_zoom_out", FluentIcon::Search},
        {"inapp_translate", FluentIcon::Language},
        {"inapp_text_recognize", QString::fromUtf8("文字识别.svg")},
        {"inapp_tool_cursor", QString::fromUtf8("鼠标.svg")},
        {"inapp_tool_pen", QString::fromUtf8("画笔.svg")},
        {"inapp_tool_highlighter", QString::fromUtf8("荧光笔.svg")},
        {"inapp_tool_mosaic", QString::fromUtf8("马赛克.svg")},
        {"inapp_tool_arrow", QString::fromUtf8("箭头.svg")},
        {"inapp_tool_number", QString::fromUtf8("序号线框.svg")},
        {"inapp_tool_rect", QString::fromUtf8("方框.svg")},
        {"inapp_tool_ellipse", QString::fromUtf8("圆框.svg")},
        {"inapp_tool_text", QString::fromUtf8("文字.svg")},
        {"inapp_tool_eraser", QString::fromUtf8("橡皮.svg")},
        {"inapp_copy_pin", QString::fromUtf8("复制.svg")},
        {"inapp_copy_pin_text", QString::fromUtf8("文字识别.svg")},
        {"inapp_pin_reset_size", QString::fromUtf8("重置大小.svg")},
        {"inapp_thumbnail", QString::fromUtf8("缩略图.svg")},
        {"inapp_toggle_toolbar", QString::fromUtf8("切换工具栏.svg")},
        {"inapp_pin_hide_annotations", QString::fromUtf8("隐藏标注.svg")},
        {"inapp_pin_lock", FluentIcon::Pin},
        {"inapp_pin_text_mode", QString::fromUtf8("文字.svg")},
        {"inapp_pin_text_selection", QString::fromUtf8("选择.svg")},
        {"inapp_pin_text_edit", FluentIcon::Edit},
        {"inapp_clipboard_quick_edit", FluentIcon::Edit},
        {"inapp_clipboard_edit_save", FluentIcon::Save},
        {"inapp_clipboard_edit_save_paste", FluentIcon::Send},
        {"inapp_clipboard_search", FluentIcon::Search},
    };
    return icons.value(cfg_key);
}

bool validate_global_hotkey_edits(SettingsDialog *dialog, bool check_system) {
    // Only which editors form one conflict domain is decided here; the rules live in
    // validate_hotkey_group, shared with the setup wizard.
    return validate_hotkey_group(global_hotkey_edits(dialog), check_system);
}

QString inapp_label(SettingsDialog *dialog, const QString &cfg_key) {
    for (const auto &key : INAPP_KEYS())
        if (key.cfg_key == cfg_key) return dialog->tr_text(key.label);
    return cfg_key;
}

QWidget *create_hotkey_page(SettingsDialog *dialog) {
    auto *scroll = page_scroll_area(dialog);
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet("QScrollArea { border: none; background: transparent; }");

    auto *view = new QWidget;
    set_own_style(view, "background: transparent;");
    auto *layout = new QVBoxLayout(view);
    layout->setContentsMargins(0, 0, dialog_scaled(10), 0);
    layout->setSpacing(dialog_scaled(12));

    // ════ Global hotkeys ════
    auto *grp_global = new SectionCard(ResourceManager::get_icon_path(QString::fromUtf8("热键.svg")), dialog->tr_text("Global Hotkeys"), dialog->tr_text("Works anytime"), view);
    for (const auto &row : GLOBAL_HOTKEY_ROWS()) {
        auto values = dialog->config_manager->get_global_hotkeys(row.function);
        if (row.function == "screenshot") values[0] = dialog->current_hotkey;
        const auto names = global_hotkey_edit_attrs(row.first_attr);
        std::vector<HotkeyEdit *> editors;
        for (int slot = 0; slot < names.size(); ++slot) {
            auto *edit = new HotkeyEdit;
            edit->setText(values[slot]);
            dialog->set_attr(names[slot], edit);
            editors.push_back(edit);
        }
        grp_global->addRow(global_row(grp_global, dialog->tr_text(row.title), row.tone, row.icon, editors));
    }

    // Conflicts are checked once every editor exists, so initialisation never sees half the
    // group; a final run flags old conflicts left in the configuration.
    for (auto *edit : global_hotkey_edits(dialog)) {
        QObject::connect(edit, &HotkeyEdit::textChanged, dialog, [dialog, edit] { on_global_hotkey_changed(dialog, edit); });
        QObject::connect(edit, &HotkeyEdit::textChanged, dialog, [dialog] { validate_global_hotkey_edits(dialog); });
        // In-app shortcuts that collide with a global hotkey update their hint too.
        QObject::connect(edit, &HotkeyEdit::textChanged, dialog, [dialog] { refresh_inapp_shadow_states(dialog); });
    }
    validate_global_hotkey_edits(dialog);
    layout->addWidget(grp_global);

    // ════ In-app shortcuts ════
    auto *grp_inapp = new SectionCard(FluentIcon::Layout, dialog->tr_text("In-App Shortcuts"), dialog->tr_text("Only in screenshot, pin and clipboard windows"), view);
    dialog->inapp_edits.clear();
    dialog->inapp_groups.clear();

    auto *tab_switch = new SegmentedWidget(grp_inapp);
    tab_switch->setFixedHeight(dialog_scaled(34));
    tab_switch->setIndicatorColor(ACCENT, ACCENT);

    auto *stack = new QStackedWidget(grp_inapp);
    stack->setObjectName("InAppShortcutStack");
    stack->setStyleSheet("#InAppShortcutStack { background: transparent; border: none; }");

    const auto build_tab = [dialog](const std::vector<InAppKey> &keys_list, const QString &group_name, const std::vector<std::function<QWidget *(QWidget *)>> &extra_rows = {},
                                    bool allow_mouse = true, const QSet<QString> &text_input_keys = {}) {
        auto *page = new QWidget;
        auto *vbox = new QVBoxLayout(page);
        vbox->setContentsMargins(0, 0, 0, 0);
        vbox->setSpacing(0);
        for (const auto &key : keys_list) {
            auto *edit = new InAppKeyEdit(nullptr, allow_mouse && key.cfg_key != "inapp_pin_reset_size", !text_input_keys.contains(key.cfg_key));
            QString value = dialog->config_manager->get_inapp_shortcut(key.cfg_key);
            // The mouse binding is edited in the mouse page's section.
            if (key.cfg_key == "inapp_pin_reset_size" && is_inapp_mouse_shortcut(value)) value = "";
            edit->setText(reserved_free(value));
            dialog->inapp_edits.emplace_back(key.cfg_key, edit);
            dialog->inapp_groups[key.cfg_key] = group_name;
            add_separated_row(vbox, inapp_row(page, inapp_icon(key.cfg_key), dialog->tr_text(key.label), edit));
        }
        for (const auto &build_row : extra_rows) add_separated_row(vbox, build_row(page));
        vbox->addStretch(1);
        return page;
    };

    // Cursor nudge keys.
    auto *cursor_move_combo = new ComboBox;
    cursor_move_combo->addItem(QString::fromUtf8("WASD+↑↓←→"), "both");
    cursor_move_combo->addItem(QString::fromUtf8("↑↓←→"), "arrows");
    cursor_move_combo->addItem("WASD", "wasd");
    dialog->set_attr("cursor_move_combo", cursor_move_combo);
    if (const int idx = cursor_move_combo->findData(dialog->config_manager->get_inapp_cursor_move_mode()); idx >= 0) cursor_move_combo->setCurrentIndex(idx);

    auto *screenshot_tab = build_tab(SCREENSHOT_KEYS, "screenshot",
                                     {[dialog, cursor_move_combo](QWidget *page) { return inapp_row(page, CURSOR_MOVE_ICON, dialog->tr_text("Cursor Move Keys"), cursor_move_combo); }});
    auto *tools_tab = build_tab(TOOL_KEYS(), "screenshot");
    auto *pin_tab = build_tab(PIN_KEYS, "pin");
    stack->addWidget(screenshot_tab);
    stack->addWidget(tools_tab);
    stack->addWidget(pin_tab);
    QStringList routes{"screenshot", "tools", "pin"};
    tab_switch->addItem("screenshot", dialog->tr_text("Screenshot Shortcuts"));
    tab_switch->addItem("tools", dialog->tr_text("Annotation Tools"));
    tab_switch->addItem("pin", dialog->tr_text("Pin Shortcuts"));

    // Direct pick keys are not configured one by one, only which set; conflicts count them in the clipboard group.
    auto *clipboard_pick_combo = new ComboBox;
    for (const auto &[mode, label] : {std::make_pair("both", "Digits + Letters"), std::make_pair("letters", "Letters"), std::make_pair("digits", "Digits")})
        clipboard_pick_combo->addItem(dialog->tr_text(label), QString(mode));
    dialog->set_attr("clipboard_pick_combo", clipboard_pick_combo);
    if (const int idx = clipboard_pick_combo->findData(dialog->config_manager->get_inapp_clipboard_pick_mode()); idx >= 0) clipboard_pick_combo->setCurrentIndex(idx);
    // The clipboard window's handler takes only keys.
    QSet<QString> editor_keys;
    for (const auto &key : CLIPBOARD_EDITOR_KEYS) editor_keys.insert(key.cfg_key);
    for (const auto &key : CLIPBOARD_SEARCH_KEYS) editor_keys.insert(key.cfg_key);
    auto keys = CLIPBOARD_KEYS;
    keys.insert(keys.end(), CLIPBOARD_EDITOR_KEYS.begin(), CLIPBOARD_EDITOR_KEYS.end());
    keys.insert(keys.end(), CLIPBOARD_SEARCH_KEYS.begin(), CLIPBOARD_SEARCH_KEYS.end());
    stack->addWidget(build_tab(keys, "clipboard",
                               {[dialog, clipboard_pick_combo](QWidget *page) { return inapp_row(page, FluentIcon::Paste, dialog->tr_text("Direct Pick Keys"), clipboard_pick_combo); }},
                               false, editor_keys));
    routes.append("clipboard");
    tab_switch->addItem("clipboard", dialog->tr_text("Clipboard Shortcuts"));

    link_tabs(tab_switch, stack, routes);
    tab_switch->setCurrentItem("screenshot");
    // The pin page's "Edit" button switches to the pin tab here.
    dialog->inapp_shortcut_tabs = tab_switch;

    auto *tab_row = new QWidget(grp_inapp);
    auto *tab_row_layout = new QHBoxLayout(tab_row);
    tab_row_layout->setContentsMargins(0, 0, 0, dialog_scaled(10));
    tab_row_layout->addWidget(tab_switch);
    tab_row_layout->addStretch(1);
    grp_inapp->addWidget(tab_row);
    grp_inapp->addWidget(stack);

    // Conflict detection.
    for (const auto &[cfg_key, edit] : dialog->inapp_edits) {
        const QString key = cfg_key;
        QObject::connect(edit, &QLineEdit::textChanged, dialog, [dialog, key](const QString &text) { on_shortcut_changed(dialog, key, text); });
        QObject::connect(edit, &QLineEdit::textChanged, dialog, [dialog] { refresh_inapp_shadow_states(dialog); });
    }
    // Flags entries already colliding with a global hotkey in the configuration.
    refresh_inapp_shadow_states(dialog);
    if (clipboard_pick_combo) {
        dialog->clipboard_pick_mode = clipboard_pick_combo->currentData().toString();
        QObject::connect(clipboard_pick_combo, &QComboBox::currentIndexChanged, dialog, [dialog] { on_pick_mode_changed(dialog); });
    }

    layout->addWidget(grp_inapp);
    layout->addStretch();
    scroll->setWidget(view);
    return scroll;
}
}
