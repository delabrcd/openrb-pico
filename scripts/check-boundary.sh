#!/usr/bin/env bash
# check-boundary.sh — portability + static-DI boundary enforcement.
#
# Two families of checks:
#
# 1. Layering (P0-P3): greps modules/service/ and modules/protocol/ for forbidden SDK/RTOS/
#    TinyUSB includes and bare FreeRTOS type identifiers that violate the boundary defined in
#    docs/architecture/modern-cpp.md.
#
#    EXCLUDED: modules/driver/ (the vendor seam — it is allowed to include TinyUSB / SDK
#    directly). service/ is fully tusb-free as of the P3 static-DI step: the TinyUSB HID
#    (guitar) and MIDI (drums) extern "C" seam callbacks, and every tusb operation the service
#    methods used to call, now live in modules/driver/guitar_hid_driver.cpp and
#    modules/driver/drums_midi_seam.cpp.
#
# 2. Static-DI invariants (P7): lock in the shape the P0-P6 re-architecture converged on so a
#    regression fails CI instead of sneaking back in:
#      - composition root: modules/app/system.cpp defines ONLY system() / system_init() /
#        bind_usb_seams() — no forwarder free functions.
#      - forwarder resurrection: none of the free-function forwarders P1-P4 grew (and P5
#        deleted) are allowed to reappear anywhere in modules/ as a definition.
#      - void* confinement: `void*`/`void *` in modules/ (excluding modules/external) is
#        restricted to an explicit allowlist of the C-ABI/RTOS trampoline + type-erasure TUs
#        that legitimately need it. Everywhere else (service/, protocol/, most of app/) a new
#        void* is a boundary regression.
#
# Usage:
#   scripts/check-boundary.sh          # run from the repo root or any subdirectory
#   scripts/check-boundary.sh --quiet  # (accepted for compatibility; no output is suppressible
#                                       #  now that every check is fatal)
#
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

QUIET=0
for arg in "$@"; do
    [[ "${arg}" == "--quiet" ]] && QUIET=1
done

SEARCH_DIRS=(
    "${REPO}/modules/service"
    "${REPO}/modules/protocol"
)

# Includes that must NEVER appear in service/ or protocol/
# (all of these are SDK, RTOS, or chipset-specific headers the portability boundary forbids).
FORBIDDEN_PATTERNS=(
    '#include[[:space:]]*[<"]\(pico/\)'
    '#include[[:space:]]*[<"]\(hardware/\)'
    '#include[[:space:]]*[<"]\(FreeRTOS\.h\)'
    '#include[[:space:]]*[<"]\(task\.h\)'
    '#include[[:space:]]*[<"]\(timers\.h\)'
    '#include[[:space:]]*[<"]\(semphr\.h\)'
    '#include[[:space:]]*[<"]\(queue\.h\)'
    '#include[[:space:]]*[<"]\(tusb\|class/\|host/\|device/\)'
    '#include[[:space:]]*[<"]\(usb_midi_host\)'
    '#include[[:space:]]*[<"]\(bsp/\)'
)

found_violations=0

for dir in "${SEARCH_DIRS[@]}"; do
    for pattern in "${FORBIDDEN_PATTERNS[@]}"; do
        while IFS= read -r match; do
            [[ -z "${match}" ]] && continue
            echo "BOUNDARY VIOLATION: ${match}"
            found_violations=1
        done < <(grep -rn --include="*.cpp" --include="*.hpp" "${pattern}" "${dir}" 2>/dev/null || true)
    done
done

# Bare FreeRTOS/tusb TYPE identifiers can leak into service/protocol transitively via allowed
# osal includes (e.g. a TimerHandle_t showing up in a signature) even with no forbidden
# #include line present. Catch those directly, as whole words, restricted to the same
# SEARCH_DIRS (this does NOT scan modules/osal or modules/hal, which are allowed to use them).
FORBIDDEN_TYPES=(
    'TimerHandle_t'
    'TaskHandle_t'
    'QueueHandle_t'
    'SemaphoreHandle_t'
    'BaseType_t'
    'UBaseType_t'
    'TickType_t'
    'StaticTimer_t'
    'StaticTask_t'
    'StaticQueue_t'
    'TimerCallbackFunction_t'
    'TaskFunction_t'
)

