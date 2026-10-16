#pragma once
#include <QGraphicsItem>
#include <memory>

namespace jietuba {
struct ItemLease;
struct ItemLifetime {
    QGraphicsItem *item = nullptr;
    std::weak_ptr<ItemLease> lease;
};
// QGraphicsScene owns attached items. A command lease owns a detached item;
// destroying a scene or calling clear() invalidates every outstanding lease.
// Callers retaining detached items must retain an ItemReference, like a Python
// wrapper reference. Never pass a stack-allocated item into an owning command.
class TrackedItem {
public:
    explicit TrackedItem(QGraphicsItem *item) : lifetime_(std::make_shared<ItemLifetime>()) { lifetime_->item = item; }
    virtual ~TrackedItem() { lifetime_->item = nullptr; }
    TrackedItem(const TrackedItem &) = delete;
    TrackedItem &operator=(const TrackedItem &) = delete;
    std::shared_ptr<ItemLifetime> lifetime() const { return lifetime_; }
private:
    std::shared_ptr<ItemLifetime> lifetime_;
};
struct ItemLease {
    explicit ItemLease(std::shared_ptr<ItemLifetime> state) : state(std::move(state)) {}
    ~ItemLease() { if (auto *item = state->item; item && !item->scene()) delete item; }
    std::shared_ptr<ItemLifetime> state;
};
class ItemReference {
public:
    ItemReference() = default;
    explicit ItemReference(TrackedItem *item) {
        if (!item) return;
        auto state = item->lifetime();
        lease_ = state->lease.lock();
        if (!lease_) { lease_ = std::make_shared<ItemLease>(state); state->lease = lease_; }
    }
    QGraphicsItem *get() const { return lease_ ? lease_->state->item : nullptr; }
    template<class T> T *as() const { return dynamic_cast<T *>(get()); }
private:
    std::shared_ptr<ItemLease> lease_;
};
// Non-owning handle that reads as null once the item is destroyed, so holders
// never touch a deleted item. Items that are not TrackedItem are held raw.
class TrackedPointer {
public:
    TrackedPointer() = default;
    TrackedPointer(QGraphicsItem *item) { reset(item); }
    void reset(QGraphicsItem *item = nullptr) {
        raw_ = item;
        auto *tracked = dynamic_cast<TrackedItem *>(item);
        lifetime_ = tracked ? tracked->lifetime() : nullptr;
    }
    QGraphicsItem *get() const { return lifetime_ ? lifetime_->item : raw_; }
    template<class T> T *as() const { return dynamic_cast<T *>(get()); }
    explicit operator bool() const { return get() != nullptr; }
private:
    QGraphicsItem *raw_ = nullptr;
    std::shared_ptr<ItemLifetime> lifetime_;
};
}

