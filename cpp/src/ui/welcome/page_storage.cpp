#include "clipboard/core/manager.h"
#include "core/data_location.h"
#include "core/i18n.h"
#include "core/ui_scale.h"
#include "settings/data_folder.h"
#include "settings/tool_settings.h"
#include "ui/dialogs.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/icons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/settings_ui/pages.h"
#include "ui/fluent_lite/cards.h"
#include "ui/welcome/demo_paint.h"
#include "ui/welcome/pages.h"
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <cmath>
#include <functional>

namespace jietuba {
namespace {
QString settings_text(const char *text) { return i18n_tr(text, "SettingsDialog"); }
QString page_subtitle() { return welcome_tr("截图、剪贴板记录和设置各自存在哪里，之后也可以在设置里改。"); }

QString clipboard_folder(const ToolSettingsManager *config) {
    const QString db = clipboard::ClipboardManager::instance().get_db_path().value_or(config->get_clipboard_db_path());
    return QDir::toNativeSeparators(QFileInfo(db).absolutePath());
}

// A path shown in full as a tooltip: the field is narrower than most paths.
LineEdit *path_field(const QString &path, bool editable) {
    auto *edit = new LineEdit;
    edit->setText(path);
    edit->setToolTip(path);
    edit->setReadOnly(!editable);
    edit->setFixedHeight(dialog_scaled(30));
    edit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return edit;
}

PushButton *path_button(const QString &text, FluentIcon icon) {
    auto *button = new PushButton(text, nullptr, icon);
    button->setFixedHeight(dialog_scaled(30));
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

// The row's title stands alone; its control is the path row under it.
QWidget *no_control() {
    auto *widget = new QWidget;
    widget->setFixedSize(0, 0);
    return widget;
}

void add_under(QWidget *card, QWidget *field, QWidget *button) {
    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, dialog_scaled(4), 0, 0);
    row->setSpacing(dialog_scaled(6));
    row->addWidget(field, 1);
    if (button) row->addWidget(button);
    static_cast<QVBoxLayout *>(card->layout())->addLayout(row);
}
}

namespace {
using namespace welcome_demo;

// Where each thing is kept, on the hotkey page's desktop: a screenshot is confirmed and goes into
// its folder, the clipboard takes a new entry into its data folder, the settings window writes
// settings.ini. The three take turns with the cards beside them, which show the places.
// Drawn in 100% units under the window scale.
class StorageScene : public LoopingDemo {
public:
    enum Scene { SCREENSHOTS, CLIPBOARD, SETTINGS };
    static constexpr int SLOT_MS = 3000;
    // Each slot's moment the thing leaves for its folder, and how long it flies.
    static constexpr int SEND_MS[] = {1500, 1100, 1150};
    static constexpr int FLY_MS = 450;

    explicit StorageScene(QWidget *parent = nullptr) : LoopingDemo(SLOT_MS * 3, parent) { setMinimumHeight(dialog_scaled(260)); }
    int current_index() const { return time() / SLOT_MS % 3; }
    // The entry playing changed.
    std::function<void(int)> on_current;
protected:
    void time_changed() override {
        if (const int now = current_index(); now != reported_) {
            reported_ = now;
            if (on_current) on_current(now);
        }
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const double scale = demo_scale();
        p.scale(scale, scale);
        const double w = width() / scale, h = height() / scale;
        const auto theme = welcome_theme();
        const double screen_w = w - 8;
        const double screen_h = std::min(screen_w * 0.95, h - 8);
        const QRectF screen((w - screen_w) / 2, (h - screen_h) / 2, screen_w, screen_h);
        draw_screen(p, theme, screen);

        const int index = current_index();
        const int t = time() % SLOT_MS;
        p.save();
        p.setOpacity(1.0 - phase(t, SLOT_MS - 350, SLOT_MS - 50));
        QPainterPath clip;
        clip.addRoundedRect(screen, 10, 10);
        p.setClipPath(clip);
        const QRectF folder = folder_rect(screen);
        switch (index) {
        case SCREENSHOTS: play_screenshots(p, theme, screen, folder, t); break;
        case CLIPBOARD: play_clipboard(p, theme, screen, folder, t); break;
        case SETTINGS: play_settings(p, theme, screen, folder, t); break;
        default: break;
        }
        // The folder waits in the corner and gives a little hop when something lands.
        const double landed = phase(t, SEND_MS[index] + FLY_MS, SEND_MS[index] + FLY_MS + 250);
        draw_folder(p, theme, folder, phase(t, 0, 250), landed > 0 && landed < 1 ? std::sin(landed * 3.14159265358979323846) : 0.0);
        p.restore();
    }
private:
    static QRectF folder_rect(const QRectF &screen) { return {screen.right() - 52, screen.bottom() - 46, 40, 34}; }

