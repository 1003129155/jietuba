#include "core/constants.h"
#include "core/crash_handler.h"
#include "core/elevation.h"
#include "core/logger.h"
#include "core/package_identity.h"
#include "core/platform_utils.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "settings/tool_settings.h"
#include "core/resource_manager.h"
#include "ui/welcome/demo_paint.h"
#include "ui/welcome/pages.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <QPainterPath>
#include <QStandardPaths>
#include <QTimer>
#include <cmath>
#include <thread>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>

namespace jietuba {
namespace {
const QString DESKTOP_LNK_NAME = PRODUCT_NAME + ".lnk";

using namespace welcome_demo;

// The corner of the taskbar: the hidden icons button opens its flyout, the program's icon is
// dragged from there onto the taskbar, and stays in view. Other programs' icons are plain tiles.
// Drawn in 100% units under the window scale.
class TrayDemo : public LoopingDemo {
public:
    static constexpr int DURATION_MS = 5800;
    static constexpr int TO_CHEVRON_MS = 300, CLICK_MS = 950, OPEN_END_MS = 1150;
    static constexpr int TO_ICON_MS = 1250, PRESS_MS = 1800, DRAG_END_MS = 2700, CLOSE_END_MS = 2950;
    static constexpr int PULSE_MS = 3050;
    static constexpr double BAR_H = 54, SLOT = 38, CELL = 40, ICON = 22, CLOCK_W = 76;
    // The flyout is four across; the program's icon is the sixth.
    static constexpr int COLUMNS = 4, FLYOUT_ICONS = 12, OURS = 5;

    explicit TrayDemo(QWidget *parent = nullptr) : LoopingDemo(DURATION_MS, parent), logo_(ResourceManager::get_resource_path(QString::fromUtf8("svg/托盘.svg"))) {
        setMinimumHeight(dialog_scaled(220));
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const double scale = demo_scale();
        p.scale(scale, scale);
        const double w = width() / scale, h = height() / scale;
        const auto theme = welcome_theme();
        const int now = time();
        p.setOpacity(loop_opacity());

        const QRectF desk(4, 4, w - 8, h - 8);
        QPainterPath clip;
        clip.addRoundedRect(desk, 10, 10);
        p.setPen(QPen(QColor(theme.border), 1));
        p.setBrush(QColor(theme.panel_subtle));
        p.drawPath(clip);
        p.save();
        p.setClipPath(clip);

        // The taskbar and its tray, right to left: the clock, the icons in view, the button.
        const QRectF bar(desk.x(), desk.bottom() - BAR_H, desk.width(), BAR_H);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(theme.panel));
        p.drawRect(bar);
        p.setPen(QPen(QColor(theme.border), 1));
        p.drawLine(bar.topLeft(), bar.topRight());
        const QRectF clock(bar.right() - 12 - CLOCK_W, bar.y(), CLOCK_W, BAR_H);
        p.setPen(Qt::NoPen);
        p.setBrush(with_alpha(theme.text, 170));
        p.drawRoundedRect(QRectF(clock.right() - 40, clock.center().y() - 9, 40, 5), 2.5, 2.5);
        p.setBrush(with_alpha(theme.text, 110));
        p.drawRoundedRect(QRectF(clock.right() - 58, clock.center().y() + 4, 58, 4.5), 2.2, 2.2);
        const double gap = ease(QEasingCurve::OutCubic, phase(now, DRAG_END_MS - 300, DRAG_END_MS));
        const double right = clock.x() - 6;
        const auto slot_x = [&](double index) { return right - (index + 1) * SLOT; };
        for (int i = 0; i < 3; ++i) draw_tile(p, QRectF(slot_x(i) + (SLOT - ICON) / 2, bar.center().y() - ICON / 2, ICON, ICON), 20 + i);
        // The button moves aside as the icon comes in; it points down while its flyout is open.
        const QRectF chevron(slot_x(3 + gap), bar.center().y() - 16, SLOT, 32);
        const bool open = now >= CLICK_MS && now < DRAG_END_MS;
        if (open || (now >= CLICK_MS - 80 && now < CLICK_MS)) {
            p.setPen(Qt::NoPen);
            p.setBrush(with_alpha(theme.text, theme.is_dark ? 30 : 18));
            p.drawRoundedRect(chevron.adjusted(2, 0, -2, 0), 5, 5);
        }
        QPen arrow(QColor(theme.text), 1.8);
        arrow.setCapStyle(Qt::RoundCap);
        arrow.setJoinStyle(Qt::RoundJoin);
        p.setPen(arrow);
        const QPointF c = chevron.center();
        const double dir = open ? 1 : -1;
        p.drawPolyline(QPolygonF({c + QPointF(-5.5, -2.8 * dir), c + QPointF(0, 2.8 * dir), c + QPointF(5.5, -2.8 * dir)}));

