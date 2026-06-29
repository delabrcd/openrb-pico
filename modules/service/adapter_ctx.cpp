/*
 * The single cross-core adapter-state instance. The AdapterState class + the full
 * rationale live in inc/adapter_ctx.h; this TU just defines the singleton and its
 * accessor. Constant-initialised (constexpr std::atomic ctors) -> BSS, no global ctor.
 */
#include "adapter_ctx.h"

namespace orb::service {
namespace {
AdapterState g_instance;
}
AdapterState& adapter() { return g_instance; }
}  // namespace orb::service
