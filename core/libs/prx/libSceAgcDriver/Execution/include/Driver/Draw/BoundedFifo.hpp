#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_BOUNDEDFIFO_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_BOUNDEDFIFO_HPP

#include <cstddef>
#include <iterator>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

template <typename T>
class BoundedFifo {
public:
    explicit BoundedFifo(std::size_t capacity) : slots(capacity == 0 ? 1 : capacity) {}

    template <bool Const>
    class Iterator {
    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = std::conditional_t<Const, const T*, T*>;
        using reference = std::conditional_t<Const, const T&, T&>;

        Iterator() = default;
        Iterator(std::conditional_t<Const, const BoundedFifo*, BoundedFifo*> fifo, std::size_t index) : fifo(fifo), index(index) {}
        reference operator*() const { return (*fifo)[index]; }
        pointer operator->() const { return &(*fifo)[index]; }
        Iterator& operator++() { ++index; return *this; }
        Iterator operator++(int) { auto copy = *this; ++index; return copy; }
        Iterator& operator--() { --index; return *this; }
        Iterator operator--(int) { auto copy = *this; --index; return copy; }
        bool operator==(const Iterator& other) const { return index == other.index; }
        bool operator!=(const Iterator& other) const { return index != other.index; }

    private:
        std::conditional_t<Const, const BoundedFifo*, BoundedFifo*> fifo = nullptr;
        std::size_t index = 0;
    };

    using iterator = Iterator<false>;
    using const_iterator = Iterator<true>;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    std::size_t size() const { return count; }
    std::size_t capacity() const { return slots.size(); }
    bool empty() const { return count == 0; }
    bool full() const { return count == slots.size(); }

    T& operator[](std::size_t index) { return slots[(head + index) % slots.size()]; }
    const T& operator[](std::size_t index) const { return slots[(head + index) % slots.size()]; }
    T& front() { return slots[head]; }
    const T& front() const { return slots[head]; }

    void push_back(T value) {
        if (full()) throw std::logic_error("bounded FIFO pushed past its capacity");
        slots[(head + count) % slots.size()] = std::move(value);
        ++count;
    }

    void pop_front() {
        slots[head] = T{};
        head = (head + 1) % slots.size();
        --count;
    }

    iterator begin() { return {this, 0}; }
    iterator end() { return {this, count}; }
    const_iterator begin() const { return {this, 0}; }
    const_iterator end() const { return {this, count}; }
    reverse_iterator rbegin() { return reverse_iterator(end()); }
    reverse_iterator rend() { return reverse_iterator(begin()); }
    const_reverse_iterator rbegin() const { return const_reverse_iterator(end()); }
    const_reverse_iterator rend() const { return const_reverse_iterator(begin()); }

private:
    std::vector<T> slots;
    std::size_t head = 0;
    std::size_t count = 0;
};

}

#endif
