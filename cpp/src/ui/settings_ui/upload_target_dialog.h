#pragma once
#include "ui/fluent_lite/frameless.h"
#include "upload/s3_target.h"
#include <QMap>
#include <vector>

class QLabel;
class QScrollArea;

namespace jietuba {
class ComboBox;
class LineEdit;
class PushButton;
class PrimaryPushButton;

// Edits one upload target. Mistakes are shown beside their field when Save or Test is pressed, and the link a
// screenshot would get follows the fields while they are typed.
class UploadTargetDialog : public FrostedFramelessDialog {
    Q_OBJECT
public:
    // others are the targets it is saved beside (for the name check).
    UploadTargetDialog(const upload::Target &initial, const std::vector<upload::Target> &others, bool is_new, QWidget *parent = nullptr);
    // The fields as typed, trimmed; the id is the initial one.
    upload::Target target() const;
    // The link preview text as it stands.
    QString preview_text() const { return preview_full_; }
    // Shows what is wrong; false when nothing is.
    bool validate_fields();

    LineEdit *name_edit, *endpoint_edit, *region_edit, *bucket_edit, *access_key_edit, *secret_edit, *prefix_edit, *public_edit;
    ComboBox *preset_combo, *addressing_combo;
    PushButton *test_button;
    PrimaryPushButton *save_button;
    PushButton *cancel_button;
    QLabel *preview_label, *test_label;
    // The error line under a field, by the target's field name; hidden while empty.
    QMap<QString, QLabel *> errors;
    QScrollArea *scroll;
private:
    void apply_preset(int index);
    void refresh_preview();
    void run_test();
    void set_test_text(const QString &text, bool failed);
    upload::Target initial_;
    std::vector<upload::Target> others_;
    QString preview_full_;
    // The region the endpoint template was last filled with; the endpoint follows the region while it still equals it.
    QString templated_region_;
    bool filling_ = false;
};
}