    static void draw_folder(QPainter &p, const WelcomeTheme &theme, const QRectF &box, double shown, double hop) {
        if (shown <= 0) return;
        p.save();
        p.setOpacity(p.opacity() * shown);
        const QRectF bubble = box.translated(0, -4 * hop);
        draw_shadow(p, theme, bubble, 8);
        p.setPen(QPen(QColor(theme.border), 0.8));
        p.setBrush(QColor(theme.panel));
        p.drawRoundedRect(bubble, 8, 8);
        demo_icon(FluentIcon::Folder).paint(&p, bubble.adjusted(10, 7, -10, -7).toRect());
        p.restore();
    }

    // Something shrinks and flies from its place to the folder.
    template <typename Paint>
    static void fly(QPainter &p, const QRectF &from, const QRectF &folder, int t, int start, Paint paint) {
        const double k = ease(QEasingCurve::InCubic, phase(t, start, start + FLY_MS));
        if (k >= 1) return;
        const QPointF centre = lerp(from.center(), folder.center(), k);
        const double size = 1.0 - 0.85 * k;
        const QRectF now(centre.x() - from.width() * size / 2, centre.y() - from.height() * size / 2, from.width() * size, from.height() * size);
        p.save();
        p.setOpacity(p.opacity() * (1.0 - 0.3 * k));
        paint(now);
        p.restore();
    }

    // A screenshot of the chart, confirmed, goes into the screenshot folder.
    static void play_screenshots(QPainter &p, const WelcomeTheme &theme, const QRectF &screen, const QRectF &folder, int t) {
        const int confirm_at = SEND_MS[SCREENSHOTS] - 250;
        if (t < SEND_MS[SCREENSHOTS]) {
            draw_capture(p, theme, screen, t, t >= confirm_at ? static_cast<int>(capture_tools().size()) - 1 : -1);
            return;
        }
        fly(p, chart_region(screen), folder, t, SEND_MS[SCREENSHOTS], [&](const QRectF &box) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(theme.panel));
            p.drawRect(box);
            draw_chart(p, theme, box.adjusted(box.width() * 0.04, box.height() * 0.05, -box.width() * 0.04, -box.height() * 0.05));
            p.setPen(QPen(theme_color(), 1.4));
            p.setBrush(Qt::NoBrush);
            p.drawRect(box);
        });
    }

    // The clipboard takes a new entry and keeps it in its data folder.
    static void play_clipboard(QPainter &p, const WelcomeTheme &theme, const QRectF &screen, const QRectF &folder, int t) {
        const double in = ease(QEasingCurve::OutCubic, phase(t, 0, 300));
        const QRectF win = clipboard_window_rect(screen).translated(-screen.width() * 0.12, 0);
        p.save();
        p.setOpacity(p.opacity() * in);
        draw_clipboard_window(p, theme, win.translated(0, 8 * (1 - in)), ease(QEasingCurve::OutCubic, phase(t, 500, 800)));
        p.restore();
        const QRectF row(win.x() + 4, win.y() + 4, win.width() - 24, 15);
        fly(p, row, folder, t, SEND_MS[CLIPBOARD], [&](const QRectF &box) {
            p.setPen(Qt::NoPen);
            p.setBrush(with_alpha(theme.accent, 120));
            p.drawRoundedRect(box, 3, 3);
        });
    }

