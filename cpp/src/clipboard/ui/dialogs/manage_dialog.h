#pragma once
#include "clipboard/core/models.h"
#include "core/i18n.h"
#include "ui/fluent_lite/frameless.h"
#include <QAction>
#include <QButtonGroup>
#include <QDateTime>
#include <QFrame>
#include <QHash>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSize>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <optional>

class QListWidgetItem;
class QMenu;

namespace jietuba {
class ComboBox;
class LineEdit;
class SegmentedWidget;
class TextEdit;
struct UIThemeTokens;
}

namespace jietuba::clipboard {
class ClipboardManager;
class FileDropZone;
class ImageDropZone;
class ImagePreview;
class OptionCard;
class ReorderListWidget;

// The clipboard manage window (one instance): groups on the left, the selected group's
// items in the middle, and the editor on the right. current_mode chooses what the right
// column edits: "group" (new or existing group), "content" (new or existing item) or
// "import_export" (CSV).
class ManageDialog : public FrostedFramelessDialog {
    Q_OBJECT
public:
    static QString tr_text(const char *text) { return i18n_tr(text, "ManageDialog"); }
    explicit ManageDialog(ClipboardManager *manager, QWidget *parent = nullptr);

    ClipboardManager *manager;
    QString current_mode = "content";
    std::optional<qint64> selected_group_id;
    std::optional<qint64> editing_group_id;
    std::optional<qint64> editing_item_id;

    // Sizes the window for the screen it is about to open on; a size the user dragged is kept and only
    // shrunk to fit. Position is Qt's: a never-moved dialog is centred within that screen on each show.
    void fit_to_screen();
    // fit_to_screen for a given available area.
    void fit_to_available(const QSize &available);
    void show_and_activate();
    // The clipboard window's entries: 0 new group, 1 new item in the current group.
    void switch_page(int index);
    void switch_mode(const QString &mode);
    // Brings the window in line after data changed elsewhere, dropping deleted groups and items.
    void refresh_after_external_change(std::optional<qint64> deleted_group_id = std::nullopt);
    void open_group_editor(qint64 group_id);
    void open_item_editor(qint64 item_id, std::optional<qint64> group_id);

    // ── Layout and form widgets the form builders fill in ──
    QWidget *sidebar = nullptr;
    QWidget *panel = nullptr;
    QWidget *list_column = nullptr;
    QFrame *column_separator = nullptr;
    QWidget *detail_column = nullptr;
    QLabel *groups_caption = nullptr;
    QPushButton *new_group_btn = nullptr;
    ReorderListWidget *group_list = nullptr;
    QPushButton *import_export_btn = nullptr;
    QLabel *list_title = nullptr;
    QLabel *list_count = nullptr;
    QPushButton *edit_group_btn = nullptr;
    LineEdit *search_input = nullptr;
    QAction *search_action = nullptr;
    QPushButton *add_item_btn = nullptr;
    QStackedWidget *list_stack = nullptr;
    ReorderListWidget *item_list = nullptr;
    QLabel *empty_label = nullptr;
    QLabel *reorder_hint = nullptr;
    QLabel *detail_title = nullptr;
    QLabel *detail_subtitle = nullptr;
    QWidget *detail_content = nullptr;
    QVBoxLayout *detail_layout = nullptr;
    QLabel *detail_meta = nullptr;
    // Shown for a saved item that can be pinned, as the item menu's entry is.
    QPushButton *pin_btn = nullptr;
    QPushButton *delete_btn = nullptr;
    QPushButton *save_btn = nullptr;

    QPointer<LineEdit> group_name_input;
    QPointer<LineEdit> icon_input;
    QPointer<QLabel> icon_preview;
    QPointer<OptionCard> radio_normal;
    QPointer<OptionCard> radio_file;
    QPointer<OptionCard> radio_hidden;
    QPointer<OptionCard> radio_filter;
    QPointer<QWidget> filter_kind_box;
    QPointer<ComboBox> filter_kind_combo;
    QPointer<ComboBox> filter_app_combo;
    QPointer<QButtonGroup> group_type_btn_group;
    QPointer<QScrollArea> emoji_scroll;
    QPointer<ImagePreview> image_preview;
    QPointer<SegmentedWidget> content_kind_switch;
    QPointer<ImageDropZone> image_drop_zone;
    QPointer<QPushButton> paste_image_btn;
    QPointer<LineEdit> title_input;
    QPointer<TextEdit> content_edit;
    QPointer<LineEdit> file_path_input;
    QPointer<FileDropZone> file_drop_zone;
    QPointer<ComboBox> export_encoding_combo;
    std::optional<QString> selected_file_path;
    std::optional<QSize> editing_image_size;
    QList<QPushButton *> emoji_tab_buttons;
    QStringList emoji_group_order;
    QHash<QString, QStringList> emoji_groups;
    int emoji_current_idx = 0;
    int detail_form_token = 0;

