#include "ui/welcome/base_page.h"
#include "core/i18n.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <tuple>

namespace jietuba {
QString welcome_tr(const char *text) { return i18n_tr(text, "WelcomeWizard"); }

namespace {
QString mix(const QString &base, const QString &tint, double amount) {
    const QColor a(base), b(tint);
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * amount, a.greenF() + (b.greenF() - a.greenF()) * amount, a.blueF() + (b.blueF() - a.blueF()) * amount).name();
}
}

WelcomeTheme welcome_theme() {
    const auto &tokens = get_ui_theme()->tokens();
    if (tokens.is_dark())
        return {"#23272C", "#2B3036", "#252A30", "#262C32", "#20252A", "#3C444D", "#4B5661", "#363E47",
                "#F2F4F6", "#AEB7C1", "#87919C", tokens.accent, tokens.accent_hover, "#31404C", "#171C21", "#303840", true, mix("#23272C", tokens.accent, 0.16)};
    return {"#FFFFFF", "#FFFFFF", "#F7F9FB", "#F3F7FA", "#F5F8FA", "#DDE5EC", "#C9D6E0", "#E5EBF0",
            "#172331", "#667587", "#8795A4", tokens.accent, tokens.accent_hover, "#E9F0F5", "#233342", "#EAF0F4", false, mix("#FFFFFF", tokens.accent, 0.18)};
}

QString brand_text(const QString &text) {
    QString result = text;
    for (const char *legacy_name : {"截图吧", "JieTuBa", "JieTuba", "JIETUBA"}) result.replace(QString::fromUtf8(legacy_name), PRODUCT_NAME);
    return result;
}

QLabel *set_welcome_label_style(QLabel *label, const QString &role, int font_size, int weight, const QString &extra) {
    label->setProperty("welcomeTextRole", role);
    label->setProperty("welcomeFontSize", font_size);
    label->setProperty("welcomeFontWeight", weight);
    label->setProperty("welcomeStyleExtra", extra);
    apply_welcome_label_style(label);
    return label;
}

void apply_welcome_label_style(QLabel *label) {
    const QString role = label->property("welcomeTextRole").toString();
    if (role.isEmpty()) return;
    const auto theme = welcome_theme();
    const QString color = role == "muted" ? theme.text_muted : role == "soft" ? theme.text_soft : role == "accent" ? theme.accent : theme.text;
    int size = label->property("welcomeFontSize").toInt() ? label->property("welcomeFontSize").toInt() : 13;
    // Body text reads one size larger than the base sizes pages pass; page titles keep theirs.
    if (size < 20) ++size;
    const int weight = label->property("welcomeFontWeight").toInt() ? label->property("welcomeFontWeight").toInt() : 400;
    const QString extra = label->property("welcomeStyleExtra").toString();
    set_own_style(label, QString("font-size: %1px; font-weight: %2; color: %3; background: transparent; border: none; %4").arg(dialog_scaled(size)).arg(weight).arg(color, extra));
}

WelcomeToggleSwitch::WelcomeToggleSwitch(QWidget *parent) : SwitchButton(parent) {
    connect(this, &SwitchButton::checkedChanged, this, &WelcomeToggleSwitch::toggled);
}

IllustrationArea::IllustrationArea(QWidget *parent) : QFrame(parent) {
    setObjectName("IllustrationArea");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(dialog_scaled(22), dialog_scaled(22), dialog_scaled(22), dialog_scaled(22));
    layout_->setSpacing(dialog_scaled(12));
    layout_->setAlignment(Qt::AlignCenter);
}

void IllustrationArea::init() {
    build_content();
    apply_welcome_theme();
}

void IllustrationArea::apply_welcome_theme() {
    const auto theme = welcome_theme();
    setStyleSheet(QString("#IllustrationArea { background: %1; border: none; border-radius: %2px; }").arg(theme.stage).arg(dialog_scaled(WELCOME_RADIUS)));
    update();
}

