#pragma once
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <functional>
#include <vector>

class QToolButton;
class QLabel;
class QListWidget;
class QStackedWidget;

// Settings search. Entries are read from the built pages, so a setting is found by the text it
// shows and a new row needs no extra declaration.
namespace jietuba {
class LineEdit;
class SegmentedWidget;
class SettingsSearchPopup;

// A navigation page, a group of settings or one setting row.
struct SettingsSearchEntry {
    enum class Kind { Page, Group, Item };
    Kind kind = Kind::Item;
    int stack_index = -1;
    QString route_key;
    QString title;
    QString description;
    // Where it is: "page / group", plus the tab for rows on a tab.
    QString path;
    // A combo box's choices, so a setting is also found by a value it offers.
    QStringList options;
    // The feature hue of its page.
    QString tone;
    // The widget to scroll to and outline; empty for a page.
    QPointer<QWidget> target;
};

// Entries containing every word of the query, best first: titles outrank choices, paths and
// descriptions, and a whole or leading match outranks one inside the text. Ties put pages before
// groups before rows, so a page's name finds the page, then keep the given order. An empty
// query finds nothing.
std::vector<SettingsSearchEntry> rank_settings_search(const std::vector<SettingsSearchEntry> &entries, const QString &query);

// Appends the groups and rows of a built page: setting cards, card groups and section cards, and
// rows marked with mark_search_row. Rows hidden on purpose (another provider's fields) are left
// out; rows on another tab of the page are kept and reached through reveal_settings_row.
void collect_settings_entries(QWidget *page, const SettingsSearchEntry &page_entry, std::vector<SettingsSearchEntry> &out);

// Makes a custom-layout row searchable by its title label.
void mark_search_row(QWidget *row, QLabel *title, QLabel *description = nullptr);
// Shows the stack page of the current tab; search switches tabs through this link.
void link_tabs(SegmentedWidget *tabs, QStackedWidget *stack, const QStringList &routes);

// A result row's text in a list row_width wide: the title keeps its whole width, the location
// tag takes what is left, dropping its leading parts first, and is left out below a few characters.
struct SearchRowWidths {
    int title_room = 0;
    int title_needed = 0;
    int tag = 0;
    QString tag_text;
};
SearchRowWidths search_row_widths(int row_width, const QString &title, const QString &tag);

// Switches the tabs between page and target to the ones holding target.
void reveal_settings_row(QWidget *target, QWidget *page);

// Outlines target for a moment; the outline is a sibling over target and goes away by itself.
QWidget *flash_settings_row(QWidget *target);

// The search field with its result list. The list is laid over the window as a child, so the
// field keeps the keyboard while it is open: arrows move the selection, Enter opens it and
// Esc closes the list, then clears the field.
class SettingsSearchBox : public QWidget {
    Q_OBJECT
public:
    explicit SettingsSearchBox(QWidget *parent = nullptr);
    ~SettingsSearchBox() override;
    // What can be found now.
    std::function<std::vector<SettingsSearchEntry>()> entries;
    // Builds one more page and returns false once all are built. Pages are built on first
    // visit, so searching builds the rest one per event loop turn and adds their rows.
    std::function<bool()> build_next_page;
    std::function<void(const SettingsSearchEntry &)> activated;

    LineEdit *edit() const { return edit_; }
    // Null until results were first shown.
    QWidget *popup() const;
    QListWidget *result_list() const;
    QLabel *empty_label() const;
    bool results_visible() const;
    // The entries listed, in list order.
    const std::vector<SettingsSearchEntry> &results() const { return results_; }
    // Runs the query again, keeping the selected entry when it is still listed.
    void refresh();
    void hide_results();
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    void ensure_popup();
    void show_results();
    void place_popup();
    void move_selection(int delta);
    void activate(int row);
    void start_building();
    void build_step();
    void apply_theme();

    void place_decorations();

    LineEdit *edit_ = nullptr;
    // Drawn here rather than as line edit actions, whose icons keep 16 px at every window scale.
    QLabel *search_icon_ = nullptr;
    QToolButton *clear_button_ = nullptr;
    SettingsSearchPopup *popup_ = nullptr;
    std::vector<SettingsSearchEntry> results_;
    QString listed_query_;
    bool building_ = false;
    bool all_built_ = false;
    // The list is wanted but empty until the pages being built add rows.
    bool waiting_ = false;
    bool filtering_ = false;
};
}
