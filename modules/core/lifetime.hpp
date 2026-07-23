#pragma once

// orb::mem::start_lifetime_as<T>(p) — obtain a T that lives over the bytes at p, reusing
// their existing value, with NO constructor run and NO undefined behaviour. This is the
// tool for viewing a wire byte buffer as a typed (implicit-lifetime / trivially-copyable)
// packet struct: it replaces the type-punning union that C code reaches for, without the
// aliasing UB a bare reinterpret_cast<T*> would incur under -fstrict-aliasing.
//
// It IS std::start_lifetime_as (C++23, P2590) when the standard library provides it. As of
// the pinned toolchain (GCC 15.2 / libstdc++), the LIBRARY function is not yet implemented
// (__cpp_lib_start_lifetime_as is undefined) even though the language is C++23, so we fall
// back to the canonical polyfill: std::memmove(p, p, sizeof(T)) implicitly creates a T in
// the destination bytes (P0593) and std::launder yields a pointer to that new object. GCC
// recognises the self-move of constant size and elides it, so at -O2 the accessor compiles
// to nothing (verified: a plain ldrb/strb on the buffer, identical to the old union). The
// const overload does NOT memmove: a read-only view must not store back through a const (or
// ROM) pointer, and the T was already implicitly created by whatever filled the buffer, so it
// just launders a const pointer to it. Drop the polyfill once libstdc++ ships the real thing.

#include <version>

#if defined(__cpp_lib_start_lifetime_as)
#include <memory>
namespace orb::mem {
using std::start_lifetime_as;
}
#else
#include <cstring>
#include <new>
namespace orb::mem {
template <typename T>
[[gnu::always_inline]] inline T *start_lifetime_as(void *p) noexcept {
    return std::launder(static_cast<T *>(std::memmove(p, p, sizeof(T))));
}
template <typename T>
[[gnu::always_inline]] inline const T *start_lifetime_as(const void *p) noexcept {
    // A const view only READS the bytes and must never write them (a const_cast + memmove
    // "self-move" would store back through the const pointer -- harmless for RAM, but UB for
    // genuinely const/ROM-resident storage). Whatever produced these bytes -- the mutable
    // start_lifetime_as, or the memcpy/DMA that filled the buffer -- already implicitly created
    // the implicit-lifetime T at p, so a const view just launders a pointer to it. Zero store,
    // zero-cost (launder is codegen-free).
    return std::launder(reinterpret_cast<const T *>(p));
}
}  // namespace orb::mem
#endif