        // The flyout over the button.
        const double fly_w = COLUMNS * CELL + 16, fly_h = (FLYOUT_ICONS / COLUMNS) * CELL + 16;
        const QRectF flyout(std::min(slot_x(3) + SLOT / 2 - fly_w / 2, desk.right() - 8 - fly_w), bar.y() - 12 - fly_h, fly_w, fly_h);
        const double shown = std::min(ease(QEasingCurve::OutCubic, phase(now, CLICK_MS, OPEN_END_MS)), 1.0 - phase(now, DRAG_END_MS + 50, CLOSE_END_MS));
        const bool dragging = now >= PRESS_MS && now < DRAG_END_MS;
        const bool placed = now >= DRAG_END_MS;
        if (shown > 0) {
            p.save();
            p.setOpacity(p.opacity() * shown);
            const QRectF box = flyout.translated(0, 6 * (1 - shown));
            draw_shadow(p, theme, box, 10);
            p.setPen(QPen(QColor(theme.border), 0.8));
            p.setBrush(QColor(theme.panel));
            p.drawRoundedRect(box, 10, 10);
            for (int i = 0; i < FLYOUT_ICONS; ++i) {
                if (i == OURS && (dragging || placed)) continue;
                const QRectF cell = cell_rect(box, i);
                const double inset = (CELL - ICON) / 2;
                if (i == OURS) draw_logo(p, cell.adjusted(inset, inset, -inset, -inset));
                else draw_tile(p, cell.adjusted(inset, inset, -inset, -inset), i);
            }
            p.restore();
        }

        // The program's icon: in the flyout, then under the pointer, then on the taskbar.
        const double inset = (CELL - ICON) / 2;
        const QRectF in_flyout = cell_rect(flyout, OURS).adjusted(inset, inset, -inset, -inset);
        const QRectF on_bar(slot_x(3) + (SLOT - ICON) / 2, bar.center().y() - ICON / 2, ICON, ICON);
        const QPointF rest(desk.x() + desk.width() * 0.45, desk.y() + desk.height() * 0.35);
        QPointF cursor = rest;
        const auto move = [now](int start, int end) { return ease(QEasingCurve::InOutCubic, phase(now, start, end)); };
        if (now < TO_ICON_MS) cursor = lerp(rest, chevron.center() + QPointF(1, 2), move(TO_CHEVRON_MS, CLICK_MS - 80));
        else if (now < PRESS_MS) cursor = lerp(chevron.center() + QPointF(1, 2), in_flyout.center(), move(TO_ICON_MS, PRESS_MS - 80));
        else if (dragging) {
            const QPointF at = lerp(in_flyout.center(), on_bar.center(), move(PRESS_MS + 80, DRAG_END_MS - 60));
            draw_shadow(p, theme, QRectF(at - QPointF(13, 13), QSizeF(26, 26)), 6);
            draw_logo(p, QRectF(at - QPointF(13, 13), QSizeF(26, 26)));
            cursor = at + QPointF(4, 4);
        } else {
            // Released: the pointer moves off the icon.
            cursor = on_bar.center() + QPointF(14, -24) * move(DRAG_END_MS + 100, DRAG_END_MS + 600);
        }
        if (placed) {
            draw_logo(p, on_bar);
            // Two rings show where it now stays.
            for (int ring = 0; ring < 2; ++ring) {
                const double k = phase(now, PULSE_MS + ring * 450, PULSE_MS + ring * 450 + 700);
                if (k <= 0 || k >= 1) continue;
                QColor color(theme.accent);
                color.setAlpha(static_cast<int>(200 * (1 - k)));
                p.setPen(QPen(color, 2));
                p.setBrush(Qt::NoBrush);
                p.drawEllipse(on_bar.center(), 14 + 14 * k, 14 + 14 * k);
            }
        }
        p.restore();
        draw_cursor(p, cursor);
    }
private:
    static QRectF cell_rect(const QRectF &flyout, int index) { return {flyout.x() + 8 + (index % COLUMNS) * CELL, flyout.y() + 8 + (index / COLUMNS) * CELL, CELL, CELL}; }

