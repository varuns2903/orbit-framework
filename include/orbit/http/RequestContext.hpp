#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <any>
#include <array>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace orbit::http {

/**
 * @brief Type-indexed, per-request storage: middleware that computes
 *        something for later middleware or the handler (the authenticated
 *        user, a tenant, a DB transaction, request timing) stashes it here
 *        instead of a header, a global, or a new fixed field on HttpRequest.
 *
 * At most one value per type `T`. `HttpRequest::context` holds one of
 * these, and `HttpRequest` has matching `set`/`get`/`ensure` templates that
 * forward to it, so a middleware writes `req.set(AuthInfo{...})` and a
 * handler reads `req.get<AuthInfo>()`.
 *
 * @code
 * struct AuthInfo { std::string user_id; };
 *
 * app.use([](HttpRequest& req, std::shared_ptr<ResponseWriter> w) {
 *     // ... verify a token ...
 *     req.set(AuthInfo{user_id});
 *     return true;
 * });
 *
 * app.get("/me", [](HttpRequest& req, std::shared_ptr<ResponseWriter> w) {
 *     if (auto* auth = req.get<AuthInfo>()) {
 *         w->send(HttpResponse().send(auth->user_id));
 *     }
 * });
 * @endcode
 *
 * The first few distinct types used (4, kept inline) cost no heap
 * allocation beyond whatever `std::any` itself needs to hold the value
 * (none for a value that fits its small-object buffer: a pointer, a
 * `shared_ptr`, a small struct). A fifth distinct type moves everything
 * into a heap-allocated overflow list.
 *
 * `T` must be copy-constructible (a `std::any` requirement), as most
 * context values are: an id, a small struct, a `shared_ptr` to something
 * with its own lifetime (a DB transaction, say). A move-only type does not
 * fit here; wrap it in a `shared_ptr` or `unique_ptr` instead.
 *
 * Not thread-safe: as with the rest of `HttpRequest`, only the thread
 * currently handling the request should read or write it.
 */
class RequestContext {
public:
    /// Sets (replacing any previous value) and returns a reference to it.
    template <typename T>
    T& set(T value) {
        Slot& slot = slot_for(std::type_index(typeid(T)));
        slot.value = std::move(value);
        return *std::any_cast<T>(&slot.value);
    }

    /// The value for `T`, or null if none is set.
    template <typename T>
    T* get() {
        Slot* slot = find(std::type_index(typeid(T)));
        return slot ? std::any_cast<T>(&slot->value) : nullptr;
    }
    template <typename T>
    const T* get() const {
        const Slot* slot = find(std::type_index(typeid(T)));
        return slot ? std::any_cast<T>(&slot->value) : nullptr;
    }

    /// True if a value for `T` is set.
    template <typename T>
    bool has() const {
        return get<T>() != nullptr;
    }

    /// The value for `T`, constructing and storing one from @p args first
    /// if none is set yet.
    template <typename T, typename... Args>
    T& ensure(Args&&... args) {
        if (T* existing = get<T>()) return *existing;
        return set<T>(T(std::forward<Args>(args)...));
    }

    /// Removes the value for `T`, if any. Returns whether one was removed.
    template <typename T>
    bool erase() {
        const auto key = std::type_index(typeid(T));
        for (size_t i = 0; i < inline_used_; ++i) {
            if (inline_[i].key == key) {
                inline_[i] = std::move(inline_[inline_used_ - 1]);
                --inline_used_;
                return true;
            }
        }
        for (auto it = overflow_.begin(); it != overflow_.end(); ++it) {
            if (it->key == key) {
                overflow_.erase(it);
                return true;
            }
        }
        return false;
    }

    // --- Named attributes, for interop or keys only known at runtime ---

    /// Sets a named string attribute (distinct storage from set<T>()).
    void set_attr(std::string name, std::string value) { attrs_[std::move(name)] = std::move(value); }

    /// A named attribute, or null if it was never set.
    const std::string* attr(std::string_view name) const {
        auto it = attrs_.find(std::string(name));
        return it == attrs_.end() ? nullptr : &it->second;
    }

private:
    struct Slot {
        std::type_index key{typeid(void)};
        std::any value;
    };

    static constexpr size_t kInlineCapacity = 4;

    Slot* find(std::type_index key) {
        for (size_t i = 0; i < inline_used_; ++i) {
            if (inline_[i].key == key) return &inline_[i];
        }
        for (auto& slot : overflow_) {
            if (slot.key == key) return &slot;
        }
        return nullptr;
    }
    const Slot* find(std::type_index key) const { return const_cast<RequestContext*>(this)->find(key); }

    Slot& slot_for(std::type_index key) {
        if (Slot* existing = find(key)) return *existing;
        if (inline_used_ < inline_.size()) {
            Slot& slot = inline_[inline_used_++];
            slot.key = key;
            return slot;
        }
        overflow_.push_back(Slot{key, {}});
        return overflow_.back();
    }

    std::array<Slot, kInlineCapacity> inline_{};
    size_t inline_used_ = 0;
    std::vector<Slot> overflow_;
    std::unordered_map<std::string, std::string> attrs_;
};

} // namespace orbit::http
