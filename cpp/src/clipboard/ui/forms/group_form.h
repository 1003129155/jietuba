#pragma once
#include "clipboard/core/models.h"

// The group form of the manage window: name, group type and the icon picker.
namespace jietuba::clipboard {
class ManageDialog;

void build_new_group_form(ManageDialog *dialog);
void build_edit_group_form(ManageDialog *dialog, const Group &group);
}
