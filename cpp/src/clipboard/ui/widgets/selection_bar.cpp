#include "clipboard/ui/widgets/selection_bar.h"
#include "core/i18n.h"
#include "core/ui_scale.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>

namespace jietuba::clipboard {
namespace {
QString tr_text(const char *text) { return i18n_tr(text, "ClipboardWindow"); }
QString px(double value) { return QString("%1px").arg(scaled(value)); }
}

SelectionBar::SelectionBar(QWidget *parent) : QWidget(parent) {
    setObjectName("selection_bar");
    setAttribute(Qt::WA_StyledBackground, true);
    auto *layout = new QHBoxLayout(this);
    count_label = new QLabel;
    layout->addWidget(count_label, 1);

    paste_button = new QPushButton;
    paste_button->setObjectName("selection_paste");
    paste_button->setCursor(Qt::PointingHandCursor);
    paste_button->setFocusPolicy(Qt::NoFocus);
    connect(paste_button, &QPushButton::clicked, this, &SelectionBar::paste_requested);
    layout->addWidget(paste_button);

    save_button = new QPushButton;
    save_button->setObjectName("selection_save");
    save_button->setCursor(Qt::PointingHandCursor);
    save_button->setFocusPolicy(Qt::NoFocus);
    connect(save_button, &QPushButton::clicked, this, &SelectionBar::save_requested);
    layout->addWidget(save_button);

    more_button = new QToolButton;
    more_button->setText(QString::fromUtf8("···"));
    connect(more_button, &QToolButton::clicked, this, &SelectionBar::more_requested);
    exit_button = new QToolButton;
    exit_button->setText(QString::fromUtf8("✕"));
    connect(exit_button, &QToolButton::clicked, this, &SelectionBar::exit_requested);
    for (QToolButton *button : {more_button, exit_button}) {
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        layout->addWidget(button);
    }
    retranslate();
    hide();
}

void SelectionBar::set_count(int count) {
    count_ = count;
    count_label->setText(tr_text("{count} selected").replace("{count}", QString::number(count)));
    setVisible(count > 0);
}

void SelectionBar::retranslate() {
    set_count(count_);
    count_label->setToolTip(tr_text("Enter pastes them together, Esc clears the selection"));
    paste_button->setText(tr_text("Paste"));
    // Shorter than the menus' "Save as" where that is long (Japanese, Korean): the bar has the window's narrowest width.
    save_button->setText(tr_text("Save"));
    more_button->setToolTip(tr_text("More actions"));
    exit_button->setToolTip(tr_text("Exit multi-select"));
}

void SelectionBar::apply_style(const Theme &theme) {
    const QString accent = theme.colors.accent_primary;
    setFixedHeight(scaled(36));
    layout()->setContentsMargins(scaled(12), 0, scaled(6), 0);
    static_cast<QHBoxLayout *>(layout())->setSpacing(scaled(4));
    for (QPushButton *button : {paste_button, save_button}) button->setFixedHeight(scaled(24));
    for (QToolButton *button : {more_button, exit_button}) button->setFixedSize(scaled(28), scaled(28));
    setStyleSheet(QString(R"(
        #selection_bar { background: %1; }
        QLabel { color: white; font-size: %2; font-weight: bold; background: transparent; }
        #selection_paste { background: white; color: %1; border: none; border-radius: %3; padding: 0 %4; font-size: %5; font-weight: bold; }
        #selection_paste:hover { background: rgba(255, 255, 255, 0.86); }
        #selection_save { background: transparent; color: white; border: 1px solid rgba(255, 255, 255, 0.7); border-radius: %3; padding: 0 %4; font-size: %5; font-weight: bold; }
        #selection_save:hover { background: rgba(255, 255, 255, 0.18); }
        QToolButton { background: transparent; color: white; border: none; border-radius: %6; font-size: %7; font-weight: bold; }
        QToolButton:hover { background: rgba(255, 255, 255, 0.18); }
    )")
                      .arg(accent, px(13), px(12), px(14), px(12), px(4), px(15)));
}
}