    // The settings window: its sidebar with the feature icons and a card of switches. A switch
    // flips, and settings.ini goes to the folder that keeps it.
    static void play_settings(QPainter &p, const WelcomeTheme &theme, const QRectF &screen, const QRectF &folder, int t) {
        const double in = ease(QEasingCurve::OutCubic, phase(t, 0, 300));
        const double win_w = screen.width() * 0.74, win_h = screen.height() * 0.62;
        const QRectF win(screen.x() + screen.width() * 0.07, screen.y() + screen.height() * 0.1 + 8 * (1 - in), win_w, win_h);
        p.save();
        p.setOpacity(p.opacity() * in);
        draw_shadow(p, theme, win, 6);
        p.setPen(QPen(QColor(theme.border), 0.8));
        p.setBrush(QColor(theme.sidebar));
        p.drawRoundedRect(win, 6, 6);
        // The sidebar: the logo, then the pages with their feature colours, the first current.
        const double nav_w = win.width() * 0.3;
        p.setPen(Qt::NoPen);
        p.setBrush(with_alpha(theme.text, 150));
        p.drawRoundedRect(QRectF(win.x() + 7, win.y() + 7, 7, 7), 2, 2);
        p.drawRoundedRect(QRectF(win.x() + 17, win.y() + 9.5, 18, 2.5), 1.2, 1.2);
        const char *tones[] = {"neutral", "neutral", "capture", "pin", "clipboard", "translate"};
        for (int i = 0; i < 6; ++i) {
            const QRectF item(win.x() + 4, win.y() + 20 + i * 11, nav_w - 8, 9);
            if (i == 0) {
                p.setBrush(with_alpha(theme.accent, theme.is_dark ? 70 : 40));
                p.drawRoundedRect(item, 3, 3);
                p.setBrush(QColor(theme.accent));
                p.drawRoundedRect(QRectF(item.x(), item.y() + 2, 1.5, 5), 0.7, 0.7);
            }
            const auto &tone = feature_tone(tones[i]);
            p.setBrush(QColor(theme.is_dark ? tone.dark_fg : tone.light_fg));
            p.drawRoundedRect(QRectF(item.x() + 4, item.y() + 2, 5, 5), 1.2, 1.2);
            p.setBrush(with_alpha(theme.text, 90));
            p.drawRoundedRect(QRectF(item.x() + 12, item.center().y() - 1, item.width() * (0.45 + 0.08 * (i % 3)), 2), 1, 1);
        }
        // The page: its title and a card of three rows with switches.
        const QRectF page(win.x() + nav_w, win.y() + 6, win.width() - nav_w - 6, win.height() - 12);
        p.setBrush(with_alpha(theme.text, 170));
        p.drawRoundedRect(QRectF(page.x() + 2, page.y() + 2, page.width() * 0.4, 3.5), 1.7, 1.7);
        const QRectF card(page.x(), page.y() + 12, page.width(), page.height() - 14);
        p.setPen(QPen(QColor(theme.border), 0.8));
        p.setBrush(QColor(theme.panel));
        p.drawRoundedRect(card, 5, 5);
        const char *row_tones[] = {"capture", "capture", "neutral"};
        const double row_h = card.height() / 3;
        const double flip = ease(QEasingCurve::OutCubic, phase(t, 600, 800));
        for (int row = 0; row < 3; ++row) {
            const QRectF line(card.x() + 5, card.y() + row * row_h, card.width() - 10, row_h);
            if (row) {
                p.setPen(QPen(QColor(theme.separator), 0.6));
                p.drawLine(QPointF(line.x() + 12, line.top()), QPointF(line.right(), line.top()));
            }
            p.setPen(Qt::NoPen);
            const auto &tone = feature_tone(row_tones[row]);
            p.setBrush(QColor(theme.is_dark ? tone.dark_bg : tone.light_bg));
            p.drawRoundedRect(QRectF(line.x(), line.center().y() - 4.5, 9, 9), 2.2, 2.2);
            p.setBrush(with_alpha(theme.text, 110));
            p.drawRoundedRect(QRectF(line.x() + 13, line.center().y() - 3, line.width() * 0.4, 2.2), 1.1, 1.1);
            p.setBrush(with_alpha(theme.text_soft, 90));
            p.drawRoundedRect(QRectF(line.x() + 13, line.center().y() + 1.5, line.width() * 0.3, 1.8), 0.9, 0.9);
            // The switch; the middle one flips on.
            const double on = row == 0 ? 1.0 : row == 1 ? flip : 0.0;
            const QRectF track(line.right() - 16, line.center().y() - 3.5, 14, 7);
            p.setBrush(on > 0.5 ? QColor(theme.accent) : with_alpha(theme.text_soft, 90));
            p.drawRoundedRect(track, 3.5, 3.5);
            p.setBrush(QColor("#FFFFFF"));
            p.drawEllipse(QPointF(track.x() + 3.5 + 7 * on, track.center().y()), 2.5, 2.5);
        }
        p.restore();
        fly(p, QRectF(card.right() - 34, card.bottom() - 18, 28, 14), folder, t, SEND_MS[SETTINGS], [&](const QRectF &box) {
            p.setPen(QPen(QColor(theme.border_strong), 0.8));
            p.setBrush(QColor(theme.panel));
            p.drawRoundedRect(box, 2.5, 2.5);
            demo_icon(FluentIcon::Document).paint(&p, QRectF(box.x() + 2, box.center().y() - box.height() * 0.3, box.height() * 0.6, box.height() * 0.6).toRect());
        });
    }

