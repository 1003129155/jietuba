#include "core/constants.h"
#include "core/crash_handler.h"
#include "core/elevation.h"
#include "core/logger.h"
#include "core/platform_utils.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "settings/tool_settings.h"
#include "ui/welcome/pages.h"
#include <QCoreApplication>
#include <QDir>
#include <QEasingCurve>
#include <QElapsedTimer>
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
constexpr double PI = 3.14159265358979323846;

// A one-time completion effect: a filled circle, a drawn check, two ripples and a few sparks.
class CheckAnim : public QWidget {
public:
    static constexpr int DURATION_MS = 1120;
    explicit CheckAnim(QWidget *parent = nullptr) : QWidget(parent) {
        set_own_style(this, "background: transparent;");
        timer_.setTimerType(Qt::PreciseTimer);
        connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
    }
    void play() {
        time_ms_ = 0;
        elapsed_.restart();
        timer_.start(16);
        update();
    }
protected:
    void showEvent(QShowEvent *event) override {
        QWidget::showEvent(event);
        // Plays when the page really becomes visible, not during the wizard's construction.
        if (has_been_shown_) {
            play();
        } else {
            has_been_shown_ = true;
            QTimer::singleShot(90, this, [this] { play(); });
        }
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const double w = width(), h = height();
        const auto theme = welcome_theme();
        const QPointF center(w / 2.0, h / 2.0);
        const double now = time_ms_;
        const double base_r = std::min(44.0, std::max(34.0, std::min(w, h) * 0.13));
        const double circle_scale = ease(QEasingCurve::OutBack, progress(now, 70, 410));
        const double check_t = ease(QEasingCurve::OutCubic, progress(now, 330, 690));
        const double effects_t = progress(now, 560, 1040);

        // The soft glow that remains, filling the space without becoming another large circle.
        QColor glow(theme.accent);
        glow.setAlpha(theme.is_dark ? 12 : 9);
        p.setPen(Qt::NoPen);
        p.setBrush(glow);
        p.drawEllipse(center, base_r * 1.9, base_r * 1.9);

        // Two confirmation ripples, once.
        for (const auto &[delay, max_scale] : {std::make_pair(0.0, 1.85), std::make_pair(0.17, 2.15)}) {
            const double wave_t = std::max(0.0, std::min(1.0, (effects_t - delay) / (1.0 - delay)));
            if (0.0 < wave_t && wave_t < 1.0) {
                const double wave_r = base_r * (1.0 + (max_scale - 1.0) * wave_t);
                QColor wave_color(theme.accent);
                wave_color.setAlpha(static_cast<int>(54 * std::pow(1.0 - wave_t, 2)));
                p.setPen(QPen(wave_color, 1.5));
                p.setBrush(Qt::NoBrush);
                p.drawEllipse(center, wave_r, wave_r);
            }
        }

        // A few sparks of the same colour drift a short way from the badge and fade.
        if (0.0 < effects_t && effects_t < 1.0) {
            QColor spark_color(theme.accent);
            spark_color.setAlpha(static_cast<int>(175 * std::sin(PI * effects_t)));
            p.setPen(Qt::NoPen);
            p.setBrush(spark_color);
            const int angles[] = {-148, -92, -34, 20, 78, 142};
            for (int index = 0; index < 6; ++index) {
                const double angle = angles[index] * PI / 180.0;
                const double distance = base_r * (1.28 + effects_t * (0.30 + index % 2 * 0.10));
                const QPointF point(center.x() + std::cos(angle) * distance, center.y() + std::sin(angle) * distance);
                const double dot_r = index % 2 ? 1.8 : 2.4;
                p.drawEllipse(point, dot_r, dot_r);
            }
        }

        // The small filled badge.
        const double radius = base_r * circle_scale;
        if (radius > 0.1) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(theme.accent));
            p.drawEllipse(center, radius, radius);
        }

        // The white check, drawn along its path in two strokes.
        if (check_t > 0.0 && circle_scale > 0.0) {
            const double r = base_r;
            const QPointF start(center.x() - r * 0.45, center.y() - r * 0.01);
            const QPointF corner(center.x() - r * 0.10, center.y() + r * 0.35);
            const QPointF end(center.x() + r * 0.50, center.y() - r * 0.34);
            QPainterPath check_path(start);
            const double first_part = 0.37;
            if (check_t <= first_part) {
                const double frac = check_t / first_part;
                check_path.lineTo(start.x() + (corner.x() - start.x()) * frac, start.y() + (corner.y() - start.y()) * frac);
            } else {
                check_path.lineTo(corner);
                const double frac = (check_t - first_part) / (1.0 - first_part);
                check_path.lineTo(corner.x() + (end.x() - corner.x()) * frac, corner.y() + (end.y() - corner.y()) * frac);
            }
            QPen check_pen(QColor("#FFFFFF"), std::max(4.0, base_r * 0.105));
            check_pen.setCapStyle(Qt::RoundCap);
            check_pen.setJoinStyle(Qt::RoundJoin);
            p.setPen(check_pen);
            p.setBrush(Qt::NoBrush);
            p.drawPath(check_path);
        }
    }
private:
    static double progress(double now, double start, double end) { return end <= start ? 1.0 : std::max(0.0, std::min(1.0, (now - start) / (end - start))); }
    static double ease(QEasingCurve::Type curve, double value) { return QEasingCurve(curve).valueForProgress(value); }
    void tick() {
        time_ms_ = std::min<qint64>(DURATION_MS, elapsed_.elapsed());
        if (time_ms_ >= DURATION_MS) timer_.stop();
        update();
    }
    QElapsedTimer elapsed_;
    qint64 time_ms_ = DURATION_MS;
    bool has_been_shown_ = false;
    QTimer timer_;
};

class FinishIllus : public IllustrationArea {
public:
    using IllustrationArea::IllustrationArea;
    // The badge floats on the page rather than inside a large empty card.
    void apply_welcome_theme() override {
        setStyleSheet(R"(
            #IllustrationArea {
                background: transparent;
                border: none;
            }
        )");
        update();
    }
protected:
    void build_content() override {
        auto *anim = new CheckAnim(this);
        anim->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        layout_->addWidget(anim);
    }
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
    else log_exception("OSError", QString("IShellLink failed: 0x%1").arg(static_cast<quint32>(hr), 8, 16, QChar('0')), T("创建桌面快捷方式"));
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
    const bool desktop_on = desktop_switch_->isChecked();
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
