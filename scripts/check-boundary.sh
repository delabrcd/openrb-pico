#!/usr/bin/env bash
# check-boundary.sh — portability boundary enforcement for the service/ and protocol/ layers.
#
# Greps modules/service/ and modules/protocol/ for forbidden SDK/RTOS/TinyUSB includes that
# violate the boundary defined in docs/architecture/modern-cpp.md. Exits non-zero if any are
# found.
#
# EXCLUDED: modules/driver/ (the vendor seam — it is allowed to include TinyUSB / SDK directly).
#
# service/ is fully tusb-free as of the P3 static-DI step: the TinyUSB HID (guitar) and MIDI
# (drums) extern "C" seam callbacks, and every tusb operation the service methods used to call,
# now live in modules/driver/guitar_hid_driver.cpp and modules/driver/drums_midi_seam.cpp.
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

echo ""
if [[ "${found_violations}" -gt 0 ]]; then
    echo "FAIL: boundary violation(s) found in service/ or protocol/"
    exit 1
fi

echo "PASS: no boundary violations in service/ or protocol/"
exit 0