for dir in "${SEARCH_DIRS[@]}"; do
    for type_name in "${FORBIDDEN_TYPES[@]}"; do
        while IFS= read -r match; do
            [[ -z "${match}" ]] && continue
            echo "BOUNDARY VIOLATION (freertos type): ${match}"
            found_violations=1
        done < <(grep -rnE --include="*.cpp" --include="*.hpp" "\b${type_name}\b" "${dir}" 2>/dev/null || true)
    done
done

# --- P7 guard: composition root stays a composition root -------------------------------------
# modules/app/system.cpp is the ONLY place g_system / system() / system_init() /
# bind_usb_seams() live. P1-P4 grew a free-function forwarder per leaf/service object here so
# every existing caller could keep calling a plain function while the callee moved into System;
# P5 deleted the last of those forwarders. Assert the file doesn't regrow one: extract every
# "name(...) {" definition-looking line and fail if any name other than the three sanctioned
# ones (or a lambda, which never matches -- lambdas have no bare identifier before the '(')
# shows up. Verified false-positive-free against the current 41-line file (matches exactly
# system/system_init/bind_usb_seams, nothing from the header-comment prose, since prose never
# happens to end in "(...) {").
SYSTEM_CPP="${REPO}/modules/app/system.cpp"
SANCTIONED_COMPOSITION_ROOT_FNS=("system" "system_init" "bind_usb_seams")

if [[ -f "${SYSTEM_CPP}" ]]; then
    while IFS=: read -r line name; do
        [[ -z "${name}" ]] && continue
        sanctioned=0
        for fn in "${SANCTIONED_COMPOSITION_ROOT_FNS[@]}"; do
            [[ "${name}" == "${fn}" ]] && sanctioned=1 && break
        done
        if [[ "${sanctioned}" -eq 0 ]]; then
            echo "BOUNDARY VIOLATION (composition root): ${SYSTEM_CPP}:${line}: unexpected function definition '${name}' -- system.cpp must contain only ${SANCTIONED_COMPOSITION_ROOT_FNS[*]}"
            found_violations=1
        fi
    done < <(grep -noP '\b[A-Za-z_][A-Za-z0-9_]*(?=\([^;{}]*\)[[:space:]]*\{)' "${SYSTEM_CPP}" 2>/dev/null || true)
fi

# --- P7 guard: deleted forwarders never come back ---------------------------------------------
# Same P5 cleanup as above, checked negatively across ALL of modules/ (not just system.cpp) so a
# forwarder resurrected somewhere else (not necessarily back in system.cpp) is still caught.
# Matches only DEFINITION-shaped lines (identifier directly followed by "(args) {", requiring
# the '{' -- so plain calls, declarations ending in ';', and comments/log strings mentioning the
# old name don't match). Verified zero hits (i.e. false-positive-free) against the current tree,
# where every remaining mention of these names is prose in a comment.
DELETED_FORWARDER_NAMES=(
    xbox_fifo_read xbox_fifo_write xbox_fifo_init xbox_fifo_peek xbox_fifo_advance
    xbox_fifo_count xbox_fifo_empty xbox_fifo_full xbox_fifo_clear
    app_queues_init host_tx_send host_tx_recv
    notify_xbox_of_all_instruments notify_xbox_of_single_instrument
    connect_instrument disconnect_instrument
    instrument_manager_init instrument_manager_service
    serial_midi_init drum_task
)

for name in "${DELETED_FORWARDER_NAMES[@]}"; do
    while IFS= read -r match; do
        [[ -z "${match}" ]] && continue
        echo "BOUNDARY VIOLATION (forwarder resurrected): ${match}"
        found_violations=1
    done < <(grep -rnE --include="*.cpp" --include="*.hpp" --include="*.h" \
                 "^[^/\"]*\b${name}\b\([^;{}]*\)[[:space:]]*\{" "${REPO}/modules" 2>/dev/null \
                 | grep -v '/external/' || true)
done

