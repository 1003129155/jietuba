#pragma once

// The CSV import/export form of the manage window; file handling is in the dialog and services.
namespace jietuba::clipboard {
class ManageDialog;

void build_import_export_form(ManageDialog *dialog);
}
