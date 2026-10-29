#include "pin/pin_manager.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "core/platform_utils.h"
#include "pin/pin_animation.h"
#include "pin/pin_file_card.h"
#include "pin/pin_text_content.h"
#include "pin/pin_window.h"
#include "settings/tool_settings.h"
#include "ui/dialogs.h"
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QScreen>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace jietuba {
namespace {
constexpr UINT SWP_FLAGS = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
PinManager *manager_instance = nullptr;
}

PinManager::PinManager() { log_info(T("钉图管理器已初始化"), "PinManager"); }

PinManager *PinManager::instance() {
    if (!manager_instance) manager_instance = new PinManager;
    return manager_instance;
}

bool PinManager::is_small_pin(const QSize &size) { return size.width() < MIN_SIDE || size.height() < MIN_SIDE; }

bool PinManager::confirm_small_pin(const QSize &size, QWidget *parent) {
    if (!is_small_pin(size)) return true;
    return show_confirm_dialog(parent, i18n_tr("Pin Too Small", "PinWindow"),
                               i18n_tr("This pin is only %1 × %2 pixels. Pin it anyway?", "PinWindow").arg(size.width()).arg(size.height()));
}

PinWindow *PinManager::create_pin(const QImage &image, const QPoint &position, ToolSettingsManager *config_manager, const QList<QGraphicsItem *> &drawing_items,
                                  const std::optional<QPoint> &selection_offset, std::optional<int> number_next) {
    // The new pin would stand alone, the others seemingly gone.
    if (all_hidden_) set_all_hidden(false);
    auto *pin_window = new PinWindow(image, position, config_manager, drawing_items, selection_offset, number_next);
    connect(pin_window, &PinWindow::closed, this, [this, pin_window] { on_pin_closed(pin_window); });
    pin_windows.append(pin_window);
    // A capture can pin an image while its session stays open: the pin goes below the capture window,
    // still in the topmost band, and the keys stay with the capture.
    if (capture_window_) {
        SetWindowPos(reinterpret_cast<HWND>(pin_window->winId()), reinterpret_cast<HWND>(capture_window_->winId()), 0, 0, 0, 0, SWP_FLAGS);
    } else {
        // The next keys are usually meant for the new pin. Under a capture they are still the capture's;
        // a capture that closes after pinning hands the focus over itself.
        pin_window->activateWindow();
    }
    emit pin_created(pin_window);
    log_debug(T("钉图已创建 (共 {count} 个)", {{"count", count()}}), "PinManager");
    return pin_window;
}

PinWindow *PinManager::create_text_pin(std::unique_ptr<PinTextContent> content, const QPoint &position, ToolSettingsManager *config_manager) {
    auto *pin_window = create_pin(content->take_page(), position, config_manager);
    pin_window->set_text_content(std::move(content));
    return pin_window;
}

PinWindow *PinManager::create_animation_pin(AnimationSource source, const QPoint &position, ToolSettingsManager *config_manager) {
    auto *pin_window = create_pin(source.first_frame, position, config_manager);
    pin_window->open_animation(std::move(source));
    return pin_window;
}

PinWindow *PinManager::create_file_pin(const PinFileCard &card, const QPoint &position, ToolSettingsManager *config_manager, const QImage &image) {
    auto *pin_window = create_pin(image.isNull() ? card.render(card.size()) : image, position, config_manager);
    pin_window->open_files(card);
    return pin_window;
}

void PinManager::on_pin_closed(PinWindow *pin_window) {
    if (!pin_windows.contains(pin_window)) return;
    pin_windows.removeAll(pin_window);
    emit pin_closed(pin_window);
    // Several closing together are merged into one trim.
    request_trim_working_set(1500);
    log_debug(T("钉图已关闭 (剩余 {count} 个)", {{"count", count()}}), "PinManager");
    if (pin_windows.isEmpty()) {
        all_hidden_ = false;
        hidden_order_.clear();
        emit all_pins_closed();
        log_debug(T("所有钉图已关闭"), "PinManager");
    }
}

void PinManager::remove_pin(PinWindow *pin_window) {
    if (!pin_windows.contains(pin_window)) return;
    pin_windows.removeAll(pin_window);
    log_debug(T("钉图已移除 (剩余 {count} 个)", {{"count", count()}}), "PinManager");
}

void PinManager::close_all() {
    if (pin_windows.isEmpty()) {
        log_debug(T("没有钉图窗口需要关闭"), "PinManager");
        return;
    }
    log_debug(T("开始关闭 {count} 个钉图窗口...", {{"count", count()}}), "PinManager");
    // A copy: closing removes entries.
    for (auto *pin_window : get_all_pins()) {
        try {
            pin_window->close_window();
        } catch (const std::exception &e) {
            log_error(T("关闭钉图窗口失败: {e}", {{"e", QString::fromUtf8(e.what())}}), "PinManager");
        }
    }
    // Closing the last pin has already announced it, unless some pin never reported closing.
    const bool announced = pin_windows.isEmpty();
    pin_windows.clear();
    log_debug(T("所有钉图窗口已关闭"), "PinManager");
    if (!announced) emit all_pins_closed();
}

QList<PinWindow *> PinManager::get_all_pins() const {
    QList<PinWindow *> result;
    for (const auto &pin : pin_windows)
        if (pin) result.append(pin);
    return result;
}

bool PinManager::repeats_pointed_out() const {
    auto *settings = get_tool_settings_manager();
    return !settings || settings->get_pin_point_out_repeats();
}

PinWindow *PinManager::image_pin_of(const QImage &image) const {
    if (image.isNull() || !repeats_pointed_out()) return nullptr;
    for (auto *pin : get_all_pins()) {
        if (pin->is_closed() || pin->text_only() || pin->animation() || pin->file_card()) continue;
        // The size rules out nearly every pin; only one of the same size has its pixels compared.
        const auto &pixmap = pin->base_pixmap();
        if (pixmap.size() != image.size()) continue;
        const QImage shown = pixmap.toImage();
        if (shown.format() == image.format() ? shown == image : shown == image.convertToFormat(shown.format())) return pin;
    }
    return nullptr;
}

PinWindow *PinManager::text_pin_of(const QString &text) const {
    if (text.isEmpty() || !repeats_pointed_out()) return nullptr;
    for (auto *pin : get_all_pins())
        if (!pin->is_closed() && pin->text_only() && pin->text_content()->text() == text) return pin;
    return nullptr;
}

PinWindow *PinManager::file_pin_of(const QStringList &files) const {
    if (files.isEmpty() || !repeats_pointed_out()) return nullptr;
    for (auto *pin : get_all_pins())
        if (!pin->is_closed() && pin->file_card() && pin->file_card()->files() == files) return pin;
    return nullptr;
}

PinWindow *PinManager::animation_pin_of(const QString &file) const {
    if (file.isEmpty() || !repeats_pointed_out()) return nullptr;
    for (auto *pin : get_all_pins())
        if (!pin->is_closed() && pin->animation() && QFileInfo(pin->animation()->source().file_path) == QFileInfo(file)) return pin;
    return nullptr;
}

bool PinManager::point_out(PinWindow *pin) {
    if (!pin) return false;
    if (all_hidden_) set_all_hidden(false);
    pin->raise();
    pin->activateWindow();
    pin->shake();
    log_debug(T("内容已在钉图中，提示已有钉图"), "PinManager");
    return true;
}

void PinManager::set_all_hidden(bool hidden) {
    if (hidden == all_hidden_) return;
    if (hidden) {
        if (pin_windows.isEmpty()) return;
        hidden_order_.clear();
        const auto pins = get_all_pins();
        for (HWND hwnd = GetTopWindow(nullptr); hwnd; hwnd = GetWindow(hwnd, GW_HWNDNEXT))
            for (auto *pin : pins)
                if (reinterpret_cast<HWND>(pin->winId()) == hwnd) hidden_order_.append(pin);
        for (auto *pin : pins) {
            // The toolbar is a window of its own and would stay.
            pin->stop_drawing();
            pin->hide_toolbar();
            pin->hide();
        }
        all_hidden_ = true;
        log_debug(T("隐藏了 {count} 个钉图窗口", {{"count", count()}}), "PinManager");
        emit all_hidden_changed(true);
        return;
    }
    all_hidden_ = false;
    for (auto *pin : get_all_pins()) {
        pin->setAttribute(Qt::WA_ShowWithoutActivating, true);
        pin->show();
        pin->setAttribute(Qt::WA_ShowWithoutActivating, false);
    }
    // Each below the one that was above it.
    HWND above = nullptr;
    for (const auto &pin : hidden_order_) {
        if (!pin || pin->is_closed()) continue;
        const auto hwnd = reinterpret_cast<HWND>(pin->winId());
        if (above) SetWindowPos(hwnd, above, 0, 0, 0, 0, SWP_FLAGS | SWP_NOOWNERZORDER);
        above = hwnd;
    }
    hidden_order_.clear();
    log_debug(T("显示了 {count} 个钉图窗口", {{"count", count()}}), "PinManager");
    emit all_hidden_changed(false);
}

void PinManager::move_all_to_screen_center() {
    for (auto *pin_window : get_all_pins()) {
        auto *screen = QApplication::screenAt(pin_window->geometry().center());
        if (!screen) screen = QApplication::primaryScreen();
        if (!screen) continue;
        const auto screen_rect = screen->availableGeometry();
        const int x = screen_rect.x() + (screen_rect.width() - pin_window->width()) / 2;
        const int y = screen_rect.y() + (screen_rect.height() - pin_window->height()) / 2;
        pin_window->move(x, y);
    }
    log_debug(T("已移动 {count} 个钉图到屏幕中心", {{"count", count()}}), "PinManager");
}

void PinManager::refresh_all_appearance() {
    for (auto *pin_window : get_all_pins()) {
        try {
            pin_window->refresh_appearance();
        } catch (const std::exception &e) {
            log_error(T("刷新钉图外观失败: {e}", {{"e", QString::fromUtf8(e.what())}}), "PinManager");
        }
    }
}

void PinManager::set_all_thumbnail_mode(bool active) {
    int changed = 0;
    for (auto *pin_window : get_all_pins()) {
        if (pin_window->thumbnail_mode() != active) {
            pin_window->toggle_thumbnail_mode();
            ++changed;
        }
    }
    if (active) log_debug(T("{changed} 个钉图已进入缩略图模式", {{"changed", changed}}), "PinManager");
    else log_debug(T("{changed} 个钉图已退出缩略图模式", {{"changed", changed}}), "PinManager");
}

std::pair<int, int> PinManager::save_all_to_directory(const QString &directory, const QString &prefix) {
    QDir().mkpath(directory);
    int saved = 0, failed = 0;
    int index = 0;
    for (auto *pin_window : get_all_pins()) {
        ++index;
        const auto file_path = QDir(directory).filePath(QString("%1_%2.png").arg(prefix).arg(index, 3, 10, QChar('0')));
        bool ok = false;
        pin_window->with_edit_paused([&] { ok = pin_window->get_current_image().save(file_path); });
        if (ok) {
            ++saved;
        } else {
            ++failed;
            log_error(T("保存钉图失败: {file_path}", {{"file_path", QDir::toNativeSeparators(file_path)}}), "PinManager");
        }
    }
    log_info(T("批量保存钉图完成: 成功 {saved}, 失败 {failed}", {{"saved", saved}, {"failed", failed}}), "PinManager");
    return {saved, failed};
}

void PinManager::begin_capture(QWidget *capture_window) { capture_window_ = capture_window; }

void PinManager::end_capture() { capture_window_ = nullptr; }

void PinManager::cleanup() {
    log_debug(T("清理管理器..."), "PinManager");
    close_all();
    manager_instance = nullptr;
    deleteLater();
    log_info(T("管理器已清理"), "PinManager");
}
}
