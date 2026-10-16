#include "clipboard/ui/forms/import_export_form.h"
#include "clipboard/ui/dialogs/manage_dialog.h"
#include "clipboard/ui/forms/form_widgets.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/cards.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/settings_ui/components.h"
#include <QHBoxLayout>

namespace jietuba::clipboard {
namespace {
std::pair<QFrame *, QVBoxLayout *> section_card(FluentIcon icon, const QString &title, const QString &description) {
    auto *card = new QFrame;
    card->setObjectName("ManageSectionCard");
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(dialog_scaled(18), dialog_scaled(16), dialog_scaled(18), dialog_scaled(16));
    layout->setSpacing(dialog_scaled(12));

    auto *header = new QHBoxLayout;
    header->setSpacing(dialog_scaled(12));
    header->addWidget(new IconBadge(icon, "neutral", card));
    auto *text_box = new QVBoxLayout;
    text_box->setSpacing(dialog_scaled(2));
    QLabel *title_label = field_label(title, false);
    apply_theme_text_style(title_label, 14, true);
    text_box->addWidget(title_label);
    text_box->addWidget(field_hint(description));
    header->addLayout(text_box, 1);
    layout->addLayout(header);
    return {card, layout};
}

PushButton *action_button(ManageDialog *dialog, const QString &text, void (ManageDialog::*handler)()) {
    auto *button = new PushButton(text);
    button->setCursor(Qt::PointingHandCursor);
    button->setMinimumWidth(dialog_scaled(140));
    QObject::connect(button, &QPushButton::clicked, dialog, handler);
    return button;
}
}

void build_import_export_form(ManageDialog *dialog) {
    auto [export_card, export_layout] = section_card(FluentIcon::Save, ManageDialog::tr_text("Export"), ManageDialog::tr_text("Export all saved content to CSV file"));
    auto *export_row = new QHBoxLayout;
    export_row->setSpacing(dialog_scaled(10));
    QLabel *encoding_label = field_hint(ManageDialog::tr_text("Encoding:"));
    encoding_label->setWordWrap(false);
    export_row->addWidget(encoding_label);
    auto *combo = new ComboBox;
    dialog->export_encoding_combo = combo;
    combo->addItem("UTF-8 (BOM)", "utf-8-sig");
    combo->addItem("UTF-8", "utf-8");
    combo->addItem("Shift_JIS", "shift_jis");
    combo->addItem("GBK", "gbk");
    export_row->addWidget(combo);
    export_row->addStretch();
    export_row->addWidget(action_button(dialog, ManageDialog::tr_text("Export to CSV"), &ManageDialog::export_to_csv));
    export_layout->addLayout(export_row);
    dialog->detail_layout->addWidget(export_card);

    auto [import_card, import_layout] =
        section_card(FluentIcon::Download, ManageDialog::tr_text("Import"), ManageDialog::tr_text("Import content from CSV file (Group, Content, Title)"));
    QLabel *format_hint = field_hint(ManageDialog::tr_text("Column 1: Group Name") + "\n" + ManageDialog::tr_text("Column 2: Content") + "\n" +
                                     ManageDialog::tr_text("Column 3: Title (optional)"));
    auto *import_row = new QHBoxLayout;
    import_row->addWidget(format_hint, 1);
    import_row->addWidget(action_button(dialog, ManageDialog::tr_text("Import from CSV"), &ManageDialog::import_from_csv), 0, Qt::AlignBottom);
    import_layout->addLayout(import_row);
    dialog->detail_layout->addWidget(import_card);

    dialog->detail_layout->addStretch();
}
}
