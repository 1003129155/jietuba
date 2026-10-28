#include "ui/hotkey_edit.h"
#include "core/i18n.h"
#include "core/ui_scale.h"
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <map>
#include <set>

namespace jietuba {
namespace {
QString tr_text(const char *text) { return i18n_tr(text, "HotkeyEdit"); }

QStringList modifier_parts(Qt::KeyboardModifiers modifiers) {
    QStringList parts;
    if (modifiers & Qt::ControlModifier) parts.append("Ctrl");
    if (modifiers & Qt::ShiftModifier) parts.append("Shift");
    if (modifiers & Qt::AltModifier) parts.append("Alt");
    if (modifiers & Qt::MetaModifier) parts.append("Win");
    return parts;
}

bool is_modifier_key(int key) { return key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt || key == Qt::Key_Meta; }
}

bool HotkeyEditHandler::owns_window(const QWidget *window) const { return line_edit_ && window == line_edit_->window(); }

bool HotkeyEditHandler::handle_key(QInputEvent *event) {
    if (event->type() == QEvent::KeyPress) line_edit_->capture_key(static_cast<QKeyEvent *>(event));
    return true;
}

bool HotkeyEditHandler::handle_hotkey(int hotkey_id, const std::function<void()> &) {
    if (const auto meta = ShortcutManager::instance()->hotkey_metadata(hotkey_id))
        if (const auto text = global_hotkey::format_hotkey(meta->first, meta->second)) line_edit_->setText(*text);
    return true;
}

bool HotkeyEditHandler::handle_captured_key(const QString &text) {
    line_edit_->setText(text);
    return true;
}

bool HotkeyEditHandler::handle_mouse_hotkey(const QString &token, const std::function<void()> &) {
    line_edit_->setText(token);
    return true;
}

HotkeyLineEdit::HotkeyLineEdit(QWidget *parent) : KeyChipLineEdit(parent), shortcut_handler_(std::make_unique<HotkeyEditHandler>(this)) {
    // Not read-only, which may block key events; standard input is blocked by not calling the base keyPressEvent.
    setFocusPolicy(Qt::StrongFocus);
}

HotkeyLineEdit::~HotkeyLineEdit() { release_capture(); }

void HotkeyLineEdit::capture_key(QKeyEvent *event) {
    const int key = event->key();
    const auto modifiers = event->modifiers();
    event->accept();

    QStringList parts = modifier_parts(modifiers);
    // The pressed key may itself be a modifier whose flag lags behind.
    if (key == Qt::Key_Control && !parts.contains("Ctrl")) parts.append("Ctrl");
    if (key == Qt::Key_Shift && !parts.contains("Shift")) parts.append("Shift");
    if (key == Qt::Key_Alt && !parts.contains("Alt")) parts.append("Alt");
    if (key == Qt::Key_Meta && !parts.contains("Win")) parts.append("Win");
    const bool is_modifier_present = !parts.isEmpty();

    // The key text, unless the key is a modifier.
    QString key_text;
    if (!is_modifier_key(key)) {
        const auto &display_map = get_key_display_map();
        if (display_map.contains(key)) {
            key_text = display_map.value(key);
        } else if (Qt::Key_F1 <= key && key <= Qt::Key_F35) {
            key_text = QString("F%1").arg(key - Qt::Key_F1 + 1);
        } else {
            QString text = QKeySequence(key).toString(QKeySequence::PortableText);
            if (text.isEmpty()) text = event->text().toUpper();
            key_text = text;
        }
    }

    // An unknown key changes nothing: the unassigned key the hook injects to keep the Start
    // menu closed arrives here too, while Win is still held.
    if (!is_modifier_key(key) && key_text.isEmpty()) return;
    // Backspace / Delete without modifiers clears.
    if (!is_modifier_present && (key_text == "Backspace" || key_text == "Delete")) {
        setText("");
        return;
    }
    // Modifiers without a main key yet show as "ctrl+".
    if (is_modifier_present && key_text.isEmpty()) {
        if (!text().endsWith("+")) before_prefix_ = text();
        setText(parts.join("+").toLower() + "+");
        return;
    }
    // Combinations are recorded as typed and the status icon tells the unusable ones; a
    // single key only when it binds alone.
    parts.append(key_text);
    const QString hotkey_str = parts.join("+").toLower();
    if (is_modifier_present || global_hotkey::route(hotkey_str) != global_hotkey::Route::Invalid) setText(hotkey_str);
}

void HotkeyLineEdit::keyReleaseEvent(QKeyEvent *event) {
    // Releasing a modifier of an unfinished combination shows what is still held, or the value before.
    if (is_modifier_key(event->key()) && text().endsWith("+")) {
        const auto parts = modifier_parts(event->modifiers());
        setText(parts.isEmpty() ? before_prefix_ : parts.join("+").toLower() + "+");
    }
    KeyChipLineEdit::keyReleaseEvent(event);
}

void HotkeyLineEdit::focusInEvent(QFocusEvent *event) {
    KeyChipLineEdit::focusInEvent(event);
    shortcut_handler_->active = true;
    auto *manager = ShortcutManager::instance();
    manager->register_handler(shortcut_handler_.get());
    if (!capture_held_) {
        // While recording the hook takes every side button and the keys the system holds,
        // so unbound side buttons and combinations such as Win+V can be recorded too.
        capture_held_ = true;
        manager->begin_mouse_capture();
        manager->begin_key_capture();
    }
}

void HotkeyLineEdit::focusOutEvent(QFocusEvent *event) {
    release_capture();
    if (text().endsWith("+")) setText(before_prefix_);
    KeyChipLineEdit::focusOutEvent(event);
}

void HotkeyLineEdit::release_capture() {
    shortcut_handler_->active = false;
    auto *manager = ShortcutManager::instance();
    manager->unregister_handler(shortcut_handler_.get());
    if (capture_held_) {
        // Released in pairs, or the hook keeps side buttons and these combinations from other programs.
        capture_held_ = false;
        manager->end_mouse_capture();
        manager->end_key_capture();
    }
}

HotkeyEdit::HotkeyEdit(QWidget *parent) : QWidget(parent), check_timer_(this) {
    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(dialog_scaled(STATUS_GAP));

    edit = new HotkeyLineEdit(this);
    layout_->addWidget(edit, 1);

    status_icon = new StatusIcon(this);
    layout_->addWidget(status_icon, 0, Qt::AlignVCenter);

    // Checks availability 200 ms after typing stops.
    check_timer_.setSingleShot(true);
    check_timer_.setInterval(200);
    connect(&check_timer_, &QTimer::timeout, this, [this] { check_availability_now(); });

    connect(edit, &QLineEdit::textChanged, this, &HotkeyEdit::on_text_changed);
    connect(edit, &QLineEdit::textChanged, this, &HotkeyEdit::textChanged);
    render_status();
}

void HotkeyEdit::setText(const QString &text) {
    edit->setText(text);
    check_availability_now();
}

QString HotkeyEdit::text() const { return edit->text(); }

void HotkeyEdit::on_text_changed(const QString &text) {
    // A new value invalidates the old system check; container conflicts are updated by
    // the outside textChanged listeners next, so the old availability is cleared first.
    system_available_.reset();
    taken_over_ = false;
    render_status();
    if (!text.isEmpty() && !text.endsWith("+")) check_timer_.start();
    else check_timer_.stop();
}

bool HotkeyEdit::check_availability_now() {
    const QString hotkey = text().trimmed();
    if (hotkey.isEmpty()) {
        // Empty means unbound, which is valid.
        system_available_ = true;
        render_status();
        return true;
    }
    if (hotkey.endsWith("+")) {
        system_available_ = false;
        render_status();
        return false;
    }
    route_ = global_hotkey::route(hotkey);
    taken_over_ = false;
    if (route_ == global_hotkey::Route::Invalid || route_ == global_hotkey::Route::Reserved) {
        system_available_ = false;
    } else if (route_ == global_hotkey::Route::Hook) {
        // A hook-route key works even when the system registration fails; PrtSc may register
        // and still open the system capture.
        const bool registrable = hotkey_system_.check_hotkey_availability(hotkey);
        taken_over_ = !registrable || global_hotkey::parse_hotkey(hotkey).second == global_hotkey::VK_SNAPSHOT;
        system_available_ = true;
    } else if (route_ == global_hotkey::Route::Mouse) {
        // A side button is taken by the hook; there is no keyboard key to parse and nothing for the system to hold.
        system_available_ = true;
    } else {
        // Held by another app: the hook takes it over while running, if it can bind the key.
        const bool registrable = hotkey_system_.check_hotkey_availability(hotkey);
        const auto [mods, vk] = global_hotkey::parse_hotkey(hotkey);
        taken_over_ = !registrable && global_hotkey::hook_can_bind(mods, vk);
        system_available_ = registrable || taken_over_;
    }
    render_status();
    return *system_available_;
}

void HotkeyEdit::render_status() {
    const QString hotkey = text().trimmed();
    QString state, tip;
    if (!validation_error_.isEmpty()) state = "error", tip = validation_error_;
    else if (hotkey.isEmpty()) state = "idle", tip = tr_text("Not set");
    else if (hotkey.endsWith("+") || !system_available_) state = "", tip = "";
    else if (*system_available_ && taken_over_ && route_ == global_hotkey::Route::Hook)
        state = "ok", tip = tr_text("Windows uses this shortcut too. This app takes it over while running; clear it to give it back.");
    else if (*system_available_ && taken_over_)
        state = "ok", tip = tr_text("Another app uses this shortcut too. This app takes it over while running; clear it to give it back.");
    else if (*system_available_) state = "ok", tip = tr_text("Available");
    else if (route_ == global_hotkey::Route::Reserved) state = "error", tip = tr_text("Reserved by Windows. No app can use it.");
    else if (route_ == global_hotkey::Route::Invalid) state = "error", tip = tr_text("This key can't be used as a global hotkey.");
    else state = "error", tip = tr_text("Already in use by another app.");
    status_icon->setState(state, tip);
    edit->setErrorState(state == "error");
    edit->setToolTip(state == "error" || taken_over_ ? tip : QString());
}

void HotkeyEdit::set_validation_error(const QString &message) {
    validation_error_ = message;
    render_status();
}

bool HotkeyEdit::validate_now() {
    if (!validation_error_.isEmpty()) return false;
    return check_availability_now();
}

bool validate_hotkey_group(const std::vector<HotkeyEdit *> &edits, bool check_system) {
    std::map<QString, std::vector<HotkeyEdit *>> by_identity;
    for (auto *edit : edits)
        if (const auto identity = hotkey_identity(edit->text())) by_identity[*identity].push_back(edit);
    std::set<HotkeyEdit *> duplicated;
    for (const auto &[identity, same_key_edits] : by_identity)
        if (same_key_edits.size() > 1) duplicated.insert(same_key_edits.begin(), same_key_edits.end());
    const QString conflict_message = tr_text("This hotkey is assigned more than once.");
    for (auto *edit : edits) edit->set_validation_error(duplicated.count(edit) ? conflict_message : QString());
    if (!check_system) return duplicated.empty();
    // Every editor is checked before deciding: stopping at the first failure would leave
    // later editors without fresh results, exposing one red mark per apply.
    bool all_ok = true;
    for (auto *edit : edits) all_ok = edit->validate_now() && all_ok;
    return all_ok && duplicated.empty();
}
}
