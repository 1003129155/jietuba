#include "core/constants.h"
#include "core/logger.h"
#include "core/resource_manager.h"
#include "core/update_checker.h"
#include "core/update_controller.h"
#include "core/updater_process.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "main_app.h"
#include "ui/dialogs.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/theme.h"
#include "ui/settings_ui/components.h"
#include "ui/settings_ui/dialog.h"
#include "ui/settings_ui/pages.h"

#include <QDesktopServices>
#include <QFile>
#include <QPainter>
#include <QPainterPath>
#include <QSvgRenderer>
#include <QUrl>

namespace jietuba {
namespace {
constexpr int MARK_TILE = 112;
constexpr int MARK_RADIUS = 26;
constexpr int MARK_GLYPH = 66;
// Room around the tile for its shadow.
constexpr int MARK_SHADOW = 8;

// The application mark on a rounded tile. The mark is drawn in the text colour and its body in the
// tile's colour, so it reads as the same line drawing in either theme.
class AppMarkTile : public QWidget {
public:
    explicit AppMarkTile(QWidget *parent) : QWidget(parent) {
        setObjectName("AboutAppMark");
        setFixedSize(dialog_scaled(MARK_TILE + 2 * MARK_SHADOW), dialog_scaled(MARK_TILE + 2 * MARK_SHADOW));
        QFile file(ResourceManager::get_resource_path(QString::fromUtf8("svg/托盘.svg")));
        if (file.open(QIODevice::ReadOnly)) svg_ = file.readAll();
        connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { update(); });
    }

protected:
    void paintEvent(QPaintEvent *) override {
        const bool dark = get_ui_theme()->tokens().is_dark();
        const QColor fill = dark ? QColor("#2D2D2D") : QColor(Qt::white);
        const QColor ink = dark ? QColor(Qt::white) : QColor("#1B1B1B");
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const qreal margin = dialog_scaled(MARK_SHADOW);
        const QRectF tile = QRectF(rect()).adjusted(margin, margin, -margin, -margin);
        const qreal radius = dialog_scaled(MARK_RADIUS);
        painter.setPen(Qt::NoPen);
        for (int step = MARK_SHADOW; step >= 1; --step) {
            const qreal grow = dialog_scaled(step) * 0.75;
            painter.setBrush(QColor(0, 0, 0, dark ? 9 : 3));
            painter.drawRoundedRect(tile.adjusted(-grow, -grow + dialog_scaled(2), grow, grow + dialog_scaled(2)), radius + grow, radius + grow);
        }
        painter.setBrush(fill);
        painter.setPen(QPen(dark ? QColor(255, 255, 255, 20) : QColor(0, 0, 0, 18), 1));
        painter.drawRoundedRect(tile.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
        if (svg_.isEmpty()) return;
        QByteArray svg = svg_;
        svg.replace("currentColor", ink.name().toUtf8());
        svg.replace("fill=\"white\"", QByteArray("fill=\"") + fill.name().toUtf8() + "\"");
        QSvgRenderer renderer(svg);
        renderer.setAspectRatioMode(Qt::KeepAspectRatio);
        const qreal glyph = dialog_scaled(MARK_GLYPH);
        QRectF target(0, 0, glyph, glyph);
        target.moveCenter(tile.center());
        renderer.render(&painter, target);
    }

private:
    QByteArray svg_;
};

// The version on a soft rounded chip.
class VersionChip : public QLabel {
public:
    VersionChip(const QString &text, QWidget *parent) : QLabel(text, parent) {
        setObjectName("AboutVersion");
        setAlignment(Qt::AlignCenter);
        apply_theme_text_style(this, 13, false, QString("font-family: %1; padding: %2px %3px;").arg(FONT_FAMILY).arg(dialog_scaled(2)).arg(dialog_scaled(11)), true);
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(get_ui_theme()->tokens().is_dark() ? QColor(255, 255, 255, 20) : QColor(0, 0, 0, 14));
        const qreal radius = height() / 2.0;
        painter.drawRoundedRect(QRectF(rect()), radius, radius);
        painter.end();
        QLabel::paintEvent(event);
    }
};

void connect_update_check(SettingsDialog *dialog, QPushButton *check_update_btn) {
    auto *release_checker = new GitHubReleaseChecker(dialog);
    auto finish_check = [dialog, check_update_btn] {
        check_update_btn->setEnabled(true);
        check_update_btn->setText(dialog->tr_text("Check for Updates"));
    };
    auto show_check_error = [dialog, finish_check](const QString &reason) {
        finish_check();
        log_warning(T("检查更新失败: {reason}", {{"reason", reason}}), "Update");
        show_warning_dialog(dialog, dialog->tr_text("Update Check Failed"),
                            dialog->tr_text("Unable to check for updates. Please check your network connection and try again."));
    };
    QObject::connect(release_checker, &GitHubReleaseChecker::release_found, dialog, [dialog, finish_check, show_check_error](const ReleaseInfo &release) {
        bool available = false;
        try {
            available = release.available.has_value() ? *release.available : is_newer_version(release.tag_name, APP_VERSION);
        } catch (const UpdateCheckError &e) {
            show_check_error(QString::fromStdString(e.what()));
            return;
        }
        finish_check();
        if (!available) {
            show_info_dialog(dialog, dialog->tr_text("Update Check"), dialog->tr_text("You're using the latest version (%1).").replace("%1", APP_VERSION));
            return;
        }
        auto *controller = qobject_cast<UpdateController *>(qApp->property("_update_controller").value<QObject *>());
        if (controller && !release.asset_name.isEmpty()) {
            controller->present(release);
            return;
        }
        const QString notes = release.notes.isEmpty() ? dialog->tr_text("No release notes provided.") : release.notes;
        const QString content = QStringList{dialog->tr_text("Current version: %1").replace("%1", APP_VERSION),
                                            dialog->tr_text("Latest version: %1").replace("%1", release.tag_name), QString(),
                                            dialog->tr_text("What's new:"), notes}
                                    .join('\n');
        show_update_dialog(dialog, dialog->tr_text("Update Available"), content, dialog->tr_text("Download:"), release.url,
                           dialog->tr_text("Open Download Page"));
    });
    QObject::connect(release_checker, &GitHubReleaseChecker::failed, dialog, show_check_error);
    QObject::connect(check_update_btn, &QPushButton::clicked, dialog, [dialog, release_checker, check_update_btn] {
        if (!release_checker->check()) return;
        check_update_btn->setEnabled(false);
        check_update_btn->setText(dialog->tr_text("Checking..."));
    });
}

// Spelt out like the page titles: these labels do not sit in a card that sets it.
QString font_family() { return QString("font-family: %1;").arg(FONT_FAMILY); }

QLabel *centred_label(const QString &text, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    label->setAlignment(Qt::AlignHCenter);
    label->setWordWrap(true);
    return label;
}
}

// The information page has no title or groups: the application's mark, name and version sit in the
// middle, with the update check and the links under them.
QWidget *create_about_page(SettingsDialog *dialog) {
    auto *scroll = page_scroll_area(dialog);
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet("QScrollArea { border: none; background: transparent; }");
    auto *view = new QWidget;
    set_own_style(view, "background: transparent;");
    auto *layout = new QVBoxLayout(view);
    layout->setContentsMargins(0, dialog_scaled(80), dialog_scaled(10), page_bottom_margin());
    layout->setSpacing(0);

    layout->addWidget(new AppMarkTile(view), 0, Qt::AlignHCenter);
    layout->addSpacing(dialog_scaled(14));
    auto *name = centred_label("jietuba", view);
    name->setObjectName("AboutName");
    apply_theme_text_style(name, 30, true, font_family());
    layout->addWidget(name);
    layout->addSpacing(dialog_scaled(4));
    auto *tagline = centred_label(dialog->tr_text("Screenshot, Screen Drawing, OCR, Pin, Translation & Clipboard Tool for Windows"), view);
    apply_theme_text_style(tagline, 14, false, font_family(), true);
    layout->addWidget(tagline);
    layout->addSpacing(dialog_scaled(14));
    layout->addWidget(new VersionChip(QString("%1 %2").arg(dialog->tr_text("Version"), APP_VERSION), view), 0, Qt::AlignHCenter);
    layout->addSpacing(dialog_scaled(26));

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(dialog_scaled(8));
    buttons->addStretch(1);
    // Builds without the bundled updater helper (JIETUBA_UPDATES off) offer no update check.
    if (updater_bundled()) {
        // Pre-tinted: the theme tint would draw it in the text colour on the accent fill.
        auto *check_update_btn = new PrimaryPushButton(dialog->tr_text("Check for Updates"), view, tinted_icon(FluentIcon::Sync, "#FFFFFF"));
        check_update_btn->setObjectName("checkUpdateButton");
        connect_update_check(dialog, check_update_btn);
        buttons->addWidget(check_update_btn);
    }
    auto *source_btn = new PushButton(dialog->tr_text("Source Code"), view, FluentIcon::GitHub);
    source_btn->setObjectName("aboutSourceButton");
    QObject::connect(source_btn, &QPushButton::clicked, dialog, [] { QDesktopServices::openUrl(QUrl(PROJECT_GITHUB_URL)); });
    buttons->addWidget(source_btn);
    auto *license_btn = new PushButton(dialog->tr_text("Licenses"), view, FluentIcon::Certificate);
    license_btn->setObjectName("aboutLicenseButton");
    QObject::connect(license_btn, &QPushButton::clicked, dialog, [dialog] { show_text_dialog(dialog, dialog->tr_text("MIT License"), dialog->tr_text("MIT License Text")); });
    buttons->addWidget(license_btn);
    auto *feedback_btn = new PushButton(dialog->tr_text("Feedback"), view, FluentIcon::Edit);
    feedback_btn->setObjectName("aboutFeedbackButton");
    QObject::connect(feedback_btn, &QPushButton::clicked, dialog, [] { QDesktopServices::openUrl(QUrl(PROJECT_GITHUB_URL + "/issues")); });
    buttons->addWidget(feedback_btn);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    layout->addStretch(1);
    auto *credit = centred_label(dialog->tr_text("Developed by %1 · Open source under the MIT License").replace("%1", "JYAARU"), view);
    apply_theme_text_style(credit, 12, false, font_family(), true);
    layout->addWidget(credit);
    const QString star_text = QStringLiteral("☆ %1").arg(dialog->tr_text("Enjoying jietuba? Give it a Star"));
    auto *link = new HyperlinkButton(PROJECT_GITHUB_URL, star_text, view);
    link->setObjectName("aboutRepositoryLink");
    layout->addWidget(link, 0, Qt::AlignHCenter);

    scroll->setWidget(view);
    return scroll;
}
}
