#pragma once
#include "canvas/items/drawing_items.h"
#include <QUndoStack>
#include <QUndoCommand>
#include <QPointer>
#include <QVariantMap>
#include <QGraphicsScene>
#include <functional>
#include <chrono>

namespace jietuba {
// Pending canvas/view dependencies. Refresh is invoked after the counter has
// changed, with the same value/force flag as NumberTool.refresh_next_number.
class NumberRefreshContext {
public:
    virtual ~NumberRefreshContext() = default;
    virtual void refresh_number_toolbar(int) = 0;
    virtual void refresh_number_cursor_if_active(bool force) = 0;
};
class NumberSequence {
public:
    static int count_numbers(QGraphicsScene *);
    static int get_next_number(QGraphicsScene *);
    static int set_next_number(QGraphicsScene *, int);
    static int set_next_number_and_refresh(QGraphicsScene *, int, bool force = true);
    static int adjust_next_number(QGraphicsScene *, int);
    static int assign_number_order(QGraphicsScene *, NumberItem *);
    static int get_max_number(QGraphicsScene *, NumberItem *override_item = nullptr, int override_number = 0);
    static int get_next_after_number_edit(QGraphicsScene *, NumberItem *, int old_number, int new_number, std::optional<int> current = {});
};
// Undo stack with debug output.
class CommandUndoStack : public QUndoStack {
public:
    using QUndoStack::QUndoStack;
    void push_command(QUndoCommand *command) { push(command); }
    void undo();
    void redo();
    void print_stack_status() const;
};
class AddItemCommand : public QUndoCommand {
public:
    AddItemCommand(QGraphicsScene *, ItemReference, const QString &text = "Add Item");
    void undo() override;
    void redo() override;
    const ItemReference &item() const { return item_; }
protected:
    QPointer<QGraphicsScene> scene_;
    ItemReference item_;
};
class RemoveItemCommand : public AddItemCommand {
public:
    RemoveItemCommand(QGraphicsScene *scene, ItemReference item, const QString &text = "Remove Item") : AddItemCommand(scene, std::move(item), text) {}
    void undo() override { AddItemCommand::redo(); }
    void redo() override { AddItemCommand::undo(); }
};
// Restores the text cleared in the editor before re-adding the item; a plain
// RemoveItemCommand would bring back an empty item.
class ClearTextItemCommand : public RemoveItemCommand {
public:
    ClearTextItemCommand(QGraphicsScene *scene, ItemReference item, QString old_text, const QString &text = "Clear Text")
        : RemoveItemCommand(scene, std::move(item), text), old_text_(std::move(old_text)) {}
    void undo() override;
    void redo() override;
private:
    QString old_text_;
};
class AddNumberCommand : public AddItemCommand {
public:
    AddNumberCommand(QGraphicsScene *, ItemReference, std::optional<int> before = {}, std::optional<int> after = {}, const QString &text = "Add Number");
    void undo() override;
    void redo() override;
private:
    int next_before_, next_after_;
};
class RemoveNumberCommand : public RemoveItemCommand {
public:
    RemoveNumberCommand(QGraphicsScene *scene, ItemReference item, const QString &text = "Remove Number") : RemoveItemCommand(scene, std::move(item), text) {}
    void undo() override;
    void redo() override;
};
using RemoveNumberItemCommand = RemoveNumberCommand;
class RemoveNumberAndRenumberCommand : public RemoveItemCommand {
public:
    RemoveNumberAndRenumberCommand(QGraphicsScene *, ItemReference, const QString &text = "Remove Number And Renumber");
    void undo() override;
    void redo() override;
private:
    struct NumberValue { ItemReference item; int number; };
    static void apply_numbers(const std::vector<NumberValue> &);
    int next_before_, next_after_;
    std::vector<NumberValue> old_numbers_, new_numbers_;
};
class BatchRemoveCommand : public QUndoCommand {
public:
    BatchRemoveCommand(QGraphicsScene *, std::vector<ItemReference>, const QString &text = "Remove Items", std::optional<int> number_next_before = {});
    void undo() override;
    void redo() override;
private:
    QPointer<QGraphicsScene> scene_;
    std::vector<ItemReference> items_;
    std::optional<int> next_before_;
    bool has_numbers_, first_redo_ = true;
};
class NumberStyleCommand : public QUndoCommand {
public:
    NumberStyleCommand(ItemReference, QString old_style, QString new_style, const QString &text = {});
    void undo() override;
    void redo() override;
private:
    ItemReference item_;
    QString old_, next_;
};
class EditItemCommand : public QUndoCommand {
public:
    EditItemCommand(ItemReference, QVariantMap old_state, QVariantMap new_state, const QString &text = "Edit Item");
    void undo() override { apply_state(old_state_); }
    void redo() override { apply_state(new_state_); }
protected:
    void apply_state(const QVariantMap &);
    ItemReference item_;
    QVariantMap old_state_, new_state_;
};
class NumberEditCommand : public EditItemCommand {
public:
    static constexpr double MERGE_WINDOW_SECONDS = .7;
    static constexpr int COMMAND_ID = 0x4E554D;
    using Clock = std::function<double()>;
    NumberEditCommand(ItemReference, QVariantMap old_state, QVariantMap new_state, std::optional<int> before = {}, std::optional<int> after = {}, const QString &text = "Edit Number", Clock clock = {});
    void undo() override;
    void redo() override;
    int id() const override { return COMMAND_ID; }
    bool mergeWith(const QUndoCommand *) override;
private:
    QPointer<QGraphicsScene> scene_;
    std::optional<int> next_before_, next_after_;
    Clock clock_;
    double last_merge_time_;
};
}