# --- P7 guard: void* confined to osal + the seam thunks ----------------------------------------
# void* is the FreeRTOS C ABI (TaskFunction_t/pvTimerID) and the general type-erasure /
# placement-storage / C-library-callback idiom; it is NOT something service/protocol/most of
# app/ ever legitimately needs (they take typed references or the wire-format pointers
# std::uint8_t*/std::byte*/const char* already allow through separately). Grep modules/
# (excluding modules/external) for void* / void * and fail on any hit outside this allowlist,
# determined empirically by running the grep first and inspecting every current hit -- each
# entry below is annotated with why that TU is a genuine seam and not feature code.
VOID_STAR_ALLOWLIST=(
    # orb::core::function_ref<R(Args...)> -- a non-owning type-erased Callable view (the
    # heap-free std::function_ref stand-in). void* obj_ + a thunk IS the type-erasure
    # mechanism; nothing RTOS-specific, but fundamental to how the type works.
    "modules/core/function_ref.hpp"
    # orb::mem::start_lifetime_as<T>(void*) -- C++23 (P2590) polyfill for viewing a wire byte
    # buffer as a typed struct without -fstrict-aliasing UB; the standard function itself is
    # specified to take void*.
    "modules/core/lifetime.hpp"
    # orb::core::static_vector<T,N>::slot(i) -- returns void* into the inline byte storage
    # immediately before placement-new; a generic fixed-capacity container primitive, not
    # feature code.
    "modules/core/static_vector.hpp"
    # xboxh_send_report(..., const void *report, ...) -- vendor USB-host seam: the raw wire
    # report pointer handed to the TinyUSB host controller driver API. modules/driver/ is the
    # sanctioned tusb/SDK seam.
    "modules/driver/xbox_controller_driver.cpp"
    "modules/driver/xbox_controller_driver.h"
    # orb_log_hexdump(..., const void* data, ...) -- generic byte-buffer log sink (dumps
    # arbitrary POD), mirrors the C stdlib's own untyped-buffer convention.
    "modules/log/orb_log.cpp"
    "modules/log/orb_log.h"
    # disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) -- FatFS diskio C-ABI callback; the
    # signature is fixed by the FatFS library, not ours to type.
    "modules/log/usb_log.cpp"
    # orb::osal::Task<N> -- FreeRTOS TaskFunction_t's mandated void* arg. Confined to
    # trampoline<T,Method>(void* arg), which immediately casts back to T& and never lets the
    # void* escape the osal layer.
    "modules/osal/task.hpp"
    # orb::osal::Timer -- FreeRTOS pvTimerID void* slot, same trampoline pattern as Task<N>.
    "modules/osal/timer.hpp"
)

is_void_star_allowlisted() {  # <absolute path>
    local path="$1"
    local rel="${path#"${REPO}"/}"
    for allowed in "${VOID_STAR_ALLOWLIST[@]}"; do
        [[ "${rel}" == "${allowed}" ]] && return 0
    done
    return 1
}

while IFS= read -r match; do
    [[ -z "${match}" ]] && continue
    file="${match%%:*}"
    if ! is_void_star_allowlisted "${file}"; then
        echo "BOUNDARY VIOLATION (void*): ${match}"
        found_violations=1
    fi
done < <(grep -rnE --include="*.cpp" --include="*.hpp" --include="*.h" 'void[[:space:]]*\*' \
             "${REPO}/modules" 2>/dev/null | grep -v '/external/' || true)

# --- P7 guard (optional): raw owning-pointer params in service/protocol -----------------------
# SKIPPED. modules/service/ and modules/protocol/ already have plenty of legitimate bare T*
# params today (XboxPacket*, xb_one_drum_input_pkt_t*, uint8_t* out-params like
# adapter_ctx.h's controller(uint8_t*, uint8_t*)) that are borrowed in/out pointers, not owning
# pointers. A grep can't tell "borrowed" from "owning" apart -- any pattern loose enough to
# catch a real owning-pointer regression is also loose enough to flag all of those, and any
# pattern narrow enough to skip them stops catching the thing it was meant to catch. Left out
# per the task's explicit "skip rather than add a noisy guard" guidance.

echo ""
if [[ "${found_violations}" -gt 0 ]]; then
    echo "FAIL: boundary violation(s) found"
    exit 1
fi

echo "PASS: no boundary violations (layering, composition root, forwarders, void*)"
exit 0
