#pragma once

#include <concepts>
#include <memory>
#include <stdexcept>
#include <typeindex>
#include <unordered_map>
#include <utility>

namespace ourweb {

// Type-erased application state storage
class StateRegistry {
public:
    // Store an already-constructed shared object (works for non-movable types
    // such as structs holding a std::mutex).
    template <typename T>
    void set(std::shared_ptr<T> ptr) {
        storage_[std::type_index(typeid(T))] = std::move(ptr);
    }

    // Move/copy a value into the registry.
    template <typename T>
        requires std::move_constructible<T>
    void set(T value) {
        set(std::make_shared<T>(std::move(value)));
    }

    // Construct the state object in place from arguments.
    template <typename T, typename... Args>
    void emplace(Args&&... args) {
        set(std::make_shared<T>(std::forward<Args>(args)...));
    }

    template <typename T>
    std::shared_ptr<T> get() const {
        auto key = std::type_index(typeid(T));
        auto it = storage_.find(key);
        if (it == storage_.end()) {
            return nullptr;
        }
        return std::static_pointer_cast<T>(it->second);
    }

    template <typename T>
    bool has() const {
        return storage_.count(std::type_index(typeid(T))) > 0;
    }

private:
    std::unordered_map<std::type_index, std::shared_ptr<void>> storage_;
};

template <typename T>
class State {
public:
    explicit State(std::shared_ptr<T> ptr) : ptr_(std::move(ptr)) {
        if (!ptr_) {
            throw std::runtime_error("State<T>: requested state type is not registered");
        }
    }

    const T& operator*() const noexcept { return *ptr_; }
    T& operator*() noexcept { return *ptr_; }
    const T* operator->() const noexcept { return ptr_.get(); }
    T* operator->() noexcept { return ptr_.get(); }

    const T& value() const noexcept { return *ptr_; }
    T& value() noexcept { return *ptr_; }

    std::shared_ptr<T> shared() const noexcept { return ptr_; }

private:
    std::shared_ptr<T> ptr_;
};

} // namespace ourweb
