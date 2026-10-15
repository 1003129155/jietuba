#pragma once
#include "clipboard/core/models.h"

// The text item form of the manage window; widgets only, saving is the dialog's job.
namespace jietuba::clipboard {
class ManageDialog;

void build_text_content_form(ManageDialog *dialog);
void build_edit_text_content_form(ManageDialog *dialog, const ClipboardItem &item);
}
