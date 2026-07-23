/*
 * Result<T, E> — a tiny, no-heap, no-exceptions "errors as values" type for the
 * firmware's C++ layer (see the Architecture wiki page).
 *
 * Why this exists instead of C++ exceptions: GCC's bare-metal ARM unwinder is not
 * thread-safe, and under FreeRTOS SMP two cores can throw at the same instant
 * (Launchpad #1905459). Throwing also allocates (__cxa_allocate_exception -> malloc)
 * and is not time-bounded, which is disqualifying anywhere near the core1 PIO-USB
 * path. So the build is -fno-exceptions and richer-than-bool failures propagate as a
 * value of this type. (`bool` / `Status` are still fine where there's no payload.)
 *
 * Design constraints, all deliberate:
 *   - Header-only, trivially destructible, zero dynamic allocation. T must be trivially
 *     copyable (static_assert'd) — every payload we move through the firmware is a small
 *     POD (handles, indices, packets), so we store it inline in a union with no lifetime
 *     management and no UB. This intentionally rules out Result<std::string> etc.
 *   - core1-safe: nothing here takes a lock, allocates, or calls the SDK. It is just a
 *     tagged union you can construct and inspect in a hot path.
 *   - std::expected-shaped semantics: value() on an error Result is the CALLER'S contract
 *     to avoid (like *std::expected), mirrored by has_value()/operator bool and value_or()
 *     for the safe path. We do not pull configASSERT into this widely-included header.
 *
 * Usage:
 *   Result<int>        f();          // ok(42)  or  Err::Timeout
 *   Result<void>       g();          // ok()    or  Err::NoResource
 *   if (auto r = f()) use(r.value());
 *   int n = f().value_or(-1);
 *   TRY(g());                        // propagate g()'s error out of the caller
 *   int n = TRY_VAL(f());            // bind f()'s value, or propagate its error
 */
#pragma once

#include <cstdint>
#include <type_traits>
#include <utility>

namespace orb::core {

// Default error domain. Small and extend-as-needed; the named cases cover the failure
// modes the C layer already reports (endpoint-claim/out-of-slots -> NoResource, xfer
// timeout -> Timeout, bad descriptor/arg -> InvalidArg, ring/fifo full -> Overflow).
// 0 is reserved as "no error" so an all-zero-initialised Status reads as Ok.
enum class Err : uint8_t {
    Ok = 0,
    Unknown,
    Timeout,
    NotFound,
    NoResource,  // out of endpoints / interface slots / queue space
    Busy,
    InvalidArg,
    IoError,
    NotReady,
    Overflow,
};

template <typename T, typename E = Err>
class [[nodiscard]] Result {
    static_assert(std::is_trivially_copyable<T>::value,
                  "Result<T>: T must be trivially copyable (no-heap embedded policy)");
    static_assert(std::is_trivially_copyable<E>::value, "Result<T,E>: E must be trivially copyable");

   public:
    // Implicit construction from a value or an error so callers can `return value;` /
    // `return Err::Timeout;` and TRY can `return r.error();` without ceremony.
    constexpr Result(const T& value) : value_(value), ok_(true) {}
    constexpr Result(T&& value) : value_(std::move(value)), ok_(true) {}
    constexpr Result(E error) : err_(error), ok_(false) {}

    // Named factories for call-site readability. NB: these do NOT make Result<E,E>
    // compile -- they forward to the same ambiguous ctor pair. If you ever need T==E,
    // use a distinct error enum for E rather than reaching for these.
    static constexpr Result ok(const T& value) { return Result(value); }
    static constexpr Result fail(E error) { return Result(error); }

    constexpr bool has_value() const { return ok_; }
    constexpr explicit operator bool() const { return ok_; }

    // value()/operator* are UB on an error Result (caller's contract, like *std::expected).
    // Use value_or() / a `bool` check first when the state is unknown.
    constexpr const T& value() const { return value_; }
    constexpr T& value() { return value_; }
    constexpr const T& operator*() const { return value_; }
    constexpr T& operator*() { return value_; }

    constexpr T value_or(T fallback) const { return ok_ ? value_ : fallback; }

    // error() is only meaningful when !has_value(); returns Err::Ok otherwise.
    constexpr E error() const { return ok_ ? E{} : err_; }

   private:
    union {
        T value_;
        E err_;
    };
    bool ok_;
};

// Payload-free specialisation: ok() or an error, no stored value. Use for the many
// operations that today return bool — Result<void> lets them carry an Err reason while
// staying TRY-able.
template <typename E>
class [[nodiscard]] Result<void, E> {
    static_assert(std::is_trivially_copyable<E>::value, "Result<void,E>: E must be trivially copyable");

   public:
    constexpr Result() : err_(E{}), ok_(true) {}        // ok
    constexpr Result(E error) : err_(error), ok_(false) {}

    static constexpr Result ok() { return Result(); }
    static constexpr Result fail(E error) { return Result(error); }

    constexpr bool has_value() const { return ok_; }
    constexpr explicit operator bool() const { return ok_; }
    constexpr E error() const { return ok_ ? E{} : err_; }

   private:
    E err_;
    bool ok_;
};

}  // namespace orb::core

/*
 * Propagation macros. These use a GCC statement-expression ({ ... }) — the toolchain is
 * GCC-only (arm-none-eabi-gcc) and the codebase already relies on GCC extensions, so
 * this is acceptable and keeps the call site clean.
 *
 *   TRY(expr)      — evaluate a Result-returning expression; on error, `return` that
 *                    error from the enclosing function (whose return type must be a
 *                    Result with a compatible E). On success, discards the value.
 *   TRY_VAL(expr)  — same, but the statement-expression *yields* the unwrapped value,
 *                    so you can write `int n = TRY_VAL(f());`. Bind the result BY VALUE:
 *                    the yielded `T&` refers into a temporary that dies at the macro's
 *                    closing `})`, so `auto& x = TRY_VAL(...)` would dangle.
 *
 * The enclosing function must return a Result<…, E>; the implicit Result(E) constructor
 * makes `return _r.error();` well-formed.
 */
#define TRY(expr)                            \
    do {                                     \
        auto _try_r = (expr);                \
        if (!_try_r) return _try_r.error();  \
    } while (0)

#define TRY_VAL(expr)                                \
    ({                                               \
        auto _try_r = (expr);                        \
        if (!_try_r) return _try_r.error();          \
        _try_r.value();                              \
    })