void IllustrationArea::paintEvent(QPaintEvent *event) {
    QFrame::paintEvent(event);
    // The corners of a capture selection frame the stage.
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QColor color(welcome_theme().accent);
    color.setAlpha(150);
    QPen pen(color, dialog_scaled_f(2));
    pen.setCapStyle(Qt::FlatCap);
    pen.setJoinStyle(Qt::MiterJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    const double inset = dialog_scaled_f(16), arm = dialog_scaled_f(18);
    const QRectF box = QRectF(rect()).adjusted(inset, inset, -inset, -inset);
    const std::tuple<QPointF, int, int> corners[] = {{box.topLeft(), 1, 1}, {box.topRight(), -1, 1}, {box.bottomLeft(), 1, -1}, {box.bottomRight(), -1, -1}};
    for (const auto &[corner, dx, dy] : corners) {
        QPainterPath path(corner + QPointF(0, dy * arm));
        path.lineTo(corner);
        path.lineTo(corner + QPointF(dx * arm, 0));
        p.drawPath(path);
    }
}

void IllustrationArea::set_pixmap(const QPixmap &pixmap, std::optional<QSize> max_size) {
    const QSize size = max_size.value_or(QSize(dialog_scaled(280), dialog_scaled(180)));
    auto *label = new QLabel(this);
    label->setAlignment(Qt::AlignCenter);
    label->setPixmap(pixmap.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    layout_->addWidget(label);
}

BasePage::BasePage(const QString &title, const QString &subtitle, QWidget *parent) : QWidget(parent) {
    setObjectName("BasePage");
    root_ = new QVBoxLayout(this);
    // The wizard's right column gives the margins.
    root_->setContentsMargins(0, 0, 0, 0);
    root_->setSpacing(dialog_scaled(22));

    // Header: what this step is about.
    auto *header = new QWidget(this);
    set_own_style(header, "background: transparent;");
    auto *header_layout = new QVBoxLayout(header);
    header_layout->setContentsMargins(0, 0, 0, 0);
    header_layout->setSpacing(dialog_scaled(7));
    if (!title.isEmpty()) {
        title_label = new QLabel(title, header);
        title_label->setWordWrap(true);
        set_welcome_label_style(title_label, "primary", 27, 700);
        header_layout->addWidget(title_label);
    }
    if (!subtitle.isEmpty()) {
        subtitle_label = new QLabel(subtitle, header);
        subtitle_label->setWordWrap(true);
        set_welcome_label_style(subtitle_label, "muted", 13, 400);
        header_layout->addWidget(subtitle_label);
    }
    root_->addWidget(header);
}

void BasePage::init() {
    // The stage sits beside the settings when the page stands alone; the wizard moves it to its
    // own left column.
    auto *body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(dialog_scaled(18));
    illus_area = create_illustration();
    illus_area->init();
    body->addWidget(illus_area, 4);

    content_widget_ = new QFrame(this);
    content_widget_->setObjectName("ContentPanel");
    content_widget_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    content_layout = new QVBoxLayout(content_widget_);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(dialog_scaled(10));
    build_controls(content_layout);
    content_layout->addStretch();
    body->addWidget(content_widget_, 5);
    root_->addLayout(body, 1);

    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { apply_welcome_theme(); });
    apply_welcome_theme();
}

void BasePage::apply_welcome_theme() {
    const auto theme = welcome_theme();
    setStyleSheet(QString("#BasePage { background: %1; }").arg(theme.page));
    content_widget_->setStyleSheet("#ContentPanel { background: transparent; border: none; }");
    illus_area->apply_welcome_theme();
    for (auto *label : findChildren<QLabel *>()) apply_welcome_label_style(label);
    for (auto *widget : findChildren<QWidget *>()) {
        if (widget->property("welcomeSettingRow").toBool()) set_setting_row_current(widget, widget->property("welcomeRowCurrent").toBool());
        if (widget->objectName() == "ListSeparator") set_own_style(widget, QString("background: %1; border: none;").arg(theme.separator));
        if (widget->objectName() == "ListRow") set_list_row_current(widget->findChild<QLabel *>(), widget->property("listRowCurrent").toBool());
        // Children that paint themselves refresh on the theme too.
        if (widget->metaObject()->indexOfMethod("apply_welcome_child_theme()") >= 0) QMetaObject::invokeMethod(widget, "apply_welcome_child_theme");
    }
    update();
}

void BasePage::set_setting_row_current(QWidget *card, bool current) {
    const auto theme = welcome_theme();
    card->setProperty("welcomeRowCurrent", current);
    card->setStyleSheet(QString("#SettingRow { background: %1; border: 1px solid %2; border-radius: %3px; }")
                            .arg(current ? theme.stage : theme.panel_subtle, theme.border)
                            .arg(dialog_scaled(10)));
}

QLabel *BasePage::add_list_row(QVBoxLayout *layout, const QString &name, QWidget *control, bool first) {
    if (!first) {
        auto *line = new QFrame;
        line->setObjectName("ListSeparator");
        line->setFixedHeight(1);
        set_own_style(line, QString("background: %1; border: none;").arg(welcome_theme().separator));
        layout->addWidget(line);
    }
    auto *row = new QWidget;
    row->setObjectName("ListRow");
    // A plain QWidget paints a style sheet background only when asked to.
    row->setAttribute(Qt::WA_StyledBackground);
    auto *row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(dialog_scaled(12), dialog_scaled(6), dialog_scaled(8), dialog_scaled(6));
    row_layout->setSpacing(dialog_scaled(16));
    auto *label = new QLabel(name, row);
    set_welcome_label_style(label, "primary", 13, 500);
    row_layout->addWidget(label, 1);
    row_layout->addWidget(control);
    layout->addWidget(row);
    set_list_row_current(label, false);
    return label;
}

void BasePage::set_list_row_current(QLabel *name, bool current) {
    if (!name) return;
    auto *row = name->parentWidget();
    row->setProperty("listRowCurrent", current);
    set_own_style(row, QString("background: %1; border: none; border-radius: %2px;").arg(current ? welcome_theme().stage : QString("transparent")).arg(dialog_scaled(8)));
    name->setProperty("welcomeFontWeight", current ? 600 : 500);
    apply_welcome_label_style(name);
}

std::tuple<QWidget *, QLabel *, QLabel *> BasePage::make_setting_row_with_refs(const QString &label_text, QWidget *widget, const QString &description) {
    auto *container = new QWidget;
    container->setObjectName("SettingRow");
    container->setProperty("welcomeSettingRow", true);
    auto *vbox = new QVBoxLayout(container);
    vbox->setContentsMargins(dialog_scaled(14), dialog_scaled(11), dialog_scaled(12), dialog_scaled(11));
    vbox->setSpacing(dialog_scaled(4));
    auto *row = new QHBoxLayout;
    row->setSpacing(dialog_scaled(12));
    auto *label = new QLabel(label_text, container);
    set_welcome_label_style(label, "primary", 14, 600);
    row->addWidget(label);
    row->addStretch();
    row->addWidget(widget);
    vbox->addLayout(row);
    QLabel *desc_label = nullptr;
    if (!description.isEmpty()) {
        desc_label = new QLabel(description, container);
        desc_label->setWordWrap(true);
        set_welcome_label_style(desc_label, "muted", 12, 400);
        vbox->addWidget(desc_label);
    }
    return {container, label, desc_label};
}
}
