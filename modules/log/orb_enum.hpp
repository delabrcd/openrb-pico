#pragma once

// Thin, header-only, debug-gated wrapper over magic_enum so call sites can render an
// enumerator's name without hand-maintaining a parallel string table. The whole point
// of the gate is flash: magic_enum reflects enum names by parsing __PRETTY_FUNCTION__
// at compile time and materialising a static table of name strings. We only ever want
// those tables in a DEBUG build (where the logger is live); a RELEASE build
// (OPENRB_DEBUG_ENABLED == 0) must pull in NONE of them. So when debug is off this
// resolves to a trivial constexpr that returns an empty view and references no
// magic_enum machinery at all -- the header (and its tables) is never included.
//
// No heap, no globals: magic_enum's tables are constexpr static storage and the names
// are null-terminated (static_str appends '\0'), so callers needing a C string may use
// enum_name(e).data() safely for a resolved name.

#include <string_view>

#include "orb_debug.h"  // OPENRB_DEBUG_ENABLED (the all-or-nothing debug switch)

#if OPENRB_DEBUG_ENABLED
#include <magic_enum/magic_enum.hpp>
#endif

namespace orb {

template <class E>
constexpr std::string_view enum_name(E e) {
#if OPENRB_DEBUG_ENABLED
    return magic_enum::enum_name(e);
#else
    (void)e;          // RELEASE: no reflection, no tables, no flash.
    return {};        // cheap fallback; logging is compiled out anyway.
#endif
}

}  // namespace orb

