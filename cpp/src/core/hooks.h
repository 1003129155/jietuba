#pragma once
#include <functional>
#include <utility>
#include <vector>

namespace jietuba {
// Callbacks attached to an extension point of an object, called in registration
// order. Each registration returns an id for removal, so modules detach
// independently of one another.
template <class Signature>
class HookList {
public:
    using Callback = std::function<Signature>;
    int add(Callback callback) {
        callbacks_.emplace_back(++last_id_, std::move(callback));
        return last_id_;
    }
    void remove(int id) {
        for (auto it = callbacks_.begin(); it != callbacks_.end(); ++it) {
            if (it->first == id) {
                callbacks_.erase(it);
                return;
            }
        }
    }
    void clear() { callbacks_.clear(); }
    bool empty() const { return callbacks_.empty(); }
    // A copy, so callbacks may register or remove hooks while running.
    std::vector<Callback> callbacks() const {
        std::vector<Callback> result;
        for (const auto &entry : callbacks_) result.push_back(entry.second);
        return result;
    }
private:
    std::vector<std::pair<int, Callback>> callbacks_;
    int last_id_ = 0;
};
}