    // ── Callbacks of the form builders ──
    // The group type chosen in the form.
    int selected_group_type() const;
    // The rule a filter group gets from the form; none for the other group types.
    std::optional<FilterRule> selected_filter_rule() const;
    void on_group_type_toggled(bool checked, std::optional<int> form_token = std::nullopt);
    void on_filter_kind_changed(const QString &kind, std::optional<int> form_token = std::nullopt);
    void create_emoji_picker(const QString &current_icon = QString::fromUtf8("📁"));
    void switch_emoji_group_checked(int group_idx, std::optional<int> form_token = std::nullopt);
    void on_icon_input_changed_checked(const QString &text, std::optional<int> form_token = std::nullopt);
    void on_preset_icon_clicked_checked(const QString &icon, std::optional<int> form_token = std::nullopt);
    void on_browse_file();
    void on_browse_folder();
    void set_selected_file_path(const QString &path);
    void set_pending_image(const QImage &image);
    void use_clipboard_image();
    void browse_image_file();
    void copy_image_item();
    void pin_image_item();
    void save_image_item_as();
    void export_to_csv();
    void import_from_csv();

signals:
    void group_added();
    void content_added(qint64 group_id);
    void data_changed();

protected:
    void closeEvent(QCloseEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    struct ImageRequest {
        std::optional<qint64> group_id;
        std::optional<QString> title;
        QString content;
        qint64 since = 0;
        qint64 deadline_ms = 0;
    };

    void setup_titlebar();
    void release_memory();
    void reload_if_released();
    void setup_ui();
    QWidget *create_sidebar();
    QWidget *create_list_column();
    QWidget *create_detail_column();
    void refresh_theme_scope();
    void apply_ui_theme(const UIThemeTokens &tokens);
    void apply_dynamic_form_theme();
    QString group_tag(const Group &group) const;
    void refresh_group_list();
    // The new group form: no group row highlighted, the "New Group" button shown as checked, and a placeholder
    // for the unsaved group in the middle column.
    void enter_creating_group();
    // Restores the current group's highlight; with refresh false the caller refreshes the middle column itself.
    void leave_creating_group(bool refresh = true);
    void show_new_group_placeholder();
    void on_group_row_changed(QListWidgetItem *current);
    void on_group_selected(std::optional<qint64> group_id);
    std::optional<Group> get_selected_group();
    void on_group_moved(qint64 group_id, std::optional<qint64> before_id, std::optional<qint64> after_id);
    void show_group_menu(const QPoint &pos);
    QListWidgetItem *item_row(const ClipboardItem &item) const;
    void refresh_content_list();
    void show_list_empty(const QString &text);
    void apply_search_filter(const QString &text);
    void open_item_if_needed(qint64 item_id);
    void on_item_moved(qint64 item_id, std::optional<qint64> before_id, std::optional<qint64> after_id);
    void show_item_menu(const QPoint &pos);
    void on_content_drag(bool dragging);
    bool can_move_item_to_group(qint64 item_id, std::optional<qint64> group_id) const;
    void move_item_to_group(qint64 item_id, qint64 group_id);
    void on_add_item_clicked();
    void select_group(std::optional<qint64> group_id);
    void clear_detail_layout();
    void finish_detail_form_setup();
    static void disable_auto_default(QWidget *root);
    bool is_current_detail_form_token(std::optional<int> form_token) const;
    void set_detail_header(const QString &title, const QString &subtitle = {});
    void set_footer(const std::optional<QString> &save_text, bool deletable);
    void show_new_group_form();
    void show_edit_group_form(qint64 group_id);
    void update_group_form_header();
    bool is_file_content_mode();
    void show_new_content_form();
    void add_content_kind_switch();
    void set_new_content_kind(const QString &kind);
    void build_new_image_form();
    void show_edit_content_form(qint64 item_id);
    void show_import_export_form();
    QString get_selected_icon() const;
    void on_save_clicked();
    void save_group();
    void save_content();
    void save_image_title(const ClipboardItem &item);
    void save_new_image();
    void poll_captured_image();
    std::optional<ClipboardItem> find_captured_image(const ImageRequest &request);
    std::optional<Group> group_of_item(qint64 item_id);
    void file_captured_image(const ClipboardItem &item, const ImageRequest &request);
    std::optional<ClipboardItem> editing_image_item();
    void content_save_failed(const QString &error);
    void after_content_saved(const QString &action, std::optional<qint64> entity_id);
    void save_file_content();
    void save_text_content();
    void delete_group(qint64 group_id);
    void on_delete_clicked();

    // Whether "new item" in a general group is text, image or file; quick launch groups always take files.
    QString new_content_kind_ = "text";
    std::optional<QImage> pending_image_;
    std::optional<ImageRequest> image_request_;
    // Whether a dragged item may drop on a group, without querying the database during the drag.
    QHash<qint64, int> group_types_;
    // Content type and text sub-kind of each listed item.
    QHash<qint64, QPair<QString, QString>> item_types_;
    // The list and form were released on close and reload on the next show.
    bool needs_reload_ = false;
    QTimer image_wait_timer_;
    // The size last set for a screen; a different current size means the user dragged it.
    std::optional<QSize> fitted_size_;
    // The right column shows the new group form.
    bool creating_group_ = false;
};

ManageDialog *get_manage_dialog(ClipboardManager *manager = nullptr);
// The existing window, never created here.
ManageDialog *get_existing_manage_dialog();
// Destroys the cached window; whether it was visible.
bool destroy_manage_dialog();
}
