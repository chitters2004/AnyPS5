#ifndef CORE_SHADER_RECOMPILER_SHAREDVECTOR_HPP
#define CORE_SHADER_RECOMPILER_SHAREDVECTOR_HPP

#include <atomic>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

namespace ShaderRecompiler {

template <class T>
class SharedVector {
public:
    using value_type = T;
    using size_type = typename std::vector<T>::size_type;
    using const_iterator = typename std::vector<T>::const_iterator;
    using iterator = const_iterator;
    using const_reference = const T&;
    using reference = const T&;

    SharedVector() = default;
    SharedVector(std::vector<T> values) { assign(std::move(values)); }
    SharedVector(std::initializer_list<T> values) { assign(std::vector<T>(values)); }

    SharedVector& operator=(std::vector<T> values) {
        assign(std::move(values));
        return *this;
    }
    SharedVector& operator=(std::initializer_list<T> values) {
        assign(std::vector<T>(values));
        return *this;
    }

    operator const std::vector<T>&() const { return get(); }
    [[nodiscard]] const std::vector<T>& get() const { return values != nullptr ? *values : empty_(); }

    [[nodiscard]] const_iterator begin() const { return get().begin(); }
    [[nodiscard]] const_iterator end() const { return get().end(); }
    [[nodiscard]] const_iterator cbegin() const { return get().cbegin(); }
    [[nodiscard]] const_iterator cend() const { return get().cend(); }
    [[nodiscard]] size_type size() const { return values != nullptr ? values->size() : 0u; }
    [[nodiscard]] bool empty() const { return values == nullptr || values->empty(); }
    [[nodiscard]] size_type capacity() const { return values != nullptr ? values->capacity() : 0u; }
    [[nodiscard]] const T* data() const { return get().data(); }
    [[nodiscard]] const T& operator[](size_type index) const { return (*values)[index]; }
    [[nodiscard]] const T& at(size_type index) const { return get().at(index); }
    [[nodiscard]] const T& front() const { return values->front(); }
    [[nodiscard]] const T& back() const { return values->back(); }

    std::vector<T>& mutate() {
        if (values == nullptr) {
            values = std::make_shared<std::vector<T>>();
        } else if (values.use_count() != 1) {
            values = std::make_shared<std::vector<T>>(*values);
        } else {
            std::atomic_thread_fence(std::memory_order_acquire);
        }
        return *values;
    }
    void assign(std::vector<T> replacement) {
        if (replacement.empty()) {
            values.reset();
            return;
        }
        values = std::make_shared<std::vector<T>>(std::move(replacement));
    }
    template <class Iterator>
    void assign(Iterator first, Iterator last) {
        assign(std::vector<T>(first, last));
    }
    void assign(size_type count, const T& value) { assign(std::vector<T>(count, value)); }
    void clear() { values.reset(); }
    void push_back(const T& value) { mutate().push_back(value); }
    void push_back(T&& value) { mutate().push_back(std::move(value)); }
    template <class... Args>
    T& emplace_back(Args&&... args) {
        return mutate().emplace_back(std::forward<Args>(args)...);
    }
    void reserve(size_type count) { mutate().reserve(count); }
    void resize(size_type count) { mutate().resize(count); }
    void resize(size_type count, const T& value) { mutate().resize(count, value); }

    void unshare() {
        if (values != nullptr && values.use_count() != 1) values = std::make_shared<std::vector<T>>(*values);
    }
    [[nodiscard]] bool shared() const { return values != nullptr && values.use_count() != 1; }

    friend bool operator==(const SharedVector& left, const SharedVector& right) { return left.values == right.values || left.get() == right.get(); }
    friend bool operator==(const SharedVector& left, const std::vector<T>& right) { return left.get() == right; }

private:
    static const std::vector<T>& empty_() {
        static const std::vector<T> none;
        return none;
    }
    std::shared_ptr<std::vector<T>> values;
};

}

#endif
