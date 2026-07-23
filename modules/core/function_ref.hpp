/*
 * orb::core::function_ref<R(Args...)> — a non-owning, type-erased view of a Callable.
 *
 * The embedded-correct stand-in for passing "a std::function" WITHOUT the heap: it stores
 * only a pointer to the callable plus a thunk, so it is two pointers, trivially copyable,
 * and allocates nothing. Like std::string_view / std::span, it is a VIEW — the referenced
 * callable must outlive the function_ref. Use it for callbacks that are passed in and
 * invoked, not stored past the call (for stored/owning callbacks use
 * core::inplace_function, which copies the callable into fixed inline storage).
 *
 * Models C++26 std::function_ref. No <functional> heap machinery is instantiated.
 *
 *   void drain(core::function_ref<void(std::span<const std::byte>)> sink);
 *   drain([&](auto bytes){ uart.write(bytes); });   // lambda lives across the call -> OK
 */
#pragma once

#include <functional>  // std::invoke (no allocation)
#include <memory>      // std::addressof
#include <type_traits>
#include <utility>

namespace orb::core {

template <typename Sig>
class function_ref;

template <typename R, typename... Args>
class function_ref<R(Args...)> {
   public:
    // Construct from any callable `f` invocable as R(Args...) that is not itself a
    // function_ref. `f` is bound by reference -- it must outlive this view.
    template <typename F,
              typename = std::enable_if_t<
                  !std::is_same_v<std::remove_cvref_t<F>, function_ref> &&
                  std::is_invocable_r_v<R, F&, Args...>>>
    constexpr function_ref(F&& f) noexcept
        : obj_(const_cast<void*>(static_cast<const void*>(std::addressof(f)))),
          thunk_(+[](void* obj, Args... args) -> R {
              return std::invoke(*static_cast<std::add_pointer_t<F>>(obj),
                                 std::forward<Args>(args)...);
          }) {}

    function_ref(const function_ref&) noexcept = default;
    function_ref& operator=(const function_ref&) noexcept = default;

    constexpr R operator()(Args... args) const {
        return thunk_(obj_, std::forward<Args>(args)...);
    }

   private:
    void* obj_;
    R (*thunk_)(void*, Args...);
};

}  // namespace orb::core