    int reported_ = -2;
};

class StorageIllus : public IllustrationArea {
public:
    using IllustrationArea::IllustrationArea;
    void apply_welcome_theme() override {
        IllustrationArea::apply_welcome_theme();
        if (scene) scene->update();
    }
    StorageScene *scene = nullptr;
protected:
    void build_content() override {
        layout_->setContentsMargins(dialog_scaled(22), dialog_scaled(22), dialog_scaled(22), dialog_scaled(22));
        scene = new StorageScene(this);
        layout_->addWidget(scene, 1);
    }
};
}

StoragePage::StoragePage(ToolSettingsManager *config, QWidget *parent) : BasePage(welcome_tr("存储位置"), page_subtitle(), parent), config_(config) {
    init();
}

IllustrationArea *StoragePage::create_illustration() { return new StorageIllus(this); }

void StoragePage::build_controls(QVBoxLayout *layout) {
    layout->setSpacing(dialog_scaled(8));
    // ── Screenshots: whether to save, then where. ──
    autosave_toggle_ = new WelcomeToggleSwitch;
    autosave_toggle_->setChecked(config_->get_screenshot_save_enabled());
    QWidget *&screenshot_card = cards_[0];
    std::tie(screenshot_card, autosave_lbl_, autosave_desc_) =
        make_setting_row_with_refs(welcome_tr("自动保存截图"), autosave_toggle_, welcome_tr("关闭时截图只复制到剪贴板，不落盘。"));
    path_edit_ = path_field(QDir::toNativeSeparators(config_->get_screenshot_save_path()), true);
    // The same word as the clipboard folder's: to the user both change a folder.
    browse_btn_ = path_button(settings_text("Change"), FluentIcon::Folder);
    connect(browse_btn_, &QPushButton::clicked, this, [this] {
        const QString folder = get_existing_directory(this, welcome_tr("选择保存文件夹"), path_edit_->text().trimmed());
        if (folder.isEmpty()) return;
        path_edit_->setText(QDir::toNativeSeparators(folder));
        path_edit_->setToolTip(path_edit_->text());
    });
    add_under(screenshot_card, path_edit_, browse_btn_);
    // The folder means nothing with saving off, so it greys out rather than mislead.
    const auto sync_save = [this](bool on) {
        path_edit_->setEnabled(on);
        browse_btn_->setEnabled(on);
    };
    connect(autosave_toggle_, &SwitchButton::checkedChanged, this, sync_save);
    sync_save(autosave_toggle_->isChecked());
    layout->addWidget(screenshot_card);

    // ── Clipboard data: moved at once, as in the settings window. ──
    QWidget *&clipboard_card = cards_[1];
    std::tie(clipboard_card, clipboard_lbl_, clipboard_desc_) =
        make_setting_row_with_refs(welcome_tr("剪贴板数据"), no_control(), welcome_tr("剪贴板历史的数据库和图片都放在这个文件夹。"));
    clipboard_edit_ = path_field(clipboard_folder(config_), false);
    change_btn_ = path_button(settings_text("Change"), FluentIcon::Folder);
    connect(change_btn_, &QPushButton::clicked, this, [this] {
        QPointer<StoragePage> guard(this);
        change_clipboard_data_location(this, [guard](const QString &db) {
            if (!guard) return;
            guard->clipboard_edit_->setText(QDir::toNativeSeparators(QFileInfo(db).absolutePath()));
            guard->clipboard_edit_->setToolTip(guard->clipboard_edit_->text());
        });
    });
    add_under(clipboard_card, clipboard_edit_, change_btn_);
    layout->addWidget(clipboard_card);

    // ── Settings: green mode moves the file at once. ──
    green_toggle_ = new WelcomeToggleSwitch;
    green_toggle_->setChecked(data_location().beside_program);
    green_writable_ = data_location().beside_program || data_folder::folder_writable(data_folder::green_mode_dir());
    green_toggle_->setEnabled(green_writable_);
    QWidget *&green_card = cards_[2];
    std::tie(green_card, green_lbl_, green_desc_) = make_setting_row_with_refs(settings_text("Green Mode"), green_toggle_, green_description());
    settings_edit_ = path_field(QDir::toNativeSeparators(data_location().settings_file()), false);
    add_under(green_card, settings_edit_, nullptr);
    connect(green_toggle_, &SwitchButton::checkedChanged, this, [this](bool on) {
        const QString error = data_folder::set_green_mode(*config_, on);
        if (!error.isEmpty()) {
            const QSignalBlocker block(green_toggle_);
            green_toggle_->setChecked(!on);
            show_warning_dialog(this, settings_text("Error"), settings_text("Could not move the settings file: %1").replace("%1", error));
            return;
        }
        settings_edit_->setText(QDir::toNativeSeparators(data_location().settings_file()));
        settings_edit_->setToolTip(settings_edit_->text());
    });
    layout->addWidget(green_card);

    // The card the stage is showing takes the stage's colour.
    if (auto *scene = static_cast<StorageIllus *>(illus_area)->scene)
        scene->on_current = [this](int current) {
            for (int i = 0; i < 3; ++i)
                if (cards_[i]) set_setting_row_current(cards_[i], i == current);
        };
}

QString StoragePage::green_description() const {
    QString text = settings_text("Keep the settings in the program's folder; copy the whole folder to take them along.");
    if (!green_writable_) text += "\n" + settings_text("The program's folder cannot be written to.");
    return text;
}

void StoragePage::retranslate() {
    title_label->setText(welcome_tr("存储位置"));
    subtitle_label->setText(page_subtitle());
    autosave_lbl_->setText(welcome_tr("自动保存截图"));
    autosave_desc_->setText(welcome_tr("关闭时截图只复制到剪贴板，不落盘。"));
    browse_btn_->setText(settings_text("Change"));
    clipboard_lbl_->setText(welcome_tr("剪贴板数据"));
    clipboard_desc_->setText(welcome_tr("剪贴板历史的数据库和图片都放在这个文件夹。"));
    change_btn_->setText(settings_text("Change"));
    green_lbl_->setText(settings_text("Green Mode"));
    green_desc_->setText(green_description());
}

void StoragePage::save() {
    config_->set_screenshot_save_enabled(autosave_toggle_->isChecked());
    const QString path = path_edit_->text().trimmed();
    if (!path.isEmpty()) {
        config_->set_screenshot_save_path(path);
        QDir().mkpath(path);
    }
}
}