    void draw_logo(QPainter &p, const QRectF &box) const { logo_.paint(&p, box.toRect()); }

    // Another program's icon: a plain tile in one of a few muted colours.
    static void draw_tile(QPainter &p, const QRectF &box, int seed) {
        static const char *colors[] = {"#7E9CB8", "#8DB59B", "#B59A7E", "#A48DB5", "#7EAEB5", "#B58D9A", "#9AA3AD"};
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(colors[seed % std::size(colors)]));
        p.drawRoundedRect(box, box.width() * 0.28, box.width() * 0.28);
        p.setBrush(QColor(255, 255, 255, 190));
        if (seed % 3 == 0) p.drawEllipse(box.center(), box.width() * 0.2, box.width() * 0.2);
        else if (seed % 3 == 1) p.drawRoundedRect(QRectF(box.center().x() - box.width() * 0.25, box.center().y() - box.height() * 0.08, box.width() * 0.5, box.height() * 0.16), 1, 1);
        else p.drawRoundedRect(QRectF(box.center().x() - box.width() * 0.18, box.center().y() - box.height() * 0.18, box.width() * 0.36, box.height() * 0.36), 1.5, 1.5);
    }

    QIcon logo_;
};

class FinishIllus : public IllustrationArea {
public:
    using IllustrationArea::IllustrationArea;
    void retranslate() override { caption_->setText(caption_text()); }
    void apply_welcome_theme() override {
        IllustrationArea::apply_welcome_theme();
        if (caption_) apply_welcome_label_style(caption_);
    }
    TrayDemo *demo = nullptr;
protected:
    void build_content() override {
        layout_->setContentsMargins(dialog_scaled(22), dialog_scaled(22), dialog_scaled(22), dialog_scaled(22));
        layout_->addStretch();
        demo = new TrayDemo(this);
        demo->setFixedHeight(dialog_scaled(300));
        layout_->addWidget(demo);
        caption_ = new QLabel(caption_text(), this);
        caption_->setAlignment(Qt::AlignCenter);
        caption_->setWordWrap(true);
        set_welcome_label_style(caption_, "muted", 12, 400);
        layout_->addWidget(caption_);
        layout_->addStretch();
    }
private:
    static QString caption_text() { return welcome_tr("把托盘图标从「^」里拖到任务栏上，就能一直看到它。"); }
    QLabel *caption_ = nullptr;
};

QString page_title() { return welcome_tr("🎉 一切就绪！").remove(QString::fromUtf8("🎉")).trimmed(); }
QString page_subtitle() { return welcome_tr("你已完成基础设置，截图工具已准备好为你服务。\n随时可以在设置面板调整更多选项。"); }
}

FinishPage::FinishPage(ToolSettingsManager *config, QWidget *parent) : BasePage(page_title(), page_subtitle(), parent), config_(config) { init(); }

IllustrationArea *FinishPage::create_illustration() { return new FinishIllus(this); }

void FinishPage::build_controls(QVBoxLayout *layout) {
    // Launch at sign-in.
    autostart_switch_ = new WelcomeToggleSwitch;
    autostart_switch_->setChecked(config_->get_autostart_enabled());
    QWidget *row = nullptr;
    std::tie(row, autostart_lbl_, autostart_desc_) = make_setting_row_with_refs(welcome_tr("开机自启"), autostart_switch_, welcome_tr("开机后在检测更新后启动。"));
    layout->addWidget(row);
    row->setVisible(!is_packaged());
    // Show the main window at start.
    show_main_switch_ = new WelcomeToggleSwitch;
    show_main_switch_->setChecked(config_->get_show_main_window());
    std::tie(row, show_main_lbl_, show_main_desc_) = make_setting_row_with_refs(welcome_tr("启动时显示主界面"), show_main_switch_, welcome_tr("每次启动时自动打开设置面板。"));
    layout->addWidget(row);
    // Desktop shortcut, on by default.
    desktop_switch_ = new WelcomeToggleSwitch;
    desktop_switch_->setChecked(true);
    std::tie(row, desktop_lbl_, desktop_desc_) = make_setting_row_with_refs(welcome_tr("快捷方式"), desktop_switch_, brand_text(welcome_tr("完成向导时在桌面创建截图吧快捷方式。")));
    layout->addWidget(row);
    // A shortcut to the exe inside the package folder cannot be launched.
    row->setVisible(!is_packaged());
    // Off unless chosen before; the tray menu has the same switch.
    game_pause_switch_ = new WelcomeToggleSwitch;
    game_pause_switch_->setChecked(config_->get_pause_hotkeys_in_games());
    std::tie(row, game_pause_lbl_, game_pause_desc_) =
        make_setting_row_with_refs(welcome_tr("玩游戏时暂停热键"), game_pause_switch_, welcome_tr("游戏在前台时暂停全局快捷键和鼠标快捷键，离开游戏后恢复。托盘菜单里也能切换。"));
    layout->addWidget(row);
}

