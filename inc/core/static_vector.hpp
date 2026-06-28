/*
 * orb::core::static_vector<T, N> — a fixed-capacity, contiguous, NO-HEAP sequence
 * container with a vector-like API. Capacity N is fixed at compile time; the element
 * count varies at runtime up to N. Storage is inline (aligned bytes in the object), so
 * it lands in BSS/stack with zero allocation — the embedded stand-in for std::vector.
 *
 * Use it wherever the old code had a C array + a separate count, or a fixed `[N]` array
 * iterated by index. Supports range-for, front/back, emplace, and bounds-checked-by-
 * contract access (operator[] is unchecked like std::vector; use size()/full() first).
 *
 *   core::static_vector<XboxController, kMaxControllers> controllers;
 *   if (auto* c = controllers.emplace_back(addr, itf)) { ... }   // nullptr if full
 *   for (auto& c : controllers) c.poll();
 */
#ifndef ORB_CORE_STATIC_VECTOR_HPP
#define ORB_CORE_STATIC_VECTOR_HPP

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace orb::core {

template <typename T, std::size_t N>
class static_vector {
    static_assert(N > 0, "static_vector capacity must be > 0");

   public:
    using value_type = T;
    using size_type = std::size_t;
    using reference = T&;
    using const_reference = const T&;
    using iterator = T*;
    using const_iterator = const T*;

    constexpr static_vector() noexcept = default;
    ~static_vector() { clear(); }

    // Non-copyable/movable by default: these containers own live objects in raw storage
    // and are used as long-lived members; add explicit copy/move if a use needs it.
    static_vector(const static_vector&) = delete;
    static_vector& operator=(const static_vector&) = delete;

    // --- capacity ---
    constexpr size_type size() const noexcept { return size_; }
    static constexpr size_type capacity() noexcept { return N; }
    constexpr bool empty() const noexcept { return size_ == 0; }
    constexpr bool full() const noexcept { return size_ == N; }

    // --- element access (unchecked, like std::vector; check size()/full() first) ---
    reference operator[](size_type i) noexcept { return data()[i]; }
    const_reference operator[](size_type i) const noexcept { return data()[i]; }
    reference front() noexcept { return data()[0]; }
    const_reference front() const noexcept { return data()[0]; }
    reference back() noexcept { return data()[size_ - 1]; }
    const_reference back() const noexcept { return data()[size_ - 1]; }
    T* data() noexcept { return std::launder(reinterpret_cast<T*>(storage_)); }
    const T* data() const noexcept { return std::launder(reinterpret_cast<const T*>(storage_)); }

    // --- iterators (enable range-for + std algorithms) ---
    iterator begin() noexcept { return data(); }
    iterator end() noexcept { return data() + size_; }
    const_iterator begin() const noexcept { return data(); }
    const_iterator end() const noexcept { return data() + size_; }
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend() const noexcept { return end(); }

    // --- modifiers --- (return false / nullptr on a full container; never allocate)
    bool push_back(const T& v) {
        if (full()) return false;
        ::new (slot(size_)) T(v);
        ++size_;
        return true;
    }
    bool push_back(T&& v) {
        if (full()) return false;
        ::new (slot(size_)) T(std::move(v));
        ++size_;
        return true;
    }
    template <typename... A>
    T* emplace_back(A&&... a) {
        if (full()) return nullptr;
        T* p = ::new (slot(size_)) T(std::forward<A>(a)...);
        ++size_;
        return p;
    }
    void pop_back() noexcept {
        if (size_ == 0) return;
        --size_;
        data()[size_].~T();
    }
    void clear() noexcept {
        for (size_type i = 0; i < size_; ++i) data()[i].~T();
        size_ = 0;
    }

   private:
    void* slot(size_type i) noexcept { return &storage_[i * sizeof(T)]; }

    alignas(T) unsigned char storage_[N * sizeof(T)];
    size_type size_ = 0;
};

}  // namespace orb::core

#endif  // ORB_CORE_STATIC_VECTOR_HPP