void FinishPage::create_desktop_shortcut() {
    const QString sandbox = test_sandbox_dir();
    const QString desktop = sandbox.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DesktopLocation) : QDir(sandbox).filePath("Desktop");
    const QString desktop_lnk = QDir(desktop).filePath(DESKTOP_LNK_NAME);
    const QString target_path = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const QString working_directory = QDir::toNativeSeparators(QFileInfo(target_path).absolutePath());
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellLinkW *link = nullptr;
    IPersistFile *file = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (SUCCEEDED(hr)) {
        link->SetPath(reinterpret_cast<LPCWSTR>(target_path.utf16()));
        link->SetWorkingDirectory(reinterpret_cast<LPCWSTR>(working_directory.utf16()));
        link->SetIconLocation(reinterpret_cast<LPCWSTR>(target_path.utf16()), 0);
        hr = link->QueryInterface(IID_PPV_ARGS(&file));
    }
    if (SUCCEEDED(hr)) hr = file->Save(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(desktop_lnk).utf16()), TRUE);
    if (file) file->Release();
    if (link) link->Release();
    if (SUCCEEDED(init)) CoUninitialize();
    if (SUCCEEDED(hr)) log_info(T("已创建桌面快捷方式: {desktop_lnk}", {{"desktop_lnk", QDir::toNativeSeparators(desktop_lnk)}}), "page6");
    else log_exception(QString("IShellLink failed: 0x%1").arg(static_cast<quint32>(hr), 8, 16, QChar('0')), T("创建桌面快捷方式"));
}

void FinishPage::retranslate() {
    title_label->setText(page_title());
    subtitle_label->setText(page_subtitle());
    autostart_lbl_->setText(welcome_tr("开机自启"));
    if (autostart_desc_) autostart_desc_->setText(welcome_tr("开机后在检测更新后启动。"));
    show_main_lbl_->setText(welcome_tr("启动时显示主界面"));
    if (show_main_desc_) show_main_desc_->setText(welcome_tr("每次启动时自动打开设置面板。"));
    desktop_lbl_->setText(welcome_tr("快捷方式"));
    if (desktop_desc_) desktop_desc_->setText(brand_text(welcome_tr("完成向导时在桌面创建截图吧快捷方式。")));
    game_pause_lbl_->setText(welcome_tr("玩游戏时暂停热键"));
    if (game_pause_desc_) game_pause_desc_->setText(welcome_tr("游戏在前台时暂停全局快捷键和鼠标快捷键，离开游戏后恢复。托盘菜单里也能切换。"));
}

void FinishPage::save_preferences() {
    config_->set_welcome_wizard_done("1");
    config_->set_show_main_window(show_main_switch_->isChecked());
    config_->set_pause_hotkeys_in_games(game_pause_switch_->isChecked());
}

void FinishPage::save() {
    save_preferences();
    const bool desktop_on = desktop_switch_->isChecked() && !is_packaged();
    const bool run_as_admin = config_->get_run_as_admin();
    // Without elevation the wizard cannot remove the administrator task, which keeps starting the
    // program at sign-in, so autostart stays on instead of being stored as off.
    const bool autostart_on = autostart_switch_->isChecked() || (run_as_admin && elevation::elevation_state() != elevation::ELEVATED);
    config_->set_autostart_enabled(autostart_on);
    // File and network path work runs in the background so UNC probing never blocks the UI thread.
    std::thread([autostart_on, desktop_on, run_as_admin] {
        run_thread_guarded("WelcomeFinish", [&] {
            elevation::apply_startup(autostart_on, run_as_admin, run_as_admin);
            if (desktop_on) create_desktop_shortcut();
        });
    }).detach();
}
}
